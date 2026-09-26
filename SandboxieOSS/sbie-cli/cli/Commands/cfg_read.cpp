// Sandboxie-OSS — sbie-cli/cli/Commands/cfg_read.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie cfg get / cfg path（04-modules.md §4.5 读路径；§5 可降级直连表内）。
// 接线波次：IPC 优先（cfg.get / cfg.path op）+ 降级直连
//（ipcroute::Invoke）；注册由 Commands.cpp 的 RegisterCommands() 末尾调用
// RegisterCfgTemplateCommands() 完成（原 SBIE_CLI_DIRECT 过渡分发器已随
// 接线完成删除）。

#include "CfgTemplateCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/DriverApi/DriverApi.h"
#include "../../../SbieCore/Model/ConfigStore.h"
#include "../../../SbieCore/SvcClient/SvcClient.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace sbie::cli {

namespace {

// 组名之后透传的命令专属选项解析（04 §3：- 前缀 token 不再按全局选项报错）
bool MatchFlag(const std::wstring& a, const wchar_t* name)
{
    return a == name;
}

// 驱动侧"节/值不存在"= STATUS_RESOURCE_NAME_NOT_FOUND=0xC000008B
// （conf.c:1885-1887；常数核对 Windows SDK 10.0.26100.0 ntstatus.h:3354），
// FromNtStatus 未列该值（折叠 GENERIC）——本命令集经 LastNtStatus 精确判别；
// DriverApi/Status 层的修正归其维护 agent（多 agent 边界）。
constexpr LONG kStatusResourceNameNotFound = 0xC000008BL;

bool ValueMissing()
{
    return drv::LastNtStatus() == kStatusResourceNameNotFound;
}

// index 递增收集全部值（QueryConfList 同构；缺失即止，不误报）
SbieStatus CollectValues(const std::wstring& section, const std::wstring& setting,
                         bool noExpand, std::vector<std::wstring>* values)
{
    values->clear();
    for (ULONG i = 0; i < 512; ++i) {
        std::wstring v;
        SbieStatus st = drv::QueryConfText(section, setting, i, noExpand, true, &v);
        if (st == SbieStatus::OK) {
            if (v.empty())
                break;   // 越过末尾（驱动返回空串）
            values->push_back(std::move(v));
            continue;
        }
        if (ValueMissing())
            return SbieStatus::OK;
        return st;
    }
    return SbieStatus::OK;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie cfg get <setting> [--section <s>] [--index <i>]（04 §4.5）
// ---------------------------------------------------------------------------

int CmdCfgGet(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring setting, section = L"GlobalSettings";
    bool haveIndex = false;
    bool raw = false;
    unsigned long index = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (MatchFlag(rest[i], L"--section") && i + 1 < rest.size()) {
            section = rest[++i];
        } else if (MatchFlag(rest[i], L"--index") && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            index = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0')
                return EmitError(o, SbieStatus::USAGE, L"invalid --index value");
            haveIndex = true;
        } else if (MatchFlag(rest[i], L"--raw")) {
            raw = true;   // P1-5：与 box get 对齐（缺省展开 %env%，--raw 关闭）
        } else if (!rest[i].empty() && rest[i][0] == L'-') {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + rest[i]);
        } else if (setting.empty()) {
            setting = rest[i];
        } else {
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + rest[i]);
        }
    }
    if (setting.empty())
        return EmitError(o, SbieStatus::USAGE,
                         L"usage: sbie cfg get <setting> [--section <s>] "
                         L"[--index <i>] [--raw]");

    // IPC 优先（cfg.get；data = 值数组）。noexpand 显式发送（P1-5：两路径
    // 展开/不展开语义一致），降级直连
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"setting", setting);
        ipcroute::PSet(&params, L"section", section);
        ipcroute::PSet(&params, L"noexpand", raw);
        if (haveIndex)
            ipcroute::PSet(&params, L"index", (long long)index);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpCfgGet, params, true,
            [&setting, &section](const GlobalOptions& op,
                                 const json::JsonValue& data) {
                std::vector<std::wstring> values;
                if (data.isArray())
                    for (const json::JsonValue& v : data.items())
                        if (v.isString())
                            values.push_back(v.asString());
                if (op.json) {
                    json::JsonValue arr = json::JsonValue::Array();
                    for (auto& v : values)
                        arr.pushBack(json::JsonValue(v));
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"section", json::JsonValue(section));
                    d.set(L"setting", json::JsonValue(setting));
                    d.set(L"values", std::move(arr));
                    EmitJsonOk(op, d);
                    return 0;
                }
                for (const auto& v : values)
                    util::PrintLineUtf8(util::WideToUtf8(v));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();

    if (!drv::LoadSbieDll(o.sbieDllPath))
        return EmitError(o, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!drv::DriverAlive())
        return EmitError(o, SbieStatus::DRIVER_UNAVAILABLE, L"driver not running");
    if (setting.size() > 64 || section.size() > 64)
        return EmitError(o, SbieStatus::INVALID,
                         L"section/setting name exceeds 64 characters");

    std::vector<std::wstring> values;
    if (haveIndex) {
        std::wstring v;
        SbieStatus st = drv::QueryConfText(section, setting, index, raw, true, &v);
        if (!Ok(st)) {
            if (ValueMissing())
                return EmitError(o, SbieStatus::NOT_FOUND,
                                 L"setting not found: [" + section + L"] "
                                 + setting);
            return EmitError(o, st, L"query failed for [" + section + L"] "
                                         + setting);
        }
        values.push_back(std::move(v));
    } else {
        SbieStatus st = CollectValues(section, setting, raw, &values);
        if (!Ok(st))
            return EmitError(o, st, L"query failed for [" + section + L"] "
                                         + setting);
        if (values.empty())
            return EmitError(o, SbieStatus::NOT_FOUND,
                             L"setting not found: [" + section + L"] " + setting);
    }

    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (auto& v : values)
            arr.pushBack(json::JsonValue(v));
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"section", json::JsonValue(section));
        data.set(L"setting", json::JsonValue(setting));
        data.set(L"values", std::move(arr));
        EmitJsonOk(o, data);
        return 0;
    }
    for (const auto& v : values)
        util::PrintLineUtf8(util::WideToUtf8(v));
    return 0;
}

// ---------------------------------------------------------------------------
// sbie cfg path（04 §4.5：经 SbieSvc IniGetPath；无降级——SbieSvc 不可达则 4）
// ---------------------------------------------------------------------------

int CmdCfgPath(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-')
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
    }

    // IPC 优先（cfg.path → server 经 SvcProxy 问 SbieSvc；server 缺席时本
    // 进程直连 SbieSvc——两侧同一 IniGetPath）
    {
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpCfgPath, json::JsonValue::Object(), true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                const std::wstring path = data.isObject()
                    ? (data.find(L"path") && data.find(L"path")->isString()
                           ? data.find(L"path")->asString()
                           : std::wstring())
                    : std::wstring();
                const bool isHome = data.isObject() && data.find(L"home")
                    && data.find(L"home")->type()
                           == json::JsonValue::Type::Bool
                    ? data.find(L"home")->asBool() : false;
                if (op.json) {
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"path", json::JsonValue(path));
                    d.set(L"is_home", json::JsonValue(isHome));
                    d.set(L"location",
                          json::JsonValue(isHome ? L"home" : L"system"));
                    EmitJsonOk(op, d);
                    return 0;
                }
                util::PrintLineUtf8(util::WideToUtf8(
                    path + L" ("
                    + (isHome ? std::wstring(L"home")
                              : std::wstring(L"system"))
                    + L")"));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"cfg path requires SbieSvc (service) via the "
                         L"sbie-cli server; neither is reachable");

    std::wstring path;
    bool isHome = false;
    SbieStatus st = svc.IniGetPath(&path, &isHome);
    if (!Ok(st))
        return EmitError(o, st, L"SbieSvc IniGetPath failed");

    if (o.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"path", json::JsonValue(path));
        data.set(L"is_home", json::JsonValue(isHome));
        data.set(L"location", json::JsonValue(isHome ? L"home" : L"system"));
        EmitJsonOk(o, data);
        return 0;
    }
    util::PrintLineUtf8(util::WideToUtf8(
        path + L" (" + (isHome ? std::wstring(L"home") : std::wstring(L"system"))
        + L")"));
    return 0;
}

// ---------------------------------------------------------------------------
// sbie cfg list-setting [section]（P0-9；04 §4.5：枚举 setting 名，缺省
// GlobalSettings）。server op cfg.listSetting 已实现（Dispatcher HCfgListSetting）
// ——本波次接 client 命令（此前为桩）；IPC 优先 + 降级直连
// ConfigStore::ListSettings（驱动 Conf_Get_Setting_Name 枚举）
// ---------------------------------------------------------------------------

namespace {

int RenderSettingNames(const GlobalOptions& o,
                       const std::vector<std::wstring>& names)
{
    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const std::wstring& n : names)
            arr.pushBack(json::JsonValue(n));
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"settings", std::move(arr));
        d.set(L"count", json::JsonValue((long long)names.size()));
        EmitJsonOk(o, d);
        return 0;
    }
    if (names.empty() && !o.quiet)
        util::PrintLineUtf8(util::WideToUtf8(L"no settings"));
    else
        for (const std::wstring& n : names)
            util::PrintLineUtf8(util::WideToUtf8(n));
    return 0;
}

} // namespace

int CmdCfgListSetting(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    std::wstring section = L"GlobalSettings";
    bool haveSection = false;
    for (const auto& a : rest) {
        if (!a.empty() && a[0] == L'-')
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        if (haveSection)
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
        section = a;
        haveSection = true;
    }

    // IPC 优先（cfg.listSetting；data = setting 名数组）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"section", section);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpCfgListSetting, params, true,
            [](const GlobalOptions& op, const json::JsonValue& data) {
                std::vector<std::wstring> names;
                if (data.isArray())
                    for (const json::JsonValue& v : data.items())
                        if (v.isString())
                            names.push_back(v.asString());
                return RenderSettingNames(op, names);
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    if (!drv::LoadSbieDll(o.sbieDllPath))
        return EmitError(o, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!drv::DriverAlive())
        return EmitError(o, SbieStatus::DRIVER_UNAVAILABLE, L"driver not running");

    return RenderSettingNames(o, model::ConfigStore().ListSettings(section));
}

// ---------------------------------------------------------------------------
// 注册（由 Commands.cpp 的 RegisterCommands() 末尾调用；幂等）
// ---------------------------------------------------------------------------

void RegisterCfgTemplateCommands()
{
    auto& reg = Commands();
    auto sub = [&](const char* g, const char* s, CommandHandler h) {
        reg[g][s] = h;
    };
    // cfg（04 §4.5；set/unset/lock/unlock → cfg_set.cpp）
    sub("cfg", "get", CmdCfgGet);
    sub("cfg", "path", CmdCfgPath);
    sub("cfg", "reload", CmdCfgReload);
    sub("cfg", "list-setting", CmdCfgListSetting);
    // template（04 §4.6 全部）
    sub("template", "list", CmdTemplateList);
    sub("template", "info", CmdTemplateInfo);
    sub("template", "apply", CmdTemplateApply);
    sub("template", "revoke", CmdTemplateRevoke);
    sub("template", "check", CmdTemplateCheck);
    // log（04 §4.7；messages 为 dump 的任务书别名）
    sub("log", "watch", CmdLogWatch);
    sub("log", "dump", CmdLogDump);
    sub("log", "messages", CmdLogDump);
}

} // namespace sbie::cli
