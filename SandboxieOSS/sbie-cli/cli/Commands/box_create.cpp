// Sandboxie-OSS — sbie-cli/cli/Commands/box_create.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box create <name> [--template <tpl>]… [--type <t>]（04 §4.3；波 B
// 07-P1-1：--type 预设）
//   ValidateName → Create（SET Enabled=y，不建目录）→ --type 预设键组 →
//   可选 Apply 模板。
// sbie box types（波 B 辅助命令）：列出 --type 预设与键组说明（纯本地表，
//   无 server/driver 依赖）。
// 退出码：0；7=名字/类型非法；5=已存在；6=密码/权限；4=SbieSvc 不可用。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/BoxTransfer.h"
#include "Model/Templates.h"

#include "Util/TablePrinter.h"

namespace sbie::cli {

int CmdBoxCreate(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box create <name> [--type <t>] "
                         L"[--template <tpl>]...");

    const std::wstring name = pos[0];

    // 提前给出精确的非法名诊断（Model 层只回 INVALID）
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid box name '" + name + L"' (max 38 chars of "
                         L"A-Z a-z 0-9 _; reserved words excluded)");

    // --type 预设（07-P1-1；缺省 = 现行为 Enabled=y）
    std::wstring typeStr = boxproc::OptionValue(ctx.args, L"--type");
    const model::BoxTypePreset* preset = nullptr;
    if (!typeStr.empty()) {
        preset = model::FindBoxTypePreset(typeStr);
        if (!preset)
            return EmitError(ctx.opts, SbieStatus::INVALID,
                             L"unknown box type '" + typeStr
                             + L"' (see: sbie-cli box types)");
    }

    // 写路径（非幂等，retry=false；params.password = P0-11 锁配置写凭据；
    // params.type = 07-P1-1 预设）
    {
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        if (preset)
            ipcroute::PSet(&params, L"type", std::wstring(preset->type));
        json::JsonValue tpls = json::JsonValue::Array();
        for (size_t i = 0; i + 1 < ctx.args.size(); ++i)
            if (ctx.args[i] == L"--template")
                tpls.pushBack(json::JsonValue(ctx.args[i + 1]));
        params.set(L"templates", std::move(tpls));
        const std::wstring okMsg = preset
            ? L"box '" + name + L"' created (type: "
                  + std::wstring(preset->type) + L")"
            : L"box '" + name + L"' created";
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxCreate, params, false,
            [okMsg](const GlobalOptions& o, const json::JsonValue& data) {
                if (o.json) {
                    json::JsonValue d = data.isObject() ? data
                                                        : json::JsonValue::
                                                              Object();
                    if (!d.find(L"message"))
                        d.set(L"message", json::JsonValue(okMsg));
                    EmitJsonOk(o, d);
                    return 0;
                }
                EmitMessage(o, okMsg);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware）：Create = SET Enabled=y（QSbieAPI CreateBox 同语义，
    // 04 §8.4——不建目录）；--type 预设键组；模板激活 = Append Template=<名>
    // + 存在性检查（与 server HBoxCreate / template apply 同序）
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

    // --type 预设键组（Enabled 之后；失败即中止——半建箱与 server 路径同态）
    if (preset) {
        st = model::ApplyBoxTypeKeys(name, *preset, pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box create failed applying type '"
                                 + std::wstring(preset->type) + L"'"
                                 + boxproc::PasswordHint(st, pw));
    }

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

    EmitMessage(ctx.opts,
                preset ? L"box '" + name + L"' created (type: "
                             + std::wstring(preset->type) + L")"
                       : L"box '" + name + L"' created");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box types（07-P1-1 辅助）：预设表（本地，无 IO）
// ---------------------------------------------------------------------------

int CmdBoxTypes(const CommandContext& ctx)
{
    // 纯文档面命令：直接渲染 Model 预设表（server 无需 op——两侧同表）
    const std::vector<model::BoxTypePreset>& table =
        model::BoxTypePresets();

    util::TablePrinter t;
    t.AddColumn(L"TYPE");
    t.AddColumn(L"DESCRIPTION");
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::BoxTypePreset& p : table) {
        // 键组摘要（DESCRIPTION 已含键组；单列化展示）
        t.AddRow({ p.type, p.label });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"type", json::JsonValue(p.type));
        r.set(L"description", json::JsonValue(p.label));
        json::JsonValue keys = json::JsonValue::Array();
        for (const model::BoxTypeKey& k : p.keys) {
            std::wstring one = k.key + std::wstring(L"=") + k.value;
            if (k.append)
                one += L" (append)";
            keys.pushBack(json::JsonValue(one));
        }
        r.set(L"keys", std::move(keys));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"no box types");
    return 0;
}

void RegisterBoxCreateCommands()
{
    Commands()["box"]["create"] = CmdBoxCreate;
    Commands()["box"]["types"] = CmdBoxTypes;
}

} // namespace sbie::cli
