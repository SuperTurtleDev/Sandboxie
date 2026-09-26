// Sandboxie-OSS — sbie-cli/cli/Commands/box_create.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box create <name> [--template <tpl>]…（04 §4.3）
//   ValidateName → Create（SET Enabled=y，不建目录）→ 可选 Apply 模板。
// 退出码：0；7=名字非法；5=已存在；6=密码/权限；4=SbieSvc 不可用。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/Templates.h"

namespace sbie::cli {

int CmdBoxCreate(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box create <name> [--template <tpl>]...");

    const std::wstring name = pos[0];

    // 提前给出精确的非法名诊断（Model 层只回 INVALID）
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid box name '" + name + L"' (max 38 chars of "
                         L"A-Z a-z 0-9 _; reserved words excluded)");

    // 写路径（非幂等，retry=false；params.password = P0-11 锁配置写凭据）
    {
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        json::JsonValue tpls = json::JsonValue::Array();
        for (size_t i = 0; i + 1 < ctx.args.size(); ++i)
            if (ctx.args[i] == L"--template")
                tpls.pushBack(json::JsonValue(ctx.args[i + 1]));
        params.set(L"templates", std::move(tpls));
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxCreate, params, false,
            [name](const GlobalOptions& o, const json::JsonValue&) {
                EmitMessage(o, L"box '" + name + L"' created");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware）：Create = SET Enabled=y（QSbieAPI CreateBox 同语义，
    // 04 §8.4——不建目录）；模板激活 = Append Template=<名> + 存在性检查
    // （与 server HBoxCreate / template apply 同序）
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box create failed");
    if (exists)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' already exists");

    svc::SvcClient& svc = svc::SvcClient::Instance();
    st = svc.IniSetSetting(name, L"Enabled", L"y",
                           svc::SvcClient::SetMode::Update, true, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box create failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    // 可选模板激活（04 §4.6 Template= 机制）
    for (size_t i = 0; i + 1 < ctx.args.size(); ++i) {
        if (ctx.args[i] != L"--template")
            continue;
        const std::wstring tpl = ctx.args[++i];
        model::TemplateRegistry treg(nullptr, svc);
        std::vector<std::pair<std::wstring, std::wstring>> probe;
        if (treg.Info(tpl, &probe) != SbieStatus::OK)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"template '" + tpl + L"' not found");
        SbieStatus ts = svc.IniSetSetting(
            name, L"Template", tpl, svc::SvcClient::SetMode::Append, true, pw);
        if (ts != SbieStatus::OK)
            return EmitError(ctx.opts, ts,
                             L"failed to apply template '" + tpl + L"'"
                             + boxproc::PasswordHint(ts, pw));
    }

    EmitMessage(ctx.opts, L"box '" + name + L"' created");
    return 0;
}

void RegisterBoxCreateCommands()
{
    Commands()["box"]["create"] = CmdBoxCreate;
}

} // namespace sbie::cli
