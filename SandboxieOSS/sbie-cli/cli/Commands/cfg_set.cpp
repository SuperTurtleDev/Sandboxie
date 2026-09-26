// Sandboxie-OSS — sbie-cli/cli/Commands/cfg_set.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie cfg set <setting> <value> [--section <s>=GlobalSettings]
//             [--append|--insert]（04 §4.5）。
// sbie cfg unset <setting> [--section <s>] [--index <i>]（P0-6）。
// sbie cfg lock <new-pw> [--password <old-pw>]（P0-7；空新密码 = 解除锁定）。
// sbie cfg unlock <pw>（P0-8；仅验证——密码不缓存，后续写逐次带
//             --password/SBIE_PASS，06 §P0-8 建议语义）。
// 写路径经 SbieSvc（0x1811-0x1814 / 0x1807-0x1808，落盘 + refresh）；
// --no-refresh 关热重载（04 §8.2）。退出码：0；6=密码/权限；7=密码超长/
// 参数非法；5=目标不存在；4=SbieSvc 不可用。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/ConfigStore.h"

namespace sbie::cli {

int CmdCfgSet(const CommandContext& ctx)
{
    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli cfg set <setting> <value> "
                         L"[--section <s>] [--append|--insert]");

    const std::wstring& setting = pos[0];
    std::wstring value;
    for (size_t i = 1; i < pos.size(); ++i) {
        if (!value.empty())
            value += L' ';
        value += pos[i];
    }
    std::wstring section = L"GlobalSettings";
    if (std::wstring s = boxproc::OptionValue(ctx.args, L"--section"); !s.empty())
        section = s;

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    const bool refresh = !ctx.opts.noRefresh;

    // 写路径（非幂等，retry=false；params.password = P0-11 锁配置写凭据；
    // params.refresh = P1-6：--no-refresh 此前不进 params → IPC 路径恒热重载）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"setting", setting);
        ipcroute::PSet(&params, L"value", value);
        ipcroute::PSet(&params, L"section", section);
        ipcroute::PSet(&params, L"refresh", refresh);
        if (boxproc::HasFlag(ctx.args, L"--append"))
            ipcroute::PSet(&params, L"mode", std::wstring(L"append"));
        else if (boxproc::HasFlag(ctx.args, L"--insert"))
            ipcroute::PSet(&params, L"mode", std::wstring(L"insert"));
        else
            ipcroute::PSet(&params, L"mode", std::wstring(L"update"));
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpCfgSet, params, false,
            [](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, L"set");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::ConfigStore cfg;

    SbieStatus st;
    if (boxproc::HasFlag(ctx.args, L"--append"))
        st = cfg.SetAppend(section, setting, value, refresh, pw);
    else if (boxproc::HasFlag(ctx.args, L"--insert"))
        st = cfg.SetInsert(section, setting, value, refresh, pw);
    else
        st = cfg.Set(section, setting, value, refresh, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"cfg set failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    boxproc::EmitSilentOk(ctx.opts, L"set");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie cfg unset <setting> [--section <s>] [--index <i>]（P0-6）
// --index 给定 = 只删该值（List→重放组合，ConfigStore::Delete）；
// 缺省 = 删整 setting（0x1814 空 value）
// ---------------------------------------------------------------------------

int CmdCfgUnset(const CommandContext& ctx)
{
    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli cfg unset <setting> "
                         L"[--section <s>] [--index <i>]");

    const std::wstring& setting = pos[0];
    std::wstring section = L"GlobalSettings";
    if (std::wstring s = boxproc::OptionValue(ctx.args, L"--section"); !s.empty())
        section = s;

    bool haveIndex = false;
    ULONG idx = 0;
    if (boxproc::HasFlag(ctx.args, L"--index")) {
        const std::wstring idxStr = boxproc::OptionValue(ctx.args, L"--index");
        if (!boxproc::ParseUlong(idxStr, &idx))
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"bad --index value: " + idxStr);
        haveIndex = true;
    }

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    const bool refresh = !ctx.opts.noRefresh;

    // 写路径（非幂等，retry=false；cfg.unset）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"setting", setting);
        ipcroute::PSet(&params, L"section", section);
        if (haveIndex)
            ipcroute::PSet(&params, L"index", (long long)idx);
        ipcroute::PSet(&params, L"refresh", refresh);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpCfgUnset, params, false,
            [](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, L"unset");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::ConfigStore cfg;

    // 整 setting 删除的存在性前置（04 §4.5 rc 5；--index 越界由 Delete 内部
    // 的 List→重放组合返回 NOT_FOUND）
    if (!haveIndex && cfg.GetList(section, setting).empty())
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"setting not found: [" + section + L"] " + setting);

    SbieStatus st = cfg.Delete(section, setting,
                               haveIndex ? std::optional<ULONG>(idx)
                                         : std::nullopt,
                               refresh, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"cfg unset failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    boxproc::EmitSilentOk(ctx.opts, L"unset");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie cfg lock <new-pw> [--password <old-pw>]（P0-7）
// SET_PASSWORD（0x1807）：空新密码 = 解除锁定；变更已设密码需经
// --password/SBIE_PASS 提供旧密码。>64 WCHAR → rc 7
// ---------------------------------------------------------------------------

int CmdCfgLock(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli cfg lock <new-pw> "
                         L"[--password <old-pw>] (empty new-pw = remove lock)");
    const std::wstring& newPw = pos[0];
    if (newPw.size() > 64)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"password exceeds 64 characters");

    const std::wstring oldPw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    if (oldPw.size() > 64)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"password exceeds 64 characters");
    const bool remove = newPw.empty();
    const wchar_t* what = remove ? L"config lock removed" : L"config locked";

    // 写路径（非幂等，retry=false；cfg.lock：password=旧密码、new_password）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"new_password", newPw);
        if (!oldPw.empty())
            ipcroute::PSet(&params, L"password", oldPw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpCfgLock, params, false,
            [what](const GlobalOptions& o, const json::JsonValue&) {
                EmitMessage(o, what);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    SbieStatus st = svc::SvcClient::Instance().SetPassword(oldPw, newPw);
    if (st == SbieStatus::ACCESS_DENIED)
        return EmitError(ctx.opts, SbieStatus::ACCESS_DENIED,
                         L"wrong old password (pass --password <current-pw>"
                         L" or set SBIE_PASS)");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"cfg lock failed (SbieSvc required)");

    EmitMessage(ctx.opts, what);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie cfg unlock <pw>（P0-8）
// TEST_PASSWORD（0x1808）验证。连接级缓存语义（06 §P0-8 建议）：CLI 一命令
// 一进程，密码**不缓存**——unlock 仅验证；后续写命令逐次携带
// --password/SBIE_PASS。解除锁定 = `cfg lock "" --password <旧密码>`。
// ---------------------------------------------------------------------------

int CmdCfgUnlock(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli cfg unlock <pw>");
    const std::wstring& pw = pos[0];
    if (pw.size() > 64)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"password exceeds 64 characters");

    // 写路径（非幂等，retry=false；cfg.unlock）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpCfgUnlock, params, false,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                const json::JsonValue* m = data.isObject()
                    ? data.find(L"message") : nullptr;
                EmitMessage(o, m && m->isString()
                    ? m->asString()
                    : std::wstring(L"password verified"));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    if (!model::ConfigStore().Locked()) {
        EmitMessage(ctx.opts, L"config is not locked");
        return 0;
    }

    SbieStatus st = svc::SvcClient::Instance().TestPassword(pw);
    if (st == SbieStatus::ACCESS_DENIED)
        return EmitError(ctx.opts, SbieStatus::ACCESS_DENIED,
                         L"wrong password (unlock verifies only; to remove"
                         L" the lock use: cfg lock \"\" --password <pw>)");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"cfg unlock failed (SbieSvc required)");

    EmitMessage(ctx.opts,
                L"password verified (not cached; subsequent writes need"
                L" --password or SBIE_PASS)");
    return 0;
}

void RegisterCfgSetCommands()
{
    auto& cfg = Commands()["cfg"];
    cfg["set"] = CmdCfgSet;
    cfg["unset"] = CmdCfgUnset;
    cfg["lock"] = CmdCfgLock;
    cfg["unlock"] = CmdCfgUnlock;
}

} // namespace sbie::cli
