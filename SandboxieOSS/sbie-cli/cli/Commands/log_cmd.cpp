// Sandboxie-OSS — sbie-cli/cli/Commands/log_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie log watch|dump|messages（04-modules.md §4.7）。
// 接线波次：server 在场时两命令均走 IPC——dump 取 log.dump（server 环形
// 缓冲）；watch 在专用连接上订阅 log.watch 后消费 server 推送的
// {"op":"log.event"} 帧（msgid=0，PeekNamedPipe 轮询保证 Ctrl+C 可退）。
// server 写路径波次（04 §12）：server 推送/dump 的 text 已经
// SbieDll_FormatMessage*（SbieMsg.dll 消息表）格式化——与直连文案一致；
// interactive queue 事件由 server 聚合推送（data.interactive=true，自动
// 应答=拒绝）；--interactive 的 y/N 决策仍需直连模式（无人值守策略边界）。
// server 缺席（--no-server / 拉起失败）降级为 watch 进程自行接管会话
// leader 的直连双源实现（驱动日志泵 + interactive queue）。
// Ctrl+C 退出码 0。dump/messages = 拉取环形缓冲近期消息后退出。

#include "CfgTemplateCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/DriverApi/DriverApi.h"
#include "../../../SbieCore/QueueClient/QueueClient.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <deque>
#include <iterator>
#include <string>
#include <vector>

namespace sbie::cli {

namespace {

constexpr LONG kStatusAlreadyAttached = 0xC0000038L; // STATUS_DEVICE_ALREADY_ATTACHED

volatile LONG g_stop = 0;

BOOL WINAPI LogCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT
        || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

std::wstring NowHms()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

struct LogEntry {
    ULONG msgCode = 0;
    ULONG pid = 0;
    std::vector<std::wstring> ins;
};

// 驱动日志泵取一条（游标 *cursor 进出；false = 无更多条目/错误）
bool FetchLogEntry(ULONG sessionId, ULONG* cursor, LogEntry* e)
{
    WCHAR buf[4096];
    ULONG msgNum = *cursor;
    ULONG msgId = 0;
    ULONG pid = 0;
    LONG rc = drv::ApiP()->SbieApi_GetMessage(&msgNum, sessionId, &msgId, &pid,
                                              buf, (ULONG)sizeof(buf));
    if (rc != 0)
        return false;   // STATUS_NO_MORE_ENTRIES / ACCESS_DENIED（非 leader）
    *cursor = msgNum;

    e->msgCode = msgId;
    e->pid = pid;
    e->ins.clear();
    // 记录体 = 多段 \0 分隔 WCHAR 串（首段为空占位；QSbieAPI GetLog 同构）
    const WCHAR* p = buf;
    size_t used = 0;
    const size_t cap = std::size(buf);
    while (used < cap) {
        size_t len = 0;
        while (used + len < cap && p[len])
            ++len;
        if (len == 0)
            break;   // 空段 = 结束
        e->ins.emplace_back(p, len);
        p += len + 1;
        used += len + 1;
    }
    return true;
}

std::wstring JoinInserts(const LogEntry& e)
{
    std::wstring s;
    for (const auto& i : e.ins) {
        if (!s.empty())
            s += L' ';
        s += i;
    }
    return s;
}

// 文案：SbieDll_FormatMessage0/1/2（SbieMsg.dll 消息表）。表键 = 完整 msgCode
// （含严重度/设施位，如 1399 = 0x41020577——实机 .rsrc 遍历核实；低 16 位查
// 不到）。注意 1399 的文案按设计就是 "%0"（无输出：进程启动通知供程序消费，
// msgs\Sbie-English-1033.txt:281）——FormatMessage 返回 0 时回退为插入串直拼
// （QSbieAPI GetLog 对 1399/2199 也走专门分支，SbieAPI.cpp:2497-2522）。
std::wstring FormatText(drv::Api* api, const LogEntry& e, bool raw)
{
    if (raw || !api || !api->SbieDll_FormatMessage0)
        return JoinInserts(e);
    WCHAR* p = nullptr;
    if (e.ins.size() >= 2)
        p = api->SbieDll_FormatMessage2(e.msgCode, e.ins[0].c_str(),
                                        e.ins[1].c_str());
    else if (e.ins.size() == 1)
        p = api->SbieDll_FormatMessage1(e.msgCode, e.ins[0].c_str());
    else
        p = api->SbieDll_FormatMessage0(e.msgCode);
    std::wstring s;
    if (p) {
        s = p;
        LocalFree(p);   // FormatMessage ALLOCATE_BUFFER / LocalAlloc 同族
    }
    if (s.empty() || s.rfind(L"err=", 0) == 0) {
        s = JoinInserts(e);   // "%0" 消息/无表项：插入串直拼（仍有信息量）
    } else {
        // 消息表文案自带 "SBIE%04u " 前缀（msgs 生成约定）——行首已有同款
        // 前缀，剥去避免重复
        wchar_t want[10];
        swprintf_s(want, L"SBIE%04u ", e.msgCode & 0xFFFF);
        if (s.rfind(want, 0) == 0)
            s.erase(0, wcslen(want));
    }
    return s;
}

struct WatchFilter {
    ULONG pid = 0;    // 0 = 不过滤
    ULONG msgId = 0;  // 低 16 位；0 = 不过滤
    bool raw = false;
    bool interactive = false;
};

bool PassesFilter(const WatchFilter& f, const LogEntry& e)
{
    if (f.pid && e.pid != f.pid)
        return false;
    if (f.msgId && (e.msgCode & 0xFFFF) != f.msgId)
        return false;
    return true;
}

void EmitLogLine(const GlobalOptions& opts, const WatchFilter& f,
                 drv::Api* api, const LogEntry& e)
{
    std::wstring text = FormatText(api, e, f.raw);
    if (opts.json) {
        // watch 的 --json：每行一个 JSON 对象（NDJSON，便于流式消费）
        json::JsonValue o = json::JsonValue::Object();
        o.set(L"time", json::JsonValue(NowHms()));
        o.set(L"code", json::JsonValue((long long)e.msgCode));
        o.set(L"id", json::JsonValue((long long)(e.msgCode & 0xFFFF)));
        o.set(L"pid", json::JsonValue((long long)e.pid));
        o.set(L"text", json::JsonValue(text));
        util::PrintLineUtf8(json::SerializeUtf8(o));
    } else {
        wchar_t buf[32];
        swprintf_s(buf, L"SBIE%04u", e.msgCode & 0xFFFF);
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + NowHms() + L"] " + buf + L" " + std::to_wstring(e.pid)
            + L" " + text));
    }
    fflush(stdout);   // 流式命令：管道/文件重定向下逐行可见
}

// 接管本会话 leader（02 §3.5 set 路径；被占时返回当前 leader pid 供诊断）
LONG TakeSessionLeadership(ULONG sessionId, ULONG* currentLeader)
{
    drv::Api* api = drv::ApiP();
    LONG rc = api->SbieApi_SessionLeader(0, nullptr);
    if (rc == 0)
        return 0;
    if (rc == kStatusAlreadyAttached) {
        HANDLE pid = nullptr;
        api->SbieApi_SessionLeader(sessionId, &pid);
        if (currentLeader)
            *currentLeader = (ULONG)(ULONG_PTR)pid;
    }
    return rc;
}

struct LogCommonOptions {
    ULONG sessionId = 0;
    drv::Api* api = nullptr;
};

// 公共前置：dll/驱动/leader。返回 0 继续；非 0 = 已输出的退出码。
int LogPrologue(const GlobalOptions& o, LogCommonOptions* out)
{
    if (!drv::LoadSbieDll(o.sbieDllPath))
        return EmitError(o, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!drv::DriverAlive())
        return EmitError(o, SbieStatus::DRIVER_UNAVAILABLE, L"driver not running");
    ULONG sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    ULONG leader = 0;
    LONG rc = TakeSessionLeadership(sid, &leader);
    if (rc == kStatusAlreadyAttached)
        return EmitError(o, SbieStatus::ACCESS_DENIED,
                         L"session leader is pid " + std::to_wstring(leader)
                             + L" (SandMan?); stop it or use the sbie-cli "
                               L"server log watch");
    if (rc != 0)
        return EmitError(o, FromNtStatus(rc),
                         L"SbieApi_SessionLeader(set) failed");
    out->sessionId = sid;
    out->api = drv::ApiP();
    return 0;
}

// 人类可读的字节数（与 box size 约定一致的同款简化）
std::wstring HumanSize(unsigned long long bytes)
{
    const wchar_t* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0)
        swprintf_s(buf, L"%llu B", bytes);
    else
        swprintf_s(buf, L"%.1f %ls", v, unit[u]);
    return buf;
}

// ---- server 推送事件（log.event data = {msg_num,msgid,pid,time,text}；
//      interactive 聚合事件再加 {interactive,kind,file_path,file_size}，
//      server 写路径波次 04 §12） ----

struct ServerLogEvent {
    ULONG msgid = 0, pid = 0;
    std::wstring time, text;
    bool interactive = false;
    int kind = 0;
    std::wstring filePath;
    unsigned long long fileSize = 0;
    bool Parse(const json::JsonValue& d)
    {
        if (!d.isObject())
            return false;
        const json::JsonValue* v = d.find(L"msgid");
        if (v && v->isInt())
            msgid = (ULONG)v->asInt();
        v = d.find(L"pid");
        if (v && v->isInt())
            pid = (ULONG)v->asInt();
        v = d.find(L"time");
        if (v && v->isString())
            time = v->asString();
        v = d.find(L"text");
        if (v && v->isString())
            text = v->asString();
        v = d.find(L"interactive");
        if (v && v->type() == json::JsonValue::Type::Bool)
            interactive = v->asBool();
        v = d.find(L"kind");
        if (v && v->isInt())
            kind = (int)v->asInt();
        v = d.find(L"file_path");
        if (v && v->isString())
            filePath = v->asString();
        v = d.find(L"file_size");
        if (v && v->isInt())
            fileSize = (unsigned long long)v->asInt();
        return true;
    }
};

void EmitServerEvent(const GlobalOptions& o, const WatchFilter& f,
                     const ServerLogEvent& e)
{
    if (f.pid && e.pid != f.pid)
        return;
    if (f.msgId && (e.msgid & 0xFFFF) != f.msgId)
        return;
    const std::wstring time = e.time.empty() ? NowHms() : e.time;
    if (e.interactive) {
        // interactive 聚合事件（server 泵已按默认无人值守策略自动应答，
        // text 尾部带 "(auto-denied)"）：与直连模式的行形态一致
        if (o.json) {
            json::JsonValue j = json::JsonValue::Object();
            j.set(L"time", json::JsonValue(time));
            j.set(L"interactive", json::JsonValue(true));
            j.set(L"kind", json::JsonValue((long long)e.kind));
            j.set(L"pid", json::JsonValue((long long)e.pid));
            if (e.kind == 1 /*MAN_FILE_MIGRATION*/) {
                j.set(L"file_path", json::JsonValue(e.filePath));
                j.set(L"file_size", json::JsonValue((long long)e.fileSize));
            }
            j.set(L"text", json::JsonValue(e.text));
            util::PrintLineUtf8(json::SerializeUtf8(j));
        } else {
            util::PrintLineUtf8(util::WideToUtf8(L"[" + time + L"] " + e.text));
        }
        fflush(stdout);
        return;
    }
    if (o.json) {
        json::JsonValue j = json::JsonValue::Object();
        j.set(L"time", json::JsonValue(time));
        j.set(L"code", json::JsonValue((long long)e.msgid));
        j.set(L"id", json::JsonValue((long long)(e.msgid & 0xFFFF)));
        j.set(L"pid", json::JsonValue((long long)e.pid));
        j.set(L"text", json::JsonValue(e.text));
        util::PrintLineUtf8(json::SerializeUtf8(j));
    } else {
        wchar_t buf[32];
        swprintf_s(buf, L"SBIE%04u", e.msgid & 0xFFFF);
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + time + L"] " + buf + L" " + std::to_wstring(e.pid)
            + L" " + e.text));
    }
    fflush(stdout);
}

// watch 的 server 订阅路径：专用连接（srvconn 缓存连接不读推送帧，不能复
// 用），RoundTrip(log.watch) 收 ok 应答后轮询消费 msgid=0 的推送帧。
// 返回 0 = 已完成（退出 0）；-1 = 订阅未成立，降级直连；>0 = 退出码。
// server 双源（04 §12）：驱动日志（SbieMsg.dll 格式化文案）+ interactive
// queue 聚合事件（data.interactive=true；server 按无人值守策略自动应答）。
int WatchViaServer(const GlobalOptions& o, const WatchFilter& f)
{
    if (!srvconn::HasServer()) {
        if (o.showTransport)
            Diag(L"transport: direct (server not connected)");
        return -1;
    }

    ipc::PipeClient pipe;
    if (!pipe.Open(1000)) {
        if (o.showTransport)
            Diag(L"transport: direct (server probe failed)");
        return -1;   // 探测窗口内 server 消失：按直连处理
    }
    std::string reply;
    if (!pipe.RoundTrip(ipc::kOpLogWatch, "{}", &reply)) {
        if (o.showTransport)
            Diag(L"transport: direct (subscribe failed)");
        return -1;
    }
    json::JsonValue env;
    SbieStatus jerr = SbieStatus::OK;
    if (!json::Parse(reply, &env, &jerr) || !env.isObject()) {
        if (o.showTransport)
            Diag(L"transport: direct (malformed subscribe reply)");
        return -1;
    }
    const json::JsonValue* okf = env.find(L"ok");
    if (!okf || !okf->asBool()) {
        // 订阅被拒（典型：log pump 未激活，如 leader 被 GUI 持有）——
        // 交回直连路径给出更精确诊断（直连接管 leader 同样会失败并指明
        // 占用者 pid）
        if (o.showTransport)
            Diag(L"transport: direct (subscription rejected)");
        return -1;
    }
    if (o.showTransport)
        Diag(L"transport: ipc (log subscription)");

    if (f.interactive)
        Diag(L"interactive decisions are auto-denied by the server"
             L" (unattended policy); use --no-server for y/N prompting");
    if (!o.json && !o.quiet)
        Diag(L"watching server log events (Ctrl+C to stop)");
    SetConsoleCtrlHandler(LogCtrlHandler, TRUE);

    bool connectionLost = false;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe.Handle(), nullptr, 0, nullptr, &avail,
                           nullptr)) {
            connectionLost = true;   // server 死/管道断
            break;
        }
        if (avail == 0) {
            Sleep(100);
            continue;
        }
        ipc::FrameHeader hdr;
        std::vector<uint8_t> payload;
        if (!ipc::ReadFrame(pipe.Handle(), &hdr, &payload)) {
            connectionLost = true;
            break;
        }
        if (hdr.msgid != 0)
            continue;   // 非 0 = 对某请求的应答（本连接不再发请求，忽略）
        json::JsonValue push;
        if (!json::Parse(std::string((const char*)payload.data(),
                                     payload.size()),
                         &push, &jerr)
            || !push.isObject())
            continue;
        const json::JsonValue* opf = push.find(L"op");
        if (!opf || !opf->isString() || opf->asString() != L"log.event")
            continue;
        ServerLogEvent e;
        if (const json::JsonValue* d = push.find(L"data");
            d && e.Parse(*d)) {
            EmitServerEvent(o, f, e);
        }
    }

    SetConsoleCtrlHandler(LogCtrlHandler, FALSE);
    if (connectionLost) {
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"server connection lost");
    }
    return 0;   // Ctrl+C = 0（04 §4.7）
}

} // namespace

// ---------------------------------------------------------------------------
// sbie log watch（04 §4.7；订阅：驱动日志泵 + interactive queue 双源）
// ---------------------------------------------------------------------------

int CmdLogWatch(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    WatchFilter f;
    for (size_t i = 0; i < rest.size(); ++i) {
        const std::wstring& a = rest[i];
        if (a == L"--pid" && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            f.pid = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0')
                return EmitError(o, SbieStatus::USAGE, L"invalid --pid");
        } else if (a == L"--msg" && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            f.msgId = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0')
                return EmitError(o, SbieStatus::USAGE, L"invalid --msg");
        } else if (a == L"--raw") {
            f.raw = true;
        } else if (a == L"--interactive") {
            f.interactive = true;
        } else if (a == L"--follow") {
            // 默认即跟随（04 §4.7）；接受显式旗标
        } else {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        }
    }

    // IPC 优先：server 订阅推送（server 持会话 leader）；server 缺席或订阅
    // 被拒时降级为本进程接管 leader 的直连双源实现
    {
        int rc = WatchViaServer(o, f);
        if (rc >= 0)
            return rc;
    }

    LogCommonOptions lc;
    int rc = LogPrologue(o, &lc);
    if (rc != 0)
        return rc;

    // watch 只看新事件：游标快进抽干既有 backlog（backlog 输出属 log dump）
    ULONG cursor = 0;
    LogEntry e;
    while (FetchLogEntry(lc.sessionId, &cursor, &e)) {
    }

    // 第二源：本会话 interactive queue（尽力而为；SbieSvc 缺席时仅日志源）
    queue::InteractiveSession iq;
    if (!Ok(iq.Start()))
        Diag(L"interactive queue unavailable (" + queue::InteractiveQueueName()
             + L"); watching driver log only");

    HANDLE hStdin = nullptr;
    if (f.interactive) {
        hStdin = GetStdHandle(STD_INPUT_HANDLE);
        if (!hStdin || hStdin == INVALID_HANDLE_VALUE)
            hStdin = nullptr;
    }

    SetConsoleCtrlHandler(LogCtrlHandler, TRUE);
    if (!o.json && !o.quiet)
        Diag(L"watching session " + std::to_wstring(lc.sessionId)
             + L" (Ctrl+C to stop)");

    queue::InteractiveSink sink =
        [&](const queue::InteractiveRequest& ir, queue::InteractiveReply* rpl) {
            const int kindFileMigration = 1;   // MAN_FILE_MIGRATION
            std::wstring line;
            if (ir.kind == kindFileMigration)
                line = L"interactive: file migration request: " + ir.filePath
                     + L" (" + HumanSize(ir.fileSize) + L") from pid "
                     + std::to_wstring(ir.clientPid);
            else if (ir.kind == 2 /*MAN_INET_BLOCKADE*/)
                line = L"interactive: internet blockade notification from pid "
                     + std::to_wstring(ir.clientPid);
            else
                line = L"interactive: unknown request kind "
                     + std::to_wstring(ir.kind);

            // 1) 呈现请求（json=请求对象行；人类=行文本，待决策时附提示）
            if (o.json) {
                json::JsonValue jd = json::JsonValue::Object();
                jd.set(L"time", json::JsonValue(NowHms()));
                jd.set(L"interactive", json::JsonValue(true));
                jd.set(L"kind", json::JsonValue((long long)ir.kind));
                jd.set(L"pid", json::JsonValue((long long)ir.clientPid));
                if (ir.kind == kindFileMigration) {
                    jd.set(L"file_path", json::JsonValue(ir.filePath));
                    jd.set(L"file_size",
                           json::JsonValue((long long)ir.fileSize));
                }
                util::PrintLineUtf8(json::SerializeUtf8(jd));
            } else if (f.interactive && ir.kind == kindFileMigration && hStdin) {
                util::PrintUtf8(util::WideToUtf8(
                    L"[" + NowHms() + L"] " + line + L" -- allow? [y/N] "));
                fflush(stdout);
            } else {
                util::PrintLineUtf8(util::WideToUtf8(
                    L"[" + NowHms() + L"] " + line + L" (auto-denied)"));
            }

            // 2) 决策：默认拒绝（03 §6 默认无人值守策略；file_copy.c
            //    retval!=0 才继续拷入沙箱）；--interactive 时读 stdin y/N
            rpl->status = 0;
            rpl->retval = 0;
            if (f.interactive && ir.kind == kindFileMigration && hStdin) {
                wchar_t ans[8] = L"";
                DWORD n = 0;
                // 控制台行读（默认 cooked 模式，回车提交；Ctrl+C 由控制处理器
                // 置停后本调用返回 0，按拒绝处理）
                if (ReadConsoleW(hStdin, ans, 4, &n, nullptr) && n
                    && (ans[0] == L'y' || ans[0] == L'Y'))
                    rpl->retval = 1;
            }

            // 3) json 模式补一行决策结果
            if (o.json) {
                json::JsonValue jd = json::JsonValue::Object();
                jd.set(L"time", json::JsonValue(NowHms()));
                jd.set(L"interactive", json::JsonValue(true));
                jd.set(L"kind", json::JsonValue((long long)ir.kind));
                jd.set(L"pid", json::JsonValue((long long)ir.clientPid));
                jd.set(L"allowed", json::JsonValue(rpl->retval != 0));
                util::PrintLineUtf8(json::SerializeUtf8(jd));
            }
            return true;
        };

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        HANDLE hs[2];
        DWORD nh = 0;
        if (iq.active())
            hs[nh++] = iq.event();
        if (hStdin)
            hs[nh++] = hStdin;
        DWORD w = MsgWaitForMultipleObjects(nh, hs, FALSE, 200, 0);

        // 源 1：驱动日志泵（无事件句柄，逐轮拉取）
        while (FetchLogEntry(lc.sessionId, &cursor, &e)) {
            if (PassesFilter(f, e))
                EmitLogLine(o, f, lc.api, e);
        }

        // 源 2：interactive queue（事件置位才抽干；传输断线则退避重建）
        if (iq.active() && nh >= 1 && w == WAIT_OBJECT_0) {
            SbieStatus st = iq.Drain(sink);
            if (!Ok(st)) {
                Diag(L"interactive queue error ("
                     + std::wstring(StatusName(st))
                     + L"); reconnecting");
                if (!Ok(queue::Reconnect(iq, 500)))
                    Diag(L"interactive queue reconnect failed; "
                         L"continuing log-only");
            }
        }
        fflush(stdout);   // 双源输出统一冲刷（流式可见性）
    }

    SetConsoleCtrlHandler(LogCtrlHandler, FALSE);
    iq.Reset();
    return 0;   // Ctrl+C = 0（04 §4.7）
}

// ---------------------------------------------------------------------------
// sbie log dump / log messages（04 §4.7；[--last <n>=100] 取环形缓冲近期消息）
// ---------------------------------------------------------------------------

int CmdLogDump(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    ULONG last = 100;
    for (size_t i = 0; i < rest.size(); ++i) {
        const std::wstring& a = rest[i];
        if (a == L"--last" && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            last = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0' || last == 0)
                return EmitError(o, SbieStatus::USAGE, L"invalid --last value");
        } else if (a == L"--raw" || a == L"--pid" || a == L"--msg") {
            return EmitError(o, SbieStatus::USAGE,
                             L"filters belong to log watch; dump takes only "
                             L"--last");
        } else {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        }
    }

    // IPC 优先（log.dump：server 环形缓冲，server 持 leader）。server 侧
    // text 经 SbieDll_FormatMessage*（SbieMsg.dll 消息表）格式化——与直连
    // 输出文案一致（04 §12；§11 遗留 4 已消除）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"last", (long long)last);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpLogDump, params, true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                std::vector<ServerLogEvent> events;
                if (data.isArray()) {
                    for (const json::JsonValue& e : data.items()) {
                        ServerLogEvent ev;
                        if (ev.Parse(e))
                            events.push_back(std::move(ev));
                    }
                }
                if (events.empty())
                    return EmitError(op, SbieStatus::NOT_FOUND,
                                     L"log buffer empty");
                if (op.json) {
                    json::JsonValue arr = json::JsonValue::Array();
                    for (const ServerLogEvent& e : events) {
                        json::JsonValue jm = json::JsonValue::Object();
                        jm.set(L"time", json::JsonValue(
                                   e.time.empty() ? NowHms() : e.time));
                        jm.set(L"code", json::JsonValue((long long)e.msgid));
                        jm.set(L"id",
                               json::JsonValue((long long)(e.msgid & 0xFFFF)));
                        jm.set(L"pid", json::JsonValue((long long)e.pid));
                        jm.set(L"text", json::JsonValue(e.text));
                        arr.pushBack(std::move(jm));
                    }
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"count",
                          json::JsonValue((long long)events.size()));
                    d.set(L"messages", std::move(arr));
                    EmitJsonOk(op, d);
                    return 0;
                }
                WatchFilter noFilter;
                for (const ServerLogEvent& e : events)
                    EmitServerEvent(op, noFilter, e);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    LogCommonOptions lc;
    int rc = LogPrologue(o, &lc);
    if (rc != 0)
        return rc;

    WatchFilter f;   // 不过滤，原样按缓冲序输出
    ULONG cursor = 0;
    LogEntry e;
    std::deque<LogEntry> ring;
    while (FetchLogEntry(lc.sessionId, &cursor, &e)) {
        ring.push_back(e);
        if (ring.size() > last)
            ring.pop_front();
    }
    if (ring.empty())
        return EmitError(o, SbieStatus::NOT_FOUND, L"log buffer empty");

    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const auto& ent : ring) {
            json::JsonValue jm = json::JsonValue::Object();
            jm.set(L"time", json::JsonValue(NowHms()));
            jm.set(L"code", json::JsonValue((long long)ent.msgCode));
            jm.set(L"id", json::JsonValue((long long)(ent.msgCode & 0xFFFF)));
            jm.set(L"pid", json::JsonValue((long long)ent.pid));
            jm.set(L"text", json::JsonValue(FormatText(lc.api, ent, false)));
            arr.pushBack(std::move(jm));
        }
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"count", json::JsonValue((long long)ring.size()));
        data.set(L"messages", std::move(arr));
        EmitJsonOk(o, data);
        return 0;
    }
    for (const auto& ent : ring)
        EmitLogLine(o, f, lc.api, ent);
    return 0;
}

} // namespace sbie::cli
