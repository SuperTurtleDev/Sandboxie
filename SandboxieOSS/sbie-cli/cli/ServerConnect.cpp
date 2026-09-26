// Sandboxie-OSS — sbie-cli/cli/ServerConnect.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 00-architecture §5 (A)-(E) 状态机实现（M2 server 波次）。

#include "ServerConnect.h"
#include "Output.h"
#include "../ipcc/SbieIpc.h"
#include "../../SbieCore/Util/Json.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

namespace sbie::cli::srvconn {

namespace {

// 进程级缓存的 IPC 连接（CLI 短生命周期：一次命令 = 一条连接）
ipc::PipeClient& Session()
{
    static ipc::PipeClient c;
    return c;
}

// (B) 拉起 server：自身 exe + --start-server（DETACHED 不继承控制台 +
// BREAKAWAY 逃脱 Job；cwd=exe 目录，SbieDll 同目录加载，00 §5）
bool SpawnServer()
{
    WCHAR self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
        return false;
    std::wstring dir(self);
    const size_t cut = dir.rfind(L'\\');
    dir = (cut == std::wstring::npos) ? std::wstring(L".") : dir.substr(0, cut);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --start-server";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        // 所在 Job 不允许 breakaway：退回无该标志（server 与 client 同 Job
        // 生死，可接受；00 §5 的主路径仍是带标志）
        ok = CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE,
                            DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi);
    }
    if (!ok)
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool EnsureConnected(const GlobalOptions& opts)
{
    if (!kServerImplemented || opts.noServer) // (E)：永不拉起
        return false;
    if (Session().IsOpen())
        return true;

    // (A)/(D)：直接探测（busy 在 1s 预算内 WaitNamedPipe 重试）
    if (Session().Open(1000))
        return true;

    const DWORD err = Session().LastError();
    if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND
        && err != ERROR_PATH_NOT_FOUND) {
        return false; // e.g. ACCESS_DENIED：非管道归属问题，不拉起 → (C)
    }

    // (B)：拉起 + 50ms 轮询，上限 3000ms
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        if (!SpawnServer())
            return false; // (C) 拉起失败 → 降级
    }
    const ULONGLONG deadline = GetTickCount64() + 3000;
    for (;;) {
        Sleep(50);
        if (Session().Open(0))
            return true;
        if (GetTickCount64() >= deadline)
            return false; // (C) 超时 → 降级
    }
}

bool HasServer()
{
    return Session().IsOpen();
}

bool ProbeRunning(ULONG* serverPid)
{
    // 只探测不拉起：open → GetNamedPipeServerProcessId → 立即断开
    //（server 侧工作线程按 EOF 收尸，开销可忽略）
    ipc::PipeClient p;
    if (!p.Open(500))
        return false;
    ULONG pid = 0;
    if (serverPid) {
        if (!GetNamedPipeServerProcessId(p.Handle(), &pid))
            pid = 0;
        *serverPid = pid;
    }
    return true;
}

void NoteDegraded()
{
    static bool noted = false;
    if (!noted && !Session().IsOpen()) {
        noted = true;
        Diag(L"server not running - degraded to direct driver connection");
    }
}

IpcOutcome Call(const char* op, const json::JsonValue& params, bool retry)
{
    IpcOutcome o;
    const std::string req = params.isObject() ? json::SerializeUtf8(params)
                                              : std::string("{}");
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Session().IsOpen()) {
            GlobalOptions def; // 探测/拉起（不带 --no-server 语义）
            if (!EnsureConnected(def))
                break;
        }
        std::string reply;
        if (!Session().RoundTrip(op, req, &reply)) {
            // 传输层断裂：server 死亡（ERROR_BROKEN_PIPE/NO_DATA）
            if (!retry || attempt == 1)
                break;
            Diag(L"server connection lost - respawning and retrying once");
            continue; // 下一轮 EnsureConnected 重新拉起
        }

        json::JsonValue v;
        SbieStatus jerr = SbieStatus::OK;
        if (!json::Parse(reply, &v, &jerr) || !v.isObject()) {
            o.transport = true;
            o.ok = false;
            o.code = (int)SbieStatus::GENERIC;
            o.message = L"malformed server reply";
            return o;
        }
        o.transport = true;
        const json::JsonValue* okf = v.find(L"ok");
        o.ok = okf && okf->asBool();
        if (o.ok) {
            const json::JsonValue* d = v.find(L"data");
            if (d)
                o.data = *d;
        } else {
            const json::JsonValue* e = v.find(L"error");
            if (e && e->isObject()) {
                const json::JsonValue* c = e->find(L"code");
                if (c && c->isInt())
                    o.code = (int)c->asInt();
                const json::JsonValue* m = e->find(L"message");
                if (m && m->isString())
                    o.message = m->asString();
            }
        }
        return o;
    }

    // 传输不可恢复（00 §7）：幂等=拉起仍失败 4；非幂等=8 提示手动重试
    o.transport = false;
    o.ok = false;
    if (retry) {
        o.code = (int)SbieStatus::SERVER_UNAVAILABLE;
        o.message = L"server unavailable (spawn failed or timed out)";
    } else {
        o.code = (int)SbieStatus::RETRY_SUGGESTED;
        o.message = L"server died mid-command; non-idempotent op - retry"
                    L" manually";
    }
    return o;
}

} // namespace sbie::cli::srvconn
