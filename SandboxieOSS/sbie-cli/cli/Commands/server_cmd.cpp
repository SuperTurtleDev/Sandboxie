// Sandboxie-OSS — sbie-cli/cli/Commands/server_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie server start|stop|status（04-modules.md §4.2）。
//   start：幂等——已在跑直接报成功；不在跑派生 "<self> --start-server
//          [--idle-timeout N]"（DETACHED + BREAKAWAY，00 §5 同款）后轮询确认。
//   stop ：经管道发 server.shutdown（同会话校验在 server 侧）；未运行报 5。
//   status：srvconn::ProbeRunning（不拉起）；运行中取 status op 的
//          server 段（pid/uptime/clients/idle/log_pump）。
//
// 两个已知框架交互（记录于 04 接线验收记录）：
//   * Run() 在路由前对一切命令做 EnsureConnected——"server stop 时 server
//     本不在跑" 会被先拉起再停。为保住 04 §4.2 的退出码 5 语义，本文件以
//     静态初始化期（先于 EnsureConnected）的 ProbeRunning 快照判定"命令
//     发起时 server 是否在跑"：不在跑 → 把刚拉起的实例静默收掉并报 5。
//   * 同因，"server start --idle-timeout N" 发现预拉起实例用了默认 300s
//     超时会先停再按自定义超时重启（保证该选项可测）。

#include "ServerCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/Util/Json.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace sbie::cli {

namespace {

// 命令发起时刻的 server 状态快照。静态初始化先于 cli::Run 的
// EnsureConnected 执行（04 §8.13：本探针仅 CreateFile/CloseHandle，无跨
// 翻译单元 C++ 全局依赖，安全）。server 侧按 EOF 收尸（00 §9.6）。
ULONG g_pidAtLaunch = 0;
bool g_runningAtLaunch = false;

struct LaunchProbe {
    LaunchProbe()
    {
        g_runningAtLaunch = srvconn::ProbeRunning(&g_pidAtLaunch);
    }
};
const LaunchProbe g_launchProbe;

// 派生 server（ServerConnect.cpp SpawnServer 同款；DETACHED 不继承控制台 +
// BREAKAWAY 逃脱 Job，Job 拒绝 breakaway 时回退无标志）。noGuardians =
// --no-guardians 透传（波次 A，07-P0-2）
bool SpawnServer(ULONG idleTimeoutSec, bool noGuardians)
{
    WCHAR self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
        return false;
    std::wstring dir(self);
    const size_t cut = dir.rfind(L'\\');
    dir = (cut == std::wstring::npos) ? std::wstring(L".") : dir.substr(0, cut);
    std::wstring cmd = L"\"" + std::wstring(self)
                       + L"\" --start-server --idle-timeout "
                       + std::to_wstring(idleTimeoutSec);
    if (noGuardians)
        cmd += L" --no-guardians";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
        ok = CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE,
                            DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi);
    if (!ok)
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 轮询等待 server 就绪/退出（50ms 步进）
bool WaitServerGone(ULONGLONG ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        if (!srvconn::ProbeRunning())
            return true;
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(50);
    }
}

ULONG WaitServerUp(ULONGLONG ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        ULONG pid = 0;
        if (srvconn::ProbeRunning(&pid))
            return pid;
        if (GetTickCount64() >= deadline)
            return 0;
        Sleep(50);
    }
}

// 经缓存连接发 server.shutdown；返回 true=已确认停机指令受理/进程已消失。
// 注：管道消失 ≠ 进程终止——垂死实例仍持互斥体/管道首实例锁，紧随的重拉起
// 会在双锁上败退并静默退出（实测：stop 后立即跑任意命令会降级且新实例不
// 存活）。故以"进程句柄 signaled"（句柄表全释放、锁全让位）为准；探测轮询
// 反而会给排空期 server 添新连接（worker 即退但仍计入 ActiveClients），
// 仅在拿不到进程句柄时兜底。
bool ShutdownServer(bool* sentOk)
{
    ULONG pid = 0;
    srvconn::ProbeRunning(&pid);
    srvconn::IpcOutcome o = srvconn::Call(ipc::kOpServerShutdown,
                                          json::JsonValue::Object(), false);
    if (o.transport && o.ok) {
        *sentOk = true;
        bool exited = false;
        if (pid) {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (h) {
                exited = (WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
                CloseHandle(h);
            }
        }
        if (!exited)
            (void)WaitServerGone(1500);
        return true;
    }
    *sentOk = false;
    // 传输断（server 可能已死）：以探测结果为准
    return !srvconn::ProbeRunning();
}

// status op 的 server 段（server 在跑才有）
json::JsonValue ServerInfoFromStatusOp(bool* have)
{
    srvconn::IpcOutcome o = srvconn::Call(ipc::kOpStatus,
                                          json::JsonValue::Object(), true);
    if (o.transport && o.ok && o.data.isObject()) {
        const json::JsonValue* s = o.data.find(L"server");
        if (s && s->isObject()) {
            *have = true;
            return *s;
        }
    }
    *have = false;
    return json::JsonValue::Object();
}

const wchar_t* kStartUsage =
    L"usage: sbie-cli server start [--idle-timeout <sec>] [--no-guardians]";

} // namespace

// ---------------------------------------------------------------------------
// sbie server start [--idle-timeout N] [--no-guardians]（04 §4.2；幂等）
// --no-guardians（波次 A，07-P0-2）：派生时透传，空箱守护监视器关（与
// --idle-timeout 同款交互：Run() 预拉起的默认实例先停再按选项重启）。
// ---------------------------------------------------------------------------

int CmdServerStart(const CommandContext& ctx)
{
    bool haveTimeout = false;
    unsigned long timeout = 300;
    bool noGuardians = false;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if ((ctx.args[i] == L"--idle-timeout" || ctx.args[i] == L"--idle")
            && i + 1 < ctx.args.size()) {
            ++i;
            wchar_t* end = nullptr;
            timeout = wcstoul(ctx.args[i].c_str(), &end, 10);
            if (!end || *end != L'\0')
                return EmitError(ctx.opts, SbieStatus::USAGE,
                                 L"invalid --idle-timeout value: "
                                 + ctx.args[i]);
            haveTimeout = true;
        } else if (ctx.args[i] == L"--no-guardians") {
            noGuardians = true;
        } else {
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i] + L"; "
                             + kStartUsage);
        }
    }

    ULONG pid = 0;
    if (srvconn::ProbeRunning(&pid)) {
        bool already = g_runningAtLaunch;
        if ((haveTimeout || noGuardians) && !already) {
            // Run() 预拉起的实例用了默认参数（300s / guardians on）：停掉按
            // 自定义选项重启，保证 --idle-timeout/--no-guardians 可测（见
            // 文件头注）
            bool sent = false;
            (void)ShutdownServer(&sent);
            (void)WaitServerGone(2500);
            pid = 0;
            already = false;
        } else {
            if (ctx.opts.json) {
                json::JsonValue d = json::JsonValue::Object();
                d.set(L"running", json::JsonValue(true));
                d.set(L"pid", json::JsonValue((long long)pid));
                d.set(L"already_running", json::JsonValue(already));
                EmitJsonOk(ctx.opts, d);
            } else {
                util::PrintLineUtf8(util::WideToUtf8(
                    L"server already running (pid "
                    + std::to_wstring(pid) + L")"));
            }
            return 0;
        }
    }

    if (!SpawnServer(timeout, noGuardians)) {
        return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                         L"failed to spawn sbie-cli server process");
    }
    pid = WaitServerUp(3000);
    if (!pid) {
        return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                         L"server did not become ready within 3s");
    }
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"running", json::JsonValue(true));
        d.set(L"pid", json::JsonValue((long long)pid));
        d.set(L"already_running", json::JsonValue(false));
        EmitJsonOk(ctx.opts, d);
    } else {
        util::PrintLineUtf8(util::WideToUtf8(
            L"server started (pid " + std::to_wstring(pid) + L")"));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie server stop（04 §4.2；未运行退出码 5）
// ---------------------------------------------------------------------------

int CmdServerStop(const CommandContext& ctx)
{
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i].empty() || ctx.args[i][0] == L'-')
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
    }

    if (!g_runningAtLaunch) {
        // 命令发起时 server 不在跑。Run() 的 EnsureConnected 可能已为此
        // 拉起一个默认实例——按 04 §4.2 报 5，并静默收掉该预拉起实例
        if (srvconn::HasServer() || srvconn::ProbeRunning()) {
            bool sent = false;
            (void)ShutdownServer(&sent);
        }
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"server not running");
    }

    ULONG pid = 0;
    if (!srvconn::ProbeRunning(&pid))
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"server not running");

    bool sent = false;
    if (!ShutdownServer(&sent)) {
        if (!sent)
            return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                             L"server.shutdown failed (same user/session?)");
        return EmitError(ctx.opts, SbieStatus::GENERIC,
                         L"server did not exit after shutdown request");
    }
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"stopped", json::JsonValue(true));
        d.set(L"pid", json::JsonValue((long long)pid));
        EmitJsonOk(ctx.opts, d);
    } else {
        util::PrintLineUtf8(util::WideToUtf8(L"server stopped"));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie server status（04 §4.2；not running 也是退出码 0）
// ---------------------------------------------------------------------------

int CmdServerStatus(const CommandContext& ctx)
{
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i].empty() || ctx.args[i][0] == L'-')
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
    }

    ULONG pid = 0;
    if (!srvconn::ProbeRunning(&pid)) {
        if (ctx.opts.json) {
            json::JsonValue d = json::JsonValue::Object();
            d.set(L"running", json::JsonValue(false));
            EmitJsonOk(ctx.opts, d);
        } else {
            util::PrintLineUtf8(util::WideToUtf8(L"not running"));
        }
        return 0;
    }

    bool haveInfo = false;
    json::JsonValue srv = ServerInfoFromStatusOp(&haveInfo);
    if (!haveInfo) {
        // status op 不可用（server 忙/正在退出）：退化为探测所得最小信息
        srv = json::JsonValue::Object();
        srv.set(L"running", json::JsonValue(true));
        srv.set(L"pid", json::JsonValue((long long)pid));
    }

    if (ctx.opts.json) {
        EmitJsonOk(ctx.opts, srv);
        return 0;
    }

    auto str = [&](const wchar_t* k) -> std::wstring {
        return ipcroute::CellOf(srv.find(k));
    };
    util::TablePrinter t;
    t.AddColumn(L"RUNNING");
    t.AddColumn(L"PID", true);
    t.AddColumn(L"UPTIME_SEC", true);
    t.AddColumn(L"CLIENTS", true);
    t.AddColumn(L"IDLE_REMAINING_SEC", true);
    t.AddColumn(L"LOG_PUMP");
    // 空箱守护监视器（波次 A，07-P0-2）：GUARDIANS=no 的实例以
    // --no-guardians 派生或 GlobalSettings\GuardiansEnabled=n
    t.AddColumn(L"GUARDIANS");
    const json::JsonValue* idle = srv.find(L"idle_remaining_sec");
    const std::wstring idleText = (idle && idle->isNull())
        ? std::wstring(L"none")    // null = 空闲不退出（--idle-timeout 0）
        : str(L"idle_remaining_sec");
    t.AddRow({ str(L"running"), str(L"pid"), str(L"uptime_sec"),
               str(L"clients"), idleText, str(L"log_pump"),
               str(L"guardians") });
    EmitRows(ctx.opts, t, srv, L"not running");
    return 0;
}

void RegisterServerCommands()
{
    auto& reg = Commands();
    reg["server"]["start"] = CmdServerStart;
    reg["server"]["stop"] = CmdServerStop;
    reg["server"]["status"] = CmdServerStatus;
}

} // namespace sbie::cli
