// Sandboxie-OSS — sbie-cli/cli/Commands/proc_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie proc info|start|kill|kill-all|suspend|resume（04 §4.4）。
//   info：GetProcInfo(1|2|4) + 驱动 flags（存在性经 QueryProcessEx2 判定，
//         02 §7.4：flags==0 与取数失败不可区分）。
//   start：SvcClient::RunSandboxed（SbieSvc 0x1205）；--elevated 降级
//          SbieDll_RunStartExe /elevated；--wait 等待并透传子进程退出码。
//   kill：SbieSvc MSGID_PROCESS_KILL_ONE（02 §6：杀进程必须经 SbieSvc；
//         SbieApi_* 无 kill 导出——已核对 137 项导出表）。
//   kill-all：MSGID_PROCESS_KILL_ALL（Model KillBox；P0-1 接线）。
//   suspend/resume：MSGID_PROCESS_SUSPEND_RESUME_ONE（P0-2 接线）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/Processes.h"

#include <windows.h>

namespace sbie::cli {

namespace {

model::ProcessRepository MakeRepo()
{
    return model::ProcessRepository(nullptr, svc::SvcClient::Instance());
}

} // namespace

// ---------------------------------------------------------------------------
// sbie proc info <pid>
// ---------------------------------------------------------------------------

int CmdProcInfo(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc info <pid>");
    ULONG pid = 0;
    if (!boxproc::ParseUlong(pos[0], &pid))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"bad pid: " + pos[0]);

    // IPC 优先（proc.info；data 字段 = 04 §7.2 snake_case），降级直连
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"pid", (long long)pid);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpProcInfo, params, true,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                return ipcroute::RenderKv(
                    o,
                    { { L"pid", L"pid" },
                      { L"box", L"box" },
                      { L"image", L"image" },
                      { L"command_line", L"cmdline" },
                      { L"working_dir", L"workdir" },
                      { L"parent_pid", L"parent_pid" },
                      { L"session", L"session" },
                      { L"started", L"started" },
                      { L"suspended", L"suspended" },
                      { L"flags", L"flags" } },
                    data);
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();

    // 存在性（在沙箱内）判定：QueryProcessEx2
    if (!drv::Loaded() && !drv::LoadSbieDll(ctx.opts.sbieDllPath))
        return EmitError(ctx.opts, SbieStatus::DRIVER_UNAVAILABLE,
                         L"SbieDll.dll not available");
    drv::ProcQuery q;
    if (drv::QueryProcessById(pid, &q) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"pid " + std::to_wstring(pid)
                         + L" is not a running sandboxed process");

    svc::ProcInfo pi;
    SbieStatus st = MakeRepo().Info(pid, &pi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"proc info failed (SbieSvc required)");

    wchar_t flagsHex[16];
    swprintf_s(flagsHex, L"0x%08lX", pi.flags);
    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"pid", std::to_wstring(pid) },
        { L"box", q.box },
        { L"image", pi.image.empty() ? q.image : pi.image },
        { L"command_line", pi.cmdline },
        { L"working_dir", pi.workdir },
        { L"parent_pid", std::to_wstring(pi.parentId) },
        { L"session", std::to_wstring(q.sessionId) },
        { L"started", boxproc::FormatCreateTime(q.createTime) },
        { L"suspended", pi.suspended ? L"yes" : L"no" },
        { L"flags", std::to_wstring(pi.flags) },
        { L"flags_hex", flagsHex },
    };

    json::JsonValue obj = json::JsonValue::Object();
    obj.set(L"pid", json::JsonValue((long long)pid));
    obj.set(L"box", json::JsonValue(q.box));
    obj.set(L"image", json::JsonValue(pi.image.empty() ? q.image : pi.image));
    obj.set(L"cmdline", json::JsonValue(pi.cmdline));
    obj.set(L"workdir", json::JsonValue(pi.workdir));
    obj.set(L"parent_pid", json::JsonValue((long long)pi.parentId));
    obj.set(L"session", json::JsonValue((long long)q.sessionId));
    obj.set(L"started", json::JsonValue(boxproc::FormatCreateTime(q.createTime)));
    obj.set(L"suspended", json::JsonValue(pi.suspended));
    obj.set(L"flags", json::JsonValue((long long)pi.flags));
    obj.set(L"flags_hex", json::JsonValue(flagsHex));

    EmitKv(ctx.opts, kv, obj);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie proc start <box> <cmd...> [--dir <d>] [--elevated] [--wait]
// ---------------------------------------------------------------------------

int CmdProcStart(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc start <box> <cmd...> "
                         L"[--dir <d>] [--elevated] [--wait]");
    const std::wstring& box = pos[0];
    std::wstring cmd;
    for (size_t i = 1; i < pos.size(); ++i) {
        if (!cmd.empty())
            cmd += L' ';
        cmd += pos[i];
    }
    std::wstring dir = boxproc::OptionValue(ctx.args, L"--dir");
    // P1-7（06 缺口表）：--dir 缺省时显式代 client 当前目录进 params——
    // 消除 IPC 路径 dir 缺省 = server cwd（exe 目录）的语义漂移
    //（Model Start 空 dir 以调用方 cwd 代入：直连=client cwd、IPC=server
    // cwd，04 §12 坑 5）；两路径自此同为 client cwd。env 块继承差异维持
    // 文档化（长期方案 --env K=V 透传，见 docs/04 §15 遗留）。
    if (dir.empty()) {
        wchar_t cwd[MAX_PATH + 1] = L"";
        if (GetCurrentDirectoryW(MAX_PATH + 1, cwd))
            dir = cwd;
        else
            dir = L"C:\\";
    }
    const bool elevated = boxproc::HasFlag(ctx.args, L"--elevated");
    const bool wait = boxproc::HasFlag(ctx.args, L"--wait");

    // 写路径（非幂等，retry=false；本构建 server op 仍是 ERR_NOT_IMPLEMENTED
    // 占位 → ipcroute 自动降级直连 SbieSvc RunSandboxed）
    if (!wait) {   // --wait 需要进程句柄，恒走直连
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"box", box);
        ipcroute::PSet(&params, L"cmd", cmd);
        ipcroute::PSet(&params, L"dir", dir);
        ipcroute::PSet(&params, L"elevated", elevated);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpProcStart, params, false, nullptr);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    SbieStatus st = repo.GetInfo(box, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + box + L"' not found");

    svc::RunResult rr;
    st = MakeRepo().Start(box, cmd, dir, elevated, &rr);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"proc start failed");

    if (elevated || !rr.hProcess) {
        // 降级路径无句柄/PID（SbieDll_RunStartExe 不外露）
        if (ctx.opts.json) {
            json::JsonValue d = json::JsonValue::Object();
            d.set(L"elevated", json::JsonValue(true));
            d.set(L"message", json::JsonValue(L"start requested (elevated)"));
            EmitJsonOk(ctx.opts, d);
        } else {
            util::PrintLineUtf8(util::WideToUtf8(
                L"start requested (elevated; pid not tracked)"));
        }
        return 0;
    }

    ULONG exitCode = 0;
    if (wait) {
        WaitForSingleObject(rr.hProcess, INFINITE);
        DWORD ec = 0;
        GetExitCodeProcess(rr.hProcess, &ec);
        exitCode = (ULONG)ec;
    }
    CloseHandle(rr.hProcess);   // 03 §8.4：句柄必须关（--wait 用完即关）

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"pid", json::JsonValue((long long)rr.pid));
        if (wait) {
            d.set(L"exit_code", json::JsonValue((long long)exitCode));
            EmitJsonOk(ctx.opts, d);
            return (int)exitCode;   // 透传子进程退出码（04 §4.4）
        }
        EmitJsonOk(ctx.opts, d);
        return 0;
    }
    util::PrintLineUtf8(util::WideToUtf8(std::to_wstring(rr.pid)));
    if (wait) {
        util::PrintLineUtf8(util::WideToUtf8(
            L"exit code " + std::to_wstring(exitCode)));
        return (int)exitCode;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie proc kill <pid>
// ---------------------------------------------------------------------------

int CmdProcKill(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc kill <pid>");
    ULONG pid = 0;
    if (!boxproc::ParseUlong(pos[0], &pid))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"bad pid: " + pos[0]);

    // 写路径（非幂等，retry=false；本构建 server op 占位 → 降级直连 SbieSvc）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"pid", (long long)pid);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpProcKill, params, false,
            [pid](const GlobalOptions& o, const json::JsonValue& /*data*/) {
                EmitMessage(o, L"pid " + std::to_wstring(pid) + L" killed");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    drv::ProcQuery q;
    if (drv::QueryProcessById(pid, &q) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"pid " + std::to_wstring(pid)
                         + L" is not a running sandboxed process");

    SbieStatus st = MakeRepo().Kill(pid);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"proc kill failed (SbieSvc required)");
    EmitMessage(ctx.opts, L"pid " + std::to_wstring(pid) + L" killed");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie proc kill-all <box>（P0-1；04 §4.4：按沙箱终止全部进程）
// sbie proc kill-all --all（P1-3：全局形态，无 box）= EnumBoxes 循环 KillBox
// Model KillBox → SbieSvc MSGID_PROCESS_KILL_ALL；计数 = 请求时该 box 的
// 进程数（EnumBoxProcesses）；全局计数 = 各 box 请求时计数之和
// ---------------------------------------------------------------------------

int CmdProcKillAll(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    const bool globalAll = boxproc::HasFlag(ctx.args, L"--all");
    if (pos.empty() && !globalAll)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc kill-all <box>"
                         L" | proc kill-all --all");
    if (!pos.empty() && globalAll)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"--all takes no box argument");
    const std::wstring box = pos.empty() ? std::wstring() : pos[0];

    // 写路径（非幂等，retry=false；proc.killAll——box 可空 = 全局，P1-3）
    {
        json::JsonValue params = json::JsonValue::Object();
        if (!box.empty())
            ipcroute::PSet(&params, L"box", box);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpProcKillAll, params, false,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                // data = {count, boxes, message}（server 已按请求时进程计数）
                if (o.json) {
                    EmitJsonOk(o, data);
                    return 0;
                }
                const json::JsonValue* m = data.isObject()
                    ? data.find(L"message") : nullptr;
                util::PrintLineUtf8(util::WideToUtf8(
                    m && m->isString() ? m->asString()
                                       : std::wstring(L"0 process(es)"
                                                      L" terminated")));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连降级：目标 box 集（全局 = 启用中的全部 box）
    std::vector<std::wstring> targets;
    if (!box.empty()) {
        model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
        model::BoxInfo bi;
        SbieStatus st = repo.GetInfo(box, &bi);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"box '" + box + L"' not found");
        targets.push_back(box);
    } else {
        model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
        for (const model::BoxInfo& bi : repo.EnumBoxes(false))
            targets.push_back(bi.name);
    }

    size_t total = 0;
    for (const std::wstring& b : targets) {
        std::vector<ULONG> pids;
        // 计数用（请求时的进程数；枚举失败=0，KillBox 会给出真实错误）
        (void)drv::EnumBoxProcesses(b, false, &pids);
        SbieStatus st = MakeRepo().KillBox(b);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"proc kill-all failed for box '" + b
                             + L"' (SbieSvc required)");
        total += pids.size();
    }

    const std::wstring msg = std::to_wstring(total)
        + L" process(es) terminated"
        + (box.empty() ? L" (" + std::to_wstring(targets.size())
                             + L" box(es))" : std::wstring());
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"count", json::JsonValue((long long)total));
        d.set(L"boxes", json::JsonValue((long long)targets.size()));
        d.set(L"message", json::JsonValue(msg));
        EmitJsonOk(ctx.opts, d);
    } else {
        util::PrintLineUtf8(util::WideToUtf8(msg));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie proc suspend <pid> / resume <pid>（P0-2；04 §4.4：SuspendResume）
// Model Suspend/Resume → SbieSvc MSGID_PROCESS_SUSPEND_RESUME_ONE
// ---------------------------------------------------------------------------

int CmdProcSuspendResume(const CommandContext& ctx, bool suspend)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         suspend ? L"usage: sbie-cli proc suspend <pid>"
                                 : L"usage: sbie-cli proc resume <pid>");
    ULONG pid = 0;
    if (!boxproc::ParseUlong(pos[0], &pid))
        return EmitError(ctx.opts, SbieStatus::USAGE, L"bad pid: " + pos[0]);

    const wchar_t* what = suspend ? L"suspended" : L"resumed";

    // 写路径（非幂等，retry=false；proc.suspend / proc.resume）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"pid", (long long)pid);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, suspend ? ipc::kOpProcSuspend : ipc::kOpProcResume,
            params, false,
            [what](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, what);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 存在性（在沙箱内）判定：QueryProcessEx2（与 proc kill 同款）
    drv::ProcQuery q;
    if (drv::QueryProcessById(pid, &q) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"pid " + std::to_wstring(pid)
                         + L" is not a running sandboxed process");

    SbieStatus st = suspend ? MakeRepo().Suspend(pid) : MakeRepo().Resume(pid);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         std::wstring(L"proc ") + what
                             + L" failed (SbieSvc required)");
    boxproc::EmitSilentOk(ctx.opts, what);
    return 0;
}

int CmdProcSuspend(const CommandContext& ctx) { return CmdProcSuspendResume(ctx, true); }
int CmdProcResume(const CommandContext& ctx)  { return CmdProcSuspendResume(ctx, false); }

void RegisterProcCommands()
{
    auto& proc = Commands()["proc"];
    proc["info"] = CmdProcInfo;
    proc["start"] = CmdProcStart;
    proc["kill"] = CmdProcKill;
    proc["kill-all"] = CmdProcKillAll;
    proc["suspend"] = CmdProcSuspend;
    proc["resume"] = CmdProcResume;
}

} // namespace sbie::cli
