// Sandboxie-OSS — sbie-cli/server/ServerMain.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// server 生命周期（00-architecture.md §4-§8，M2 server 波次实现）：
//   INIT: 沙箱自检 → 双锁（会话互斥体 + 管道首实例抢占）→ SbieDll →
//         驱动在场 → API_SESSION_LEADER(set) → LogPump → Dispatcher/SvcProxy
//   RUNNING: accept 线程（阻塞 ConnectNamedPipe，交付每 client 工作线程）；
//            主线程 1s 粒度重估空闲窗口
//   EXIT: server.shutdown（同会话校验）或空闲超时 → Stop → 自连带活 accept →
//         排空工作线程（≤10s）→ 回收泵/SvcProxy → 退出 0

#include "ServerMain.h"
#include "Dispatcher.h"
#include "LogPump.h"
#include "ServerState.h"
#include "SvcProxy.h"
#include "../ipcc/SbieIpc.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Util/Json.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>
#include <accctrl.h>
#include <aclapi.h>

#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sbie::server {

namespace {

// ---------------------------------------------------------------------------
// 管道安全属性（00 §3：本会话用户 SID + 本地 SYSTEM 完全访问；双重保险于
// 名称中的 S<session_id>。构建失败回退默认安全（diag）。
// ---------------------------------------------------------------------------

class PipeSecurity {
public:
    PipeSecurity() = default;
    ~PipeSecurity() { Release(); }
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    bool Build()
    {
        // 当前用户 SID（复制出 token 信息生命周期之外）
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
            return false;
        DWORD need = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
        if (need == 0) {
            CloseHandle(tok);
            return false;
        }
        std::vector<BYTE> info(need);
        BOOL ok = GetTokenInformation(tok, TokenUser, info.data(), need, &need);
        CloseHandle(tok);
        if (!ok)
            return false;
        const TOKEN_USER* tu = (const TOKEN_USER*)info.data();
        DWORD sidLen = GetLengthSid(tu->User.Sid);
        userSid_ = LocalAlloc(LMEM_FIXED, sidLen);
        if (!userSid_ || !CopySid(sidLen, userSid_, tu->User.Sid))
            return false;

        // 本地 SYSTEM（S-1-5-18）
        DWORD sysLen = SECURITY_MAX_SID_SIZE;
        systemSid_ = LocalAlloc(LMEM_FIXED, SECURITY_MAX_SID_SIZE);
        if (!systemSid_
            || !CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid_,
                                   &sysLen))
            return false;

        EXPLICIT_ACCESSW ea[2] = {};
        ea[0].grfAccessPermissions = FILE_ALL_ACCESS;
        ea[0].grfAccessMode = SET_ACCESS;
        ea[0].grfInheritance = NO_INHERITANCE;
        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[0].Trustee.ptstrName = (LPWSTR)userSid_;
        ea[1] = ea[0];
        ea[1].Trustee.ptstrName = (LPWSTR)systemSid_;
        if (SetEntriesInAclW(2, ea, nullptr, &acl_) != ERROR_SUCCESS)
            return false;

        sd_ = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
        if (!sd_
            || !InitializeSecurityDescriptor(sd_, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(sd_, TRUE, acl_, FALSE))
            return false;
        sa_.nLength = sizeof(sa_);
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
        return true;
    }

    bool Valid() const { return sd_ != nullptr; }
    SECURITY_ATTRIBUTES* Sa() { return &sa_; }

private:
    void Release()
    {
        if (sd_) LocalFree(sd_);
        if (acl_) LocalFree(acl_);
        if (systemSid_) LocalFree(systemSid_);
        if (userSid_) LocalFree(userSid_);
        sd_ = nullptr;
        acl_ = nullptr;
        systemSid_ = nullptr;
        userSid_ = nullptr;
    }

    SECURITY_ATTRIBUTES sa_{};
    LPVOID sd_ = nullptr;
    PACL acl_ = nullptr;
    LPVOID userSid_ = nullptr;
    LPVOID systemSid_ = nullptr;
};

// ---------------------------------------------------------------------------
// 管道实例
// ---------------------------------------------------------------------------

HANDLE CreatePipeInstance(bool first, SECURITY_ATTRIBUTES* sa)
{
    DWORD openMode = PIPE_ACCESS_DUPLEX;
    if (first)
        openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE; // 00 §4：抢占式防劫持
    return CreateNamedPipeW(
        ipc::PipeName().c_str(), openMode,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        (DWORD)ipc::kMaxPipeInstances,
        65536, 65536, 0,
        sa);
}

// ---------------------------------------------------------------------------
// 回复 envelope：{"ok":true,"data":…} /
//   {"ok":false,"error":{code,message[,ntstatus][,exit]}}
//
// 错误信封（server 写路径波次，04 §12）：业务码与传输码分离——
//   * "code" = SbieStatus 业务码**原样**（0..9 语义码；100+ 内部码）。此前
//     code 经 ToExitCode 折叠（103→1），client 只能靠消息标记嗅探占位桩，
//     且 100+ 码到达 client 前已丢语义。原样下发后 client IpcRoute 按
//     code==103 精确识别"未实现→降级"；
//   * "exit" = ToExitCode 折叠后的退出码语义（向后兼容字段：旧 client 把
//     code 当退出码用时 0..9 两字段恒等，100+ 的折叠结果也与 client 侧
//     EmitError→ToExitCode 一致，行为不变）；
//   * 传输层失败（server 死/管道断裂）不产生回复帧，client 自行合成
//     4/8——传输码从不到达信封。
// ---------------------------------------------------------------------------

std::string BuildReplyPayload(const OpResult& r)
{
    json::JsonValue top = json::JsonValue::Object();
    if (r.ok) {
        top.set(L"ok", json::JsonValue(true));
        top.set(L"data", r.data);
        return json::SerializeUtf8(top);
    }
    top.set(L"ok", json::JsonValue(false));
    json::JsonValue err = json::JsonValue::Object();
    err.set(L"code", json::JsonValue((long long)r.status));
    err.set(L"exit", json::JsonValue((long long)ToExitCode(r.status)));
    err.set(L"message", json::JsonValue(r.message));
    if (r.ntstatus) {
        wchar_t hex[16];
        swprintf_s(hex, L"0x%08lX", r.ntstatus);
        err.set(L"ntstatus", json::JsonValue(hex));
    }
    top.set(L"error", err);
    return json::SerializeUtf8(top);
}

// ---------------------------------------------------------------------------
// 工作线程：一连接一线程，帧往返（00 §6 RUNNING）。
// 订阅 log.watch 后切换轮询模式（PeekNamedPipe + pushQueue 排空）——同步
// 管道句柄不支持跨线程并发读写（泵线程直写会 ERROR_NO_DATA，实测坑），
// 一切读/写收敛在本线程。
// ---------------------------------------------------------------------------

bool ServeOneRequest(const std::shared_ptr<Connection>& conn,
                     const ipc::FrameHeader& hdr,
                     const std::vector<uint8_t>& payload)
{
    OpResult r;
    json::JsonValue root;
    SbieStatus jerr = SbieStatus::OK;
    std::string_view text((const char*)payload.data(), payload.size());
    if (!json::Parse(text, &root, &jerr) || !root.isObject()) {
        r = OpResult::Fail(SbieStatus::ERR_JSON, L"malformed request payload");
    } else {
        const json::JsonValue* opv = root.find(L"op");
        if (!opv || !opv->isString()) {
            r = OpResult::Fail(SbieStatus::ERR_JSON, L"missing 'op' string");
        } else {
            const std::string op = util::WideToUtf8(opv->asString());
            const json::JsonValue* pp = root.find(L"params");
            json::JsonValue params = (pp && pp->isObject())
                                         ? *pp
                                         : json::JsonValue::Object();
            if (op == ipc::kOpServerShutdown) {
                // 00 §6：仅同用户（管道 DACL 已限）同会话
                if (conn->clientSession != ServerState::Get().Session()) {
                    r = OpResult::Fail(SbieStatus::ACCESS_DENIED,
                                       L"shutdown denied: session mismatch");
                } else {
                    ServerState::Get().Stop();
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"stopping", json::JsonValue(true));
                    r = OpResult::Succeed(std::move(d));
                }
            } else {
                r = Dispatch(op, params, conn);
            }
        }
    }
    const std::string reply = BuildReplyPayload(r);
    return ipc::WriteFrame(conn->pipe,
                           hdr.msgid | (r.ok ? 0u : ipc::kMsgIdErrorFlag),
                           reply.data(), (uint32_t)reply.size());
}

void WorkerMain(std::shared_ptr<Connection> conn)
{
    for (;;) {
        if (ServerState::Get().Stopping())
            break;

        if (!conn->logSubscriber) {
            // 常规模式：阻塞读（无推送目标，单线程读写天然安全）
            ipc::FrameHeader hdr;
            std::vector<uint8_t> payload;
            if (!ipc::ReadFrame(conn->pipe, &hdr, &payload))
                break;
            if (!ServeOneRequest(conn, hdr, payload))
                break;
            continue;
        }

        // 订阅者轮询模式：10ms 粒度查新请求 + 排空推送队列
        DWORD avail = 0;
        if (!PeekNamedPipe(conn->pipe, nullptr, 0, nullptr, &avail, nullptr))
            break; // 断开
        if (avail > 0) {
            ipc::FrameHeader hdr;
            std::vector<uint8_t> payload;
            if (!ipc::ReadFrame(conn->pipe, &hdr, &payload))
                break;
            if (!ServeOneRequest(conn, hdr, payload))
                break;
        }
        std::vector<std::string> pushes;
        conn->DrainPushes(&pushes);
        bool broken = false;
        for (auto& p : pushes) {
            if (!ipc::WriteFrame(conn->pipe, 0 /*server 推送 msgid=0*/,
                                 p.data(), (uint32_t)p.size())) {
                broken = true;
                break;
            }
        }
        if (broken)
            break;
        Sleep(10);
    }

    FlushFileBuffers(conn->pipe); // 尽力保证末条回复落达客户端
    DisconnectNamedPipe(conn->pipe);
    CloseHandle(conn->pipe);
    conn->pipe = nullptr;
    ServerState::Get().Detach(conn); // 计数-- / 摘订阅 / 重武装空闲计时
}

// ---------------------------------------------------------------------------
// accept 线程（00 §8）。pending 实例所有权转入本函数。
// ---------------------------------------------------------------------------

void AcceptLoop(SECURITY_ATTRIBUTES* sa, HANDLE pending)
{
    for (;;) {
        if (ServerState::Get().Stopping())
            break;

        if (!pending) {
            pending = CreatePipeInstance(false, sa);
            if (!pending) {
                if (ServerState::Get().Stopping())
                    break;
                Sleep(50); // 实例数达上限（16）：等工作线程释放
                continue;
            }
        }

        BOOL connected = ConnectNamedPipe(pending, nullptr);
        DWORD err = GetLastError();
        if (ServerState::Get().Stopping()) {
            if (connected || err == ERROR_PIPE_CONNECTED)
                DisconnectNamedPipe(pending);
            CloseHandle(pending);
            break;
        }
        if (!connected && err == ERROR_NO_DATA) {
            // client 连上又立刻断开（探测/唤醒）：重用循环
            DisconnectNamedPipe(pending);
            CloseHandle(pending);
            pending = nullptr;
            continue;
        }
        if (!connected && err != ERROR_PIPE_CONNECTED) {
            CloseHandle(pending);
            pending = nullptr;
            Sleep(20);
            continue;
        }

        auto conn = std::make_shared<Connection>(pending);
        ULONG pid = 0, sid = 0;
        if (GetNamedPipeClientProcessId(pending, &pid))
            conn->clientPid = pid;
        if (GetNamedPipeClientSessionId(pending, &sid))
            conn->clientSession = sid;
        ServerState::Get().Attach(conn);
        std::thread(WorkerMain, std::move(conn)).detach();
        pending = nullptr; // 实例已归工作线程
    }
    if (pending) { // 不可达防御（break 前均已处理）
        DisconnectNamedPipe(pending);
        CloseHandle(pending);
    }
}

// 唤醒可能阻塞在 ConnectNamedPipe 的 accept 线程：自连即断（00 §6 停机路径）
void WakeAcceptLoop()
{
    const std::wstring name = ipc::PipeName();
    for (int i = 0; i < 20; ++i) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h); // accept 线程已被唤醒（其 Stop 检查会清理该实例）
            return;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return; // 无监听实例：accept 线程不在 ConnectNamedPipe 中等待
        if (e == ERROR_PIPE_BUSY)
            WaitNamedPipeW(name.c_str(), 100);
        else
            Sleep(25);
    }
    // 兜底（理论不可达）：accept 线程无响应——直接终结，避免悬挂。
    // 工作线程已在前置排空阶段收尾。
    ExitProcess(0);
}

// 主线程等待粒度：≤1s（周期重估空闲窗口——最后一个客户端断开时无需额外
// 唤醒事件）
DWORD ComputeWaitMs()
{
    if (ServerState::Get().IdleTimeoutSec() == 0)
        return 1000; // 永不空闲退出
    ULONGLONG rem = ServerState::Get().IdleRemainingMs();
    if (rem == ~0ULL)
        return 1000; // 有活动客户端
    if (rem > 1000)
        return 1000;
    return (DWORD)rem;
}

void DiagServer(const char* line)
{
    util::PrintErrLineUtf8(line);
}

} // namespace

int RunServer(const ServerOptions& options)
{
    // 0) 沙箱自检（02 §6 / 00 §9.1：server 绝不能在沙箱内，否则 leader set
    //    路径 STATUS_NOT_IMPLEMENTED）
    if (drv::LoadSbieDll() && drv::InSandbox()) {
        DiagServer("sbie-cli server: must not run inside a sandbox");
        return (int)SbieStatus::ACCESS_DENIED;
    }

    // 1) 单实例锁 1/2：会话互斥体（00 §4；Local\ = 每会话命名空间）
    HANDLE mutex = CreateMutexW(nullptr, FALSE, ipc::MutexName().c_str());
    if (!mutex)
        return (int)SbieStatus::GENERIC;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex); // 已有实例：静默退出 0（竞态收敛点）
        return 0;
    }

    const ULONG session = ipc::CurrentSessionId();
    if (!ServerState::Get().Initialize(session, options.idleTimeoutSec)) {
        CloseHandle(mutex);
        return (int)SbieStatus::GENERIC;
    }

    // 2) SbieDll → 驱动在场 → API_SESSION_LEADER(set) → LogPump（00 §6 INIT）
    bool leaderTaken = false;
    if (drv::Loaded() || drv::LoadSbieDll()) {
        if (drv::DriverAlive()) {
            // set 路径（02 §3.5/§7.2）：ProcessId==NULL，封装内部忽略
            // session_id；失败=他者已是 leader（如 SandMan）或权限问题
            LONG rc = drv::ApiP()->SbieApi_SessionLeader(session, nullptr);
            if (rc == 0) {
                leaderTaken = true;
            } else {
                char buf[160];
                snprintf(buf, std::size(buf),
                         "sbie-cli server: session leader not taken"
                         " (rc=0x%08lX) - log pump disabled",
                         (unsigned long)rc);
                DiagServer(buf);
            }
        } else {
            DiagServer("sbie-cli server: driver not running - log pump"
                       " disabled, read ops still served");
        }
    } else {
        DiagServer("sbie-cli server: SbieDll.dll not loaded - serving"
                   " degraded status only");
    }
    if (leaderTaken) {
        if (!StartLogPump())
            DiagServer("sbie-cli server: log pump failed to start");
        // interactive queue 聚合（03 §6；SbieSvc 缺席时泵内部 5s 退避重试，
        // 不影响 server 存活）
        if (!StartInteractivePump())
            DiagServer("sbie-cli server: interactive queue pump failed"
                       " to start");
    }

    // 3) 分发表 + SbieSvc 专职线程
    RegisterBuiltinOps();
    StartSvcProxy();

    // 4) 单实例锁 2/2：管道首实例（FILE_FLAG_FIRST_PIPE_INSTANCE 抢占）
    PipeSecurity sec;
    if (!sec.Build())
        DiagServer("sbie-cli server: pipe DACL build failed - using default"
                   " security");
    SECURITY_ATTRIBUTES* sa = sec.Valid() ? sec.Sa() : nullptr;
    HANDLE first = CreatePipeInstance(true, sa);
    if (!first) {
        const DWORD e = GetLastError();
        StopInteractivePump();
        StopLogPump();
        StopSvcProxy();
        CloseHandle(mutex);
        if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY)
            return 0; // 同名管道已被持有：已有 server，静默退出 0
        char buf[128];
        snprintf(buf, std::size(buf),
                 "sbie-cli server: CreateNamedPipe failed (%lu)", 
                 (unsigned long)e);
        DiagServer(buf);
        return (int)SbieStatus::GENERIC;
    }

    {
        char buf[160];
        snprintf(buf, std::size(buf),
                 "sbie-cli server: ready (session %lu, pid %lu, idle %lus,"
                 " leader %d)",
                 (unsigned long)session, (unsigned long)GetCurrentProcessId(),
                 (unsigned long)options.idleTimeoutSec, leaderTaken ? 1 : 0);
        DiagServer(buf);
    }

    // 5) accept 线程（detach：停机唤醒经自连；进程退出收尾）
    std::thread acceptor(AcceptLoop, sa, first);
    first = nullptr;
    acceptor.detach();

    // 6) 主线程：空闲计时/停机等待（00 §6）
    for (;;) {
        const DWORD w = WaitForSingleObject(ServerState::Get().StopEvent(),
                                            ComputeWaitMs());
        if (w == WAIT_OBJECT_0)
            break;
        // WAIT_TIMEOUT：1s 重估点
        if (ServerState::Get().IdleTimeoutSec() != 0
            && ServerState::Get().ActiveClients() == 0
            && ServerState::Get().IdleRemainingMs() == 0)
            break; // 空闲到期（有新客户端竞态连入则本轮不触发）
    }
    ServerState::Get().Stop(); // 幂等（shutdown op 路径已置位）

    // 7) 唤醒 accept + 排空工作线程（≤10s，00 §6 优雅停机）
    WakeAcceptLoop();
    {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (ServerState::Get().ActiveClients() > 0
               && GetTickCount64() < deadline)
            Sleep(25);
    }
    Sleep(100); // accept 线程收尾余量

    // 8) 回收（泵/SvcProxy 以停机事件/quit 标志快速退出）
    StopInteractivePump();
    StopLogPump();
    StopSvcProxy();
    CloseHandle(mutex);
    return 0;
}

} // namespace sbie::server
