// Sandboxie-OSS — sbie-cli/cli/Commands/force_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie force on [<seconds>] / off / status（P1-1，06 缺口表；docs/04 §4.8）。
//   on  ：驱动 API_DISABLE_FORCE_PROCESS set_flag=1；<seconds> 给定时先写
//         GlobalSettings\ForceDisableSeconds（QSbieAPI DisableProcess 同序，
//         SbieAPI.cpp:2589-2596），缺省沿用现配置（缺省 10）。
//   off ：set_flag=0（清除禁用时间戳，立即恢复）。
//   status：get_flag 查询（Session_IsForceDisabled，BOOLEAN）；剩余秒数在
//         IPC 路径由 server 记录的禁用时刻推算（SandMan 托盘倒计时同源
//         语义）；直连路径跨进程无时刻可考 → 剩余未知。
// 直驱动（SbieApi_Ioctl），无 SbieSvc 依赖——唯 ForceDisableSeconds 写经
// SbieSvc。退出码：0；3=驱动不可用；6=权限（ForceDisableAdminOnly/锁配置）；
// 7=seconds 非法。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/ConfigStore.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace sbie::cli {

namespace {

// 现配置的禁用窗口（缺省 10——drv Conf_Get_Number 同缺省）
long long ForceWindowSeconds()
{
    auto v = model::ConfigStore().Get(L"GlobalSettings",
                                      L"ForceDisableSeconds", 0, true, true);
    if (!v.has_value() || v->empty())
        return 10;
    return _wtol(v->c_str());
}

// status 的文本/JSON 渲染（IPC data 与直连自组对象共用字段名：
// disabled / window_seconds / remaining_seconds(null=未知)）
int RenderForceStatus(const GlobalOptions& opts, bool disabled,
                      long long window, bool haveRemaining,
                      long long remaining)
{
    const std::wstring msg = disabled
        ? (haveRemaining
           ? L"force process: disabled (" + std::to_wstring(remaining)
             + L" second(s) remaining)"
           : std::wstring(L"force process: disabled"
                          L" (remaining time unknown; set by another"
                          L" process)"))
        : std::wstring(L"force process: normal (force enabled)");
    if (opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"disabled", json::JsonValue(disabled));
        d.set(L"window_seconds", json::JsonValue(window));
        d.set(L"remaining_seconds",
              haveRemaining ? json::JsonValue(remaining) : json::JsonValue());
        d.set(L"message", json::JsonValue(msg));
        EmitJsonOk(opts, d);
        return 0;
    }
    EmitMessage(opts, msg);
    return 0;
}

// IPC 路径的 force.status 渲染
int RenderForceStatusIpc(const GlobalOptions& opts, const json::JsonValue& data)
{
    if (opts.json) {
        EmitJsonOk(opts, data);
        return 0;
    }
    const json::JsonValue* m = data.isObject() ? data.find(L"message") : nullptr;
    EmitMessage(opts, m && m->isString()
        ? m->asString() : std::wstring(L"force process: unknown"));
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie force on [<seconds>]
// ---------------------------------------------------------------------------

int CmdForceOn(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    bool haveSeconds = false;
    long long seconds = 0;
    if (!pos.empty()) {
        wchar_t* end = nullptr;
        seconds = wcstol(pos[0].c_str(), &end, 10);
        if (!end || *end != L'\0' || seconds < 0 || seconds > 86400)
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"bad seconds: " + pos[0]
                             + L" (0 < s <= 86400)");
        if (seconds == 0)
            return EmitError(ctx.opts, SbieStatus::INVALID,
                             L"seconds must be > 0"
                             L" (ForceDisableSeconds=0 forbids disabling)");
        haveSeconds = true;
    }
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);

    // 写路径（非幂等，retry=false；force.set）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"enable", true);
        if (haveSeconds)
            ipcroute::PSet(&params, L"seconds", seconds);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpForceSet, params, false, nullptr);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连降级：seconds 给定时先写 ForceDisableSeconds（SbieSvc 写路径）
    if (haveSeconds) {
        wchar_t buf[24];
        swprintf_s(buf, L"%lld", seconds);
        SbieStatus st = model::ConfigStore().Set(
            L"GlobalSettings", L"ForceDisableSeconds", buf, true, pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"force on failed (ForceDisableSeconds write)"
                             + std::wstring(boxproc::PasswordHint(st, pw)));
    }

    ULONG flag = 1;
    SbieStatus st = drv::DisableForceProcess(&flag, nullptr);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"force on failed (driver ioctl; admin required if"
                         L" ForceDisableAdminOnly=y)");

    const long long window = haveSeconds ? seconds : ForceWindowSeconds();
    const std::wstring msg = L"force process disabled for "
        + std::to_wstring(window) + L" second(s)";
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"disabled", json::JsonValue(true));
        d.set(L"window_seconds", json::JsonValue(window));
        d.set(L"message", json::JsonValue(msg));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts, msg);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie force off
// ---------------------------------------------------------------------------

int CmdForceOff(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    // 写路径（非幂等，retry=false；force.set）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"enable", false);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpForceSet, params, false,
            [](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, L"force process enabled");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    ULONG flag = 0;
    SbieStatus st = drv::DisableForceProcess(&flag, nullptr);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"force off failed (driver ioctl)");
    boxproc::EmitSilentOk(ctx.opts, L"force process enabled");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie force status
// ---------------------------------------------------------------------------

int CmdForceStatus(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    // 读路径（retry=true；force.status：server 记录禁用时刻可推剩余秒）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpForceStatus, params, true,
            RenderForceStatusIpc);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连降级：跨进程禁用时刻不可考 → 剩余未知
    ULONG flag = 0;
    SbieStatus st = drv::DisableForceProcess(nullptr, &flag);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"force status failed (driver ioctl)");
    return RenderForceStatus(ctx.opts, flag != FALSE, ForceWindowSeconds(),
                             false, 0);
}

void RegisterForceCommands()
{
    auto& force = Commands()["force"];
    force["on"] = CmdForceOn;
    force["off"] = CmdForceOff;
    force["status"] = CmdForceStatus;
}

} // namespace sbie::cli
