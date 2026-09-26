// Sandboxie-OSS — sbie-cli/cli/Commands/template_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie template list|info|apply|revoke|check（04-modules.md §4.6 全部）。
// 读（list/info/check 的枚举）：TemplateRegistry（直读安装目录 Templates.ini
// + Sandboxie.ini 本地 [Template_*]；可降级直连）；写（apply/revoke）：经
// SbieSvc IniSetSetting（04 §5 不可降级列）——带 --password/SBIE_PASS 直调，
// 无密码时走 registry.Apply/Revoke（server 波次 Dispatcher 复用同实现）。

#include "CfgTemplateCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/DriverApi/DriverApi.h"
#include "../../../SbieCore/Model/Templates.h"
#include "../../../SbieCore/SvcClient/SvcClient.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace sbie::cli {

namespace {

// 密码优先级（04 §3）：--password > SBIE_PASS 环境变量
std::wstring ResolvePassword(const GlobalOptions& opts)
{
    if (!opts.password.empty())
        return opts.password;
    wchar_t buf[256] = L"";
    DWORD n = GetEnvironmentVariableW(L"SBIE_PASS", buf, 256);
    if (n > 0 && n < 256)
        return buf;
    return L"";
}

svc::SvcClient& Svc()
{
    return svc::SvcClient::Instance();
}

// box 是否存在（不区分启用态；drv::IsBoxEnabled 三态语义）
bool BoxExists(const std::wstring& box)
{
    bool enabled = false, exists = false;
    if (!Ok(drv::IsBoxEnabled(box, &enabled, &exists)))
        return false;
    return exists;
}

int RequireBox(const CommandContext& ctx, const std::wstring& box)
{
    if (!drv::LoadSbieDll(ctx.opts.sbieDllPath))
        return EmitError(ctx.opts, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!BoxExists(box))
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box not found: " + box);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie template list [--class <c>]（04 §4.6；表：NAME CLASS DESCRIPTION）
// ---------------------------------------------------------------------------

int CmdTemplateList(const CommandContext& ctx)
{
    srvconn::NoteDegraded();

    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring clazz = L"*";
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == L"--class" && i + 1 < rest.size()) {
            clazz = rest[++i];
        } else if (!rest[i].empty() && rest[i][0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + rest[i]);
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + rest[i]);
        }
    }

    // IPC 优先（tpl.list；本构建 server op 占位 → 降级直连本地枚举）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"class", clazz);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTplList, params, true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                return ipcroute::RenderRows(
                    op,
                    { { L"NAME", L"name" },
                      { L"CLASS", L"class" },
                      { L"DESCRIPTION", L"description" } },
                    data, L"no templates");
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::TemplateRegistry registry(nullptr, Svc());
    std::vector<model::TemplateInfo> list = registry.List(clazz);

    util::TablePrinter t;
    t.AddColumn(L"NAME");
    t.AddColumn(L"CLASS");
    t.AddColumn(L"DESCRIPTION");
    json::JsonValue rows = json::JsonValue::Array();
    for (const auto& ti : list) {
        t.AddRow({ ti.name, ti.clazz.empty() ? L"-" : ti.clazz,
                   ti.descr });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"name", json::JsonValue(ti.name));
        r.set(L"class", json::JsonValue(ti.clazz));
        r.set(L"description", json::JsonValue(ti.descr));
        rows.pushBack(std::move(r));
    }
    EmitRows(o, t, rows, L"no templates");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie template info <name>（04 §4.6；键值行）
// ---------------------------------------------------------------------------

int CmdTemplateInfo(const CommandContext& ctx)
{
    srvconn::NoteDegraded();

    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring name;
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        } else if (name.empty()) {
            name = a;
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
        }
    }
    if (name.empty())
        return EmitError(o, SbieStatus::USAGE, L"usage: sbie template info <name>");

    // IPC 优先（tpl.info；本构建 server op 占位 → 降级直连）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTplInfo, params, true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                if (op.json) {
                    EmitJsonOk(op, data);
                    return 0;
                }
                // data.settings = [{key,value}]（04 §4.6 键值行）
                std::vector<std::pair<std::wstring, std::wstring>> kv;
                if (const json::JsonValue* s = data.isObject()
                        ? data.find(L"settings") : nullptr;
                    s && s->isArray()) {
                    for (const json::JsonValue& e : s->items()) {
                        const json::JsonValue* k = e.isObject()
                            ? e.find(L"key") : nullptr;
                        const json::JsonValue* v = e.isObject()
                            ? e.find(L"value") : nullptr;
                        if (k && k->isString() && v && v->isString())
                            kv.emplace_back(k->asString(), v->asString());
                    }
                }
                EmitKv(op, kv, json::JsonValue::Object());
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::TemplateRegistry registry(nullptr, Svc());
    std::vector<std::pair<std::wstring, std::wstring>> settings;
    SbieStatus st = registry.Info(name, &settings);
    if (!Ok(st))
        return EmitError(o, st, L"template not found: " + name);

    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const auto& kv : settings) {
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"key", json::JsonValue(kv.first));
            r.set(L"value", json::JsonValue(kv.second));
            arr.pushBack(std::move(r));
        }
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"name", json::JsonValue(name));
        data.set(L"settings", std::move(arr));
        EmitJsonOk(o, data);
        return 0;
    }
    EmitKv(o, settings, json::JsonValue::Object());
    return 0;
}

// ---------------------------------------------------------------------------
// sbie template apply <box> <name>（04 §4.6；box 节 Append Template=<名>）
// ---------------------------------------------------------------------------

int CmdTemplateApply(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring box, tmpl;
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        } else if (box.empty()) {
            box = a;
        } else if (tmpl.empty()) {
            tmpl = a;
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
        }
    }
    if (box.empty() || tmpl.empty())
        return EmitError(o, SbieStatus::USAGE,
                         L"usage: sbie template apply <box> <name>");

    // 写路径（非幂等，retry=false；params.password = P0-11 锁配置写凭据）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"box", box);
        ipcroute::PSet(&params, L"name", tmpl);
        const std::wstring pw = ResolvePassword(o);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTplApply, params, false,
            [](const GlobalOptions& op, const json::JsonValue&) {
                EmitMessage(op, L"applied");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    int rc = RequireBox(ctx, box);
    if (rc != 0)
        return rc;

    model::TemplateRegistry registry(nullptr, Svc());
    std::vector<std::pair<std::wstring, std::wstring>> probe;
    if (!Ok(registry.Info(tmpl, &probe)))
        return EmitError(o, SbieStatus::NOT_FOUND,
                         L"template not found: " + tmpl);

    if (!Svc().Connected())
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"template apply requires SbieSvc (write path)");

    SbieStatus st = Svc().IniSetSetting(
        box, L"Template", tmpl, svc::SvcClient::SetMode::Append, true,
        ResolvePassword(o));
    if (!Ok(st))
        return EmitError(o, st,
                         L"apply failed (locked config? use --password)");
    EmitMessage(o, L"applied");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie template revoke <box> <name>（04 §4.6；删 box 节该 Template 值）
// ---------------------------------------------------------------------------

int CmdTemplateRevoke(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring box, tmpl;
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        } else if (box.empty()) {
            box = a;
        } else if (tmpl.empty()) {
            tmpl = a;
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
        }
    }
    if (box.empty() || tmpl.empty())
        return EmitError(o, SbieStatus::USAGE,
                         L"usage: sbie template revoke <box> <name>");

    // 写路径（非幂等，retry=false；params.password = P0-11）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"box", box);
        ipcroute::PSet(&params, L"name", tmpl);
        const std::wstring pw = ResolvePassword(o);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTplRevoke, params, false,
            [](const GlobalOptions& op, const json::JsonValue&) {
                EmitMessage(op, L"revoked");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    int rc = RequireBox(ctx, box);
    if (rc != 0)
        return rc;

    model::TemplateRegistry registry(nullptr, Svc());
    bool applied = false;
    for (const auto& v : registry.Applied(box))
        if (v == tmpl)
            applied = true;
    if (!applied)
        return EmitError(o, SbieStatus::NOT_FOUND,
                         L"template not active on box " + box + L": " + tmpl);

    if (!Svc().Connected())
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"template revoke requires SbieSvc (write path)");

    SbieStatus st = Svc().IniSetSetting(
        box, L"Template", tmpl, svc::SvcClient::SetMode::Delete, true,
        ResolvePassword(o));
    if (!Ok(st))
        return EmitError(o, st,
                         L"revoke failed (locked config? use --password)");
    EmitMessage(o, L"revoked");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie template check <box>（04 §4.6；表：TEMPLATE SOURCE）
// 与 Sandboxie.ini 已激活模板对照（任务书）：来源三档——
//   config          box 节自身的 Template= 值（盘上 Sandboxie.ini，驱动缓存同读）
//   DefaultTemplates [DefaultTemplates] 节（conf.c:1166-1175 并入 GlobalSettings）
//   GlobalSettings  [GlobalSettings] 节自身的 Template= 值（设置回退进 box）
// ---------------------------------------------------------------------------

int CmdTemplateCheck(const CommandContext& ctx)
{
    srvconn::NoteDegraded();

    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring box;
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        } else if (box.empty()) {
            box = a;
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
        }
    }
    if (box.empty())
        return EmitError(o, SbieStatus::USAGE,
                         L"usage: sbie template check <box>");

    // IPC 优先（tpl.check；本构建 server op 占位 → 降级直连本地对照读盘）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"box", box);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTplCheck, params, true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                return ipcroute::RenderRows(
                    op,
                    { { L"TEMPLATE", L"template" },
                      { L"SOURCE", L"source" } },
                    data, L"no templates active");
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    int rc = RequireBox(ctx, box);
    if (rc != 0)
        return rc;

    model::TemplateRegistry registry(nullptr, Svc());
    std::vector<model::TemplateInfo> catalog = registry.List(L"*");
    auto known = [&](const std::wstring& n) {
        for (const auto& ti : catalog)
            if (_wcsicmp(ti.name.c_str(), n.c_str()) == 0)
                return true;
        return false;
    };

    // 来源对照读盘上 Sandboxie.ini（GetPrivateProfileSectionW 原生支持
    // UTF-16 ini；驱动缓存的 box 查询带 GlobalSettings 回退，不能用于分档）
    std::wstring iniPath;
    bool iniIsHome = false;
    if (!svc::SvcClient::Instance().Connected()
        || !Ok(svc::SvcClient::Instance().IniGetPath(&iniPath, &iniIsHome))) {
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"template check requires SbieSvc to locate "
                         L"Sandboxie.ini");
    }

    struct Row { std::wstring tmpl, source; bool exists; };
    std::vector<Row> rows;
    auto collect = [&](const wchar_t* section, const wchar_t* source) {
        std::vector<wchar_t> buf(32768);
        DWORD n = GetPrivateProfileSectionW(section, buf.data(),
                                             (DWORD)buf.size(), iniPath.c_str());
        for (DWORD p = 0; p < n;) {
            std::wstring kv = buf.data() + p;
            p += (DWORD)kv.size() + 1;
            const std::wstring kEq = L"Template=";
            if (_wcsnicmp(kv.c_str(), kEq.c_str(), kEq.size()) != 0)
                continue;
            std::wstring v = kv.substr(kEq.size());
            if (v.empty())
                continue;
            bool dup = false;
            for (const auto& r : rows)
                if (_wcsicmp(r.tmpl.c_str(), v.c_str()) == 0)
                    dup = true;   // 先入档优先（box config > Default > Global）
            if (!dup)
                rows.push_back({ v, source, known(v) });
        }
    };
    collect(box.c_str(), L"config");
    collect(L"DefaultTemplates", L"DefaultTemplates");
    collect(L"GlobalSettings", L"GlobalSettings");

    util::TablePrinter t;
    t.AddColumn(L"TEMPLATE");
    t.AddColumn(L"SOURCE");
    json::JsonValue jrows = json::JsonValue::Array();
    for (const auto& r : rows) {
        t.AddRow({ r.tmpl, r.source });
        json::JsonValue j = json::JsonValue::Object();
        j.set(L"template", json::JsonValue(r.tmpl));
        j.set(L"source", json::JsonValue(r.source));
        j.set(L"exists", json::JsonValue(r.exists));
        jrows.pushBack(std::move(j));
    }
    EmitRows(o, t, jrows, L"no templates active");
    return 0;
}

} // namespace sbie::cli
