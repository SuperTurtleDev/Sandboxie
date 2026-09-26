// Sandboxie-OSS — sbie-cli/cli/Commands/box_manage.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box info|get|set|list-setting|rename|delete|enable|disable|clean|size
//（04 §4.3）。
//   info / get / list-setting：驱动缓存读（04 §5 可降级直连）。
//   set / rename / delete / enable / disable / clean：SbieSvc 写路径 / 文件
//   操作（Model 层落盘 + refresh）。
//   size：递归统计 FileRoot（Model ScanBoxSize，纯文件读；06 §P0-5）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/ConfigStore.h"
#include "Model/Snapshots.h"
#include "Model/BoxUsage.h"
#include "Util/TablePrinter.h"

// vendor：CONF_GET_PROPERTY / CONF_GET_NO_* 标志（list-setting 枚举）
#include "api_flags.h"

#include <windows.h>

#include <cstdio>

namespace sbie::cli {

namespace {

model::BoxRepository MakeRepo()
{
    return model::BoxRepository(nullptr, svc::SvcClient::Instance());
}

// box.info 的 IPC data → 本地补算 has_snapshots（纯文件事实，client 侧
// SnapshotManager 与 server 侧同实现）
int RenderBoxInfoIpc(const GlobalOptions& o, const json::JsonValue& data)
{
    model::BoxInfo bi;
    if (const json::JsonValue* v = data.find(L"name"); v && v->isString())
        bi.name = v->asString();
    if (const json::JsonValue* v = data.find(L"file_root"); v && v->isString())
        bi.fileRoot = v->asString();
    model::SnapshotManager sm(bi);
    const bool hasSnaps = sm.HasAny();

    json::JsonValue obj = data.isObject() ? data : json::JsonValue::Object();
    obj.set(L"has_snapshots", json::JsonValue(hasSnaps));
    return ipcroute::RenderKv(
        o,
        { { L"name", L"name" },
          { L"enabled", L"enabled" },
          { L"file_root", L"file_root" },
          { L"reg_root", L"reg_root" },
          { L"ipc_root", L"ipc_root" },
          { L"has_processes", L"has_processes" },
          { L"has_snapshots", L"has_snapshots" } },
        obj);
}

} // namespace

// ---------------------------------------------------------------------------
// sbie box info <name> [--paths]
// ---------------------------------------------------------------------------

int CmdBoxInfo(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box info <name> [--paths]");
    // --paths：表格恒含三根路径（对齐 04 §4.3 输出列），接受该 flag 以兼容

    // IPC 优先（box.info），降级直连
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", pos[0]);
        ipcroute::Result r = ipcroute::Invoke(ctx.opts, ipc::kOpBoxInfo,
                                              params, true, RenderBoxInfoIpc);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(pos[0], &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + pos[0] + L"' not found");

    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"name", bi.name },
        { L"enabled", bi.enabled ? L"yes" : L"no" },
        { L"file_root", bi.fileRoot },
        { L"reg_root", bi.regRoot },
        { L"ipc_root", bi.ipcRoot },
        { L"has_processes", bi.hasProcesses ? L"yes" : L"no" },
    };
    // 快照能力是纯文件事实，顺带报告（HasAny 读 <fileRoot>\Snapshots.ini）
    model::SnapshotManager sm(bi);
    kv.push_back({ L"has_snapshots", sm.HasAny() ? L"yes" : L"no" });

    json::JsonValue obj = json::JsonValue::Object();
    obj.set(L"name", json::JsonValue(bi.name));
    obj.set(L"enabled", json::JsonValue(bi.enabled));
    obj.set(L"file_root", json::JsonValue(bi.fileRoot));
    obj.set(L"reg_root", json::JsonValue(bi.regRoot));
    obj.set(L"ipc_root", json::JsonValue(bi.ipcRoot));
    obj.set(L"has_processes", json::JsonValue(bi.hasProcesses));
    obj.set(L"has_snapshots", json::JsonValue(sm.HasAny()));

    EmitKv(ctx.opts, kv, obj);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box get <name> <setting> [--index <i>] [--raw]
// ---------------------------------------------------------------------------

int CmdBoxGet(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box get <name> <setting> "
                         L"[--index <i>] [--raw]");

    const std::wstring& name = pos[0];
    const std::wstring& setting = pos[1];

    // IPC 优先（box.get；data = 值数组）。noexpand 显式发送（P1-5 消除两路
    // 径展开语义漂移）：缺省 false = 展开 %env%（与直连 --raw 缺省一致），
    // --raw → noexpand=true
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"setting", setting);
        ipcroute::PSet(&params, L"noexpand",
                       boxproc::HasFlag(ctx.args, L"--raw"));
        if (boxproc::HasFlag(ctx.args, L"--index")) {
            const std::wstring idxStr =
                boxproc::OptionValue(ctx.args, L"--index");
            ULONG idx = 0;
            if (!boxproc::ParseUlong(idxStr, &idx))
                return EmitError(ctx.opts, SbieStatus::USAGE,
                                 L"bad --index value: " + idxStr);
            ipcroute::PSet(&params, L"index", (long long)idx);
        }
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxGet, params, true,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                std::vector<std::wstring> vals;
                if (data.isArray())
                    for (const json::JsonValue& v : data.items())
                        if (v.isString())
                            vals.push_back(v.asString());
                if (vals.empty()) {
                    if (o.json)
                        EmitJsonOk(o, json::JsonValue::Array());
                    return 0;
                }
                if (vals.size() == 1) {
                    EmitValue(o, vals[0], json::JsonValue(vals[0]));
                    return 0;
                }
                if (o.json) {
                    json::JsonValue d = json::JsonValue::Object();
                    json::JsonValue arr = json::JsonValue::Array();
                    for (const std::wstring& v : vals)
                        arr.pushBack(json::JsonValue(v));
                    d.set(L"values", std::move(arr));
                    d.set(L"count",
                          json::JsonValue((long long)vals.size()));
                    EmitJsonOk(o, d);
                } else {
                    for (const std::wstring& v : vals)
                        util::PrintLineUtf8(util::WideToUtf8(v));
                }
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + name + L"' not found");

    model::ConfigStore cfg;
    const bool raw = boxproc::HasFlag(ctx.args, L"--raw");

    if (boxproc::HasFlag(ctx.args, L"--index")) {
        const std::wstring idxStr = boxproc::OptionValue(ctx.args, L"--index");
        ULONG idx = 0;
        if (!boxproc::ParseUlong(idxStr, &idx))
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"bad --index value: " + idxStr);
        auto v = cfg.Get(name, setting, idx, raw, true);
        if (!v.has_value())
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"setting '" + setting + L"' index "
                             + std::to_wstring(idx) + L" not found");
        json::JsonValue val = json::JsonValue(*v);
        EmitValue(ctx.opts, *v, val);
        return 0;
    }

    std::vector<std::wstring> vals = cfg.GetList(name, setting, raw, true);
    if (vals.empty())
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"setting '" + setting + L"' not found");
    if (vals.size() == 1) {
        json::JsonValue val = json::JsonValue(vals[0]);
        EmitValue(ctx.opts, vals[0], val);
        return 0;
    }
    if (ctx.opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        json::JsonValue arr = json::JsonValue::Array();
        for (const std::wstring& v : vals)
            arr.pushBack(json::JsonValue(v));
        data.set(L"values", std::move(arr));
        data.set(L"count", json::JsonValue((long long)vals.size()));
        EmitJsonOk(ctx.opts, data);
    } else {
        for (const std::wstring& v : vals)
            util::PrintLineUtf8(util::WideToUtf8(v));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box set <name> <setting> <value> [--append|--insert|--index <i>]
// ---------------------------------------------------------------------------

int CmdBoxSet(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 3)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box set <name> <setting> <value> "
                         L"[--append|--insert|--index <i>]");

    const std::wstring& name = pos[0];
    const std::wstring& setting = pos[1];
    // 值 = 其余位置参数以空格连接（未加引号的多词值）
    std::wstring value;
    for (size_t i = 2; i < pos.size(); ++i) {
        if (!value.empty())
            value += L' ';
        value += pos[i];
    }

    // 写路径（非幂等，retry=false）。params：mode + index（P0-10：--index
    // 此前不发 → server 走整 setting 替换，多值静默丢失）+ password
    // （P0-11：锁配置写此前以 server 侧空密码发 → WRONG_PASSWORD）+ refresh
    // （P1-6 同坑收口：--no-refresh 此前不进 params → IPC 路径恒热重载）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"setting", setting);
        ipcroute::PSet(&params, L"value", value);
        ipcroute::PSet(&params, L"refresh", !ctx.opts.noRefresh);
        if (boxproc::HasFlag(ctx.args, L"--append"))
            ipcroute::PSet(&params, L"mode", std::wstring(L"append"));
        else if (boxproc::HasFlag(ctx.args, L"--insert"))
            ipcroute::PSet(&params, L"mode", std::wstring(L"insert"));
        else
            ipcroute::PSet(&params, L"mode", std::wstring(L"update"));
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        if (boxproc::HasFlag(ctx.args, L"--index")) {
            const std::wstring idxStr =
                boxproc::OptionValue(ctx.args, L"--index");
            ULONG idx = 0;
            if (!boxproc::ParseUlong(idxStr, &idx))
                return EmitError(ctx.opts, SbieStatus::USAGE,
                                 L"bad --index value: " + idxStr);
            ipcroute::PSet(&params, L"index", (long long)idx);
        }
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxSet, params, false,
            [](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, L"set");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + name + L"' not found");

    model::ConfigStore cfg;
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    const bool refresh = !ctx.opts.noRefresh;

    if (boxproc::HasFlag(ctx.args, L"--index")) {
        // 组合语义：替换第 <i> 个值，其余重放（0x1811 替换全部 + 0x1812 追加）
        const std::wstring idxStr = boxproc::OptionValue(ctx.args, L"--index");
        ULONG idx = 0;
        if (!boxproc::ParseUlong(idxStr, &idx))
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"bad --index value: " + idxStr);
        std::vector<std::wstring> vals = cfg.GetList(name, setting);
        if (idx >= vals.size())
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"setting '" + setting + L"' has "
                             + std::to_wstring(vals.size())
                             + L" value(s); index " + std::to_wstring(idx)
                             + L" out of range");
        vals[idx] = value;
        st = cfg.Set(name, setting, vals[0], false, pw);
        for (size_t i = 1; i < vals.size() && st == SbieStatus::OK; ++i)
            st = cfg.SetAppend(name, setting, vals[i],
                               refresh && i + 1 == vals.size(), pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box set --index failed"
                             + std::wstring(boxproc::PasswordHint(st, pw)));
        boxproc::EmitSilentOk(ctx.opts, L"set");
        return 0;
    }

    if (boxproc::HasFlag(ctx.args, L"--append"))
        st = cfg.SetAppend(name, setting, value, refresh, pw);
    else if (boxproc::HasFlag(ctx.args, L"--insert"))
        st = cfg.SetInsert(name, setting, value, refresh, pw);
    else
        st = cfg.Set(name, setting, value, refresh, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box set failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));
    boxproc::EmitSilentOk(ctx.opts, L"set");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box list-setting <name> [--all]
// ---------------------------------------------------------------------------

int CmdBoxListSetting(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box list-setting <name> [--all]");

    const std::wstring& name = pos[0];
    const bool all = boxproc::HasFlag(ctx.args, L"--all");

    // IPC 优先（box.listSetting；data = setting 名数组）。no_tmpls 显式发送
    // （P1-4：--all 此前恒直连；server 已参数化 NO_TEMPLS 反向语义——
    // no_tmpls=false = 含模板注入项）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"no_tmpls", !all);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxListSetting, params, true,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                std::vector<std::wstring> names;
                if (data.isArray())
                    for (const json::JsonValue& v : data.items())
                        if (v.isString())
                            names.push_back(v.asString());
                if (o.json) {
                    json::JsonValue arr = json::JsonValue::Array();
                    for (const std::wstring& n : names)
                        arr.pushBack(json::JsonValue(n));
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"settings", std::move(arr));
                    d.set(L"count",
                          json::JsonValue((long long)names.size()));
                    EmitJsonOk(o, d);
                } else {
                    if (names.empty() && !o.quiet) {
                        util::PrintLineUtf8(util::WideToUtf8(L"no settings"));
                    } else {
                        for (const std::wstring& n : names)
                            util::PrintLineUtf8(util::WideToUtf8(n));
                    }
                }
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::BoxInfo bi;
    if (MakeRepo().GetInfo(name, &bi) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' not found");

    // 枚举 setting 名：section=<name>、setting=NULL、index=序号（驱动端
    // Conf_Get_Setting_Name，drv/conf.c:1555）；--all 去掉 NO_TEMPLS（含模板
    // 注入项）。CONF_GET_PROPERTY 不可用（节属性查询，坑 §8.16）
    std::vector<std::wstring> names;
    for (ULONG i = 0; i < 4096; ++i) {
        WCHAR buf[130] = L"";
        ULONG flags = CONF_GET_NO_EXPAND;
        if (!all)
            flags |= CONF_GET_NO_TEMPLS;
        (void)drv::ApiP()->SbieApi_QueryConf(name.c_str(), nullptr,
                                             i | flags, buf, sizeof(buf));
        if (buf[0] == L'\0')
            break;
        names.push_back(buf);
    }

    if (ctx.opts.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const std::wstring& n : names)
            arr.pushBack(json::JsonValue(n));
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"settings", std::move(arr));
        data.set(L"count", json::JsonValue((long long)names.size()));
        EmitJsonOk(ctx.opts, data);
    } else {
        if (names.empty() && !ctx.opts.quiet) {
            util::PrintLineUtf8(util::WideToUtf8(L"no settings"));
        } else {
            for (const std::wstring& n : names)
                util::PrintLineUtf8(util::WideToUtf8(n));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box rename <old> <new>
// ---------------------------------------------------------------------------

int CmdBoxRename(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box rename <old> <new>");

    const std::wstring& oldN = pos[0];
    const std::wstring& newN = pos[1];

    if (model::BoxRepository::ValidateName(newN) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid new box name '" + newN + L"' (max 38 chars "
                         L"of A-Z a-z 0-9 _; reserved words excluded)");

    // 写路径（非幂等，retry=false；params.password = P0-11 锁配置写凭据）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"old", oldN);
        ipcroute::PSet(&params, L"new", newN);
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxRename, params, false,
            [oldN, newN](const GlobalOptions& o, const json::JsonValue&) {
                EmitMessage(o, L"box '" + oldN + L"' renamed to '" + newN
                                + L"'");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware：Model BoxRepository::Rename 的等价转录——两步写均带
    // 密码；整节替换 + 删旧节，04 §8.15）
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    bool enabled = false, exists = false;
    if (drv::IsBoxEnabled(oldN, &enabled, &exists) != SbieStatus::OK
        || !exists)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + oldN + L"' not found, or '" + newN
                         + L"' already exists");
    if (drv::IsBoxEnabled(newN, &enabled, &exists) != SbieStatus::OK || exists)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + oldN + L"' not found, or '" + newN
                         + L"' already exists");

    model::ConfigStore cfg;
    std::vector<std::wstring> settings = cfg.ListSettings(oldN);
    std::wstring sectionData;
    for (const std::wstring& key : settings) {
        for (const std::wstring& v : cfg.GetList(oldN, key))
            sectionData += key + L"=" + v + L"\n";
    }
    if (sectionData.empty())
        sectionData = L"Enabled=y\n";   // 空节兜底（保持 Create 语义）

    SbieStatus st = cfg.Set(newN, L"", sectionData, false, pw);
    if (st == SbieStatus::OK) {
        st = cfg.Delete(oldN, L"*", std::nullopt, true, pw);
        if (st != SbieStatus::OK)
            cfg.Delete(newN, L"*", std::nullopt, false, pw); // 回滚新节
    }
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box rename failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    EmitMessage(ctx.opts, L"box '" + oldN + L"' renamed to '" + newN + L"'");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box delete <name> [--files] [--keep-section]
// ---------------------------------------------------------------------------

int CmdBoxDelete(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box delete <name> [--files] "
                         L"[--keep-section]");

    const std::wstring& name = pos[0];
    const bool delFiles = boxproc::HasFlag(ctx.args, L"--files");
    const bool delSection = !boxproc::HasFlag(ctx.args, L"--keep-section");
    if (!delFiles && !delSection)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"nothing to delete: use --files and/or remove "
                         L"--keep-section");

    // 写路径（非幂等，retry=false；params.password = P0-11）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"files", delFiles);
        ipcroute::PSet(&params, L"keep_section", !delSection);
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxDelete, params, false,
            [name](const GlobalOptions& o, const json::JsonValue&) {
                EmitMessage(o, L"box '" + name + L"' deleted");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware）：检查 + 目录删除走 Model（节删拆出——ConfigStore 的
    // Delete 带 password 形参，Model BoxRepository::Delete 内部固定空密码）
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    SbieStatus st = MakeRepo().Delete(name, delFiles, false);
    if (st == SbieStatus::BOX_BUSY)
        return EmitError(ctx.opts, SbieStatus::BOX_BUSY,
                         L"box '" + name + L"' has running processes; "
                         L"terminate them first (proc kill-all / proc kill)");
    if (st == SbieStatus::ACCESS_DENIED)
        return EmitError(ctx.opts, SbieStatus::ACCESS_DENIED,
                         L"box '" + name + L"' is protected (NeverDelete=y)");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box delete failed");

    if (delSection) {
        st = model::ConfigStore().Delete(name, L"*", std::nullopt, true, pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box delete failed"
                             + std::wstring(boxproc::PasswordHint(st, pw)));
    }

    EmitMessage(ctx.opts, L"box '" + name + L"' deleted");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box enable|disable <name>（P0-3；04 §4.3：Enabled=y/n）
// ---------------------------------------------------------------------------

int CmdBoxSetEnabled(const CommandContext& ctx, bool on)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         on ? L"usage: sbie-cli box enable <name>"
                            : L"usage: sbie-cli box disable <name>");
    const std::wstring& name = pos[0];
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    const wchar_t* what = on ? L"enabled" : L"disabled";

    // 写路径（非幂等，retry=false；box.setEnabled）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"enabled", on);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxSetEnabled, params, false,
            [what](const GlobalOptions& o, const json::JsonValue&) {
                boxproc::EmitSilentOk(o, what);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连：Model BoxRepository::SetEnabled 的 pw-aware 等价（Enabled=y/n，
    // Boxes.cpp:236-246 同款 IniSetSetting，密码透传）
    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' not found");
    st = model::ConfigStore().Set(name, L"Enabled", on ? L"y" : L"n", true,
                                  pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         std::wstring(L"box ") + what + L" failed"
                         + boxproc::PasswordHint(st, pw));

    // 写后回读校验（验收回传加固，04 §13；与 server HBoxSetEnabled 同款）
    {
        auto v = model::ConfigStore().Get(name, L"Enabled", 0, true, true);
        const bool nowOn = v.has_value() && (*v == L"y" || *v == L"Y");
        if (nowOn != on)
            return EmitError(ctx.opts, SbieStatus::GENERIC,
                             std::wstring(L"box ") + what
                                 + L" verification failed: Enabled is"
                                   L" still '"
                                 + (v.has_value() ? *v : std::wstring(L""))
                                 + L"' after write");
    }
    boxproc::EmitSilentOk(ctx.opts, what);
    return 0;
}

int CmdBoxEnable(const CommandContext& ctx)  { return CmdBoxSetEnabled(ctx, true); }
int CmdBoxDisable(const CommandContext& ctx) { return CmdBoxSetEnabled(ctx, false); }

// ---------------------------------------------------------------------------
// sbie box clean <name>（P0-4；04 §4.3：清空沙箱内容、保留节）
// 语义对齐 CSandBox::CleanBox（06 §P0-4 建议的"拒+提示"变体，与 delete 一致）：
//   NeverDelete=y → ACCESS_DENIED；有活动进程 → BOX_BUSY（提示 proc kill-all）；
//   否则递归删 FileRoot 内容（保留 FileRoot 目录本身与 ini 节）。
// ---------------------------------------------------------------------------

namespace {

// 递归删除目录（对齐 Model Boxes.cpp 的 DeleteDirRecursive 行为：只读属性
// 清理 + 句柄释放 500ms×20 重试）——server 侧 Dispatcher.cpp 有同构副本
SbieStatus DeleteDirRecursiveLocal(const std::wstring& dir)
{
    if (dir.empty())
        return SbieStatus::GENERIC;
    DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES)
        return SbieStatus::OK;   // 不存在 = 已删
    if (!(at & FILE_ATTRIBUTE_DIRECTORY))
        return SbieStatus::GENERIC;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            st = DeleteDirRecursiveLocal(full);
        else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (st != SbieStatus::OK)
        return st;
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(dir.c_str()))
            return SbieStatus::OK;
        if (i < 19)
            Sleep(500);
    }
    return SbieStatus::GENERIC;
}

// 清空目录内容、保留目录本身（CleanBoxFolders 语义）
SbieStatus CleanDirContents(const std::wstring& dir)
{
    DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES)
        return SbieStatus::OK;   // 未初始化的 box：无目录即已清空
    if (!(at & FILE_ATTRIBUTE_DIRECTORY))
        return SbieStatus::GENERIC;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            st = DeleteDirRecursiveLocal(full);
        else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return st;
}

} // namespace

int CmdBoxClean(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box clean <name>");
    const std::wstring& name = pos[0];

    // 写路径（非幂等，retry=false；box.clean —— 纯文件操作，server 不触
    // SbieSvc；NeverDelete/进程检查两侧同序）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxClean, params, false,
            [](const GlobalOptions& o, const json::JsonValue&) {
                EmitMessage(o, L"cleaned");
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' not found");

    // NeverDelete 保护（对齐 CleanBox 的 SB_DeleteProtect）
    auto nd = model::ConfigStore().Get(name, L"NeverDelete", 0, true, true);
    if (nd.has_value() && *nd == L"y")
        return EmitError(ctx.opts, SbieStatus::ACCESS_DENIED,
                         L"box '" + name + L"' is protected (NeverDelete=y)");
    // 有活动进程先拒（与 box delete 一致：提示 kill-all）
    if (bi.hasProcesses)
        return EmitError(ctx.opts, SbieStatus::BOX_BUSY,
                         L"box '" + name + L"' has running processes; "
                         L"terminate them first (proc kill-all / proc kill)");

    if (!bi.fileRoot.empty()) {
        st = CleanDirContents(bi.fileRoot);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box clean failed (file error; retry after"
                             L" handles release)");
    }
    EmitMessage(ctx.opts, L"cleaned");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box size <name>（P0-5；04 §4.3：ScanBoxSize 递归统计 FileRoot）
// 输出：SIZE（人性化）/BYTES/FILES/DIRS；--json {name,bytes,human,files,dirs}
// ---------------------------------------------------------------------------

namespace {

// 渲染 box.size 的 data（两路径共用；直连分支自行组装同构 data）
int RenderBoxSize(const GlobalOptions& o, const json::JsonValue& data)
{
    if (o.json) {
        EmitJsonOk(o, data);
        return 0;
    }
    util::TablePrinter t;
    t.AddColumn(L"SIZE");
    t.AddColumn(L"BYTES", true);
    t.AddColumn(L"FILES", true);
    t.AddColumn(L"DIRS", true);
    std::vector<std::wstring> row = {
        ipcroute::CellOf(data.find(L"human")),
        ipcroute::CellOf(data.find(L"bytes")),
        ipcroute::CellOf(data.find(L"files")),
        ipcroute::CellOf(data.find(L"dirs")),
    };
    t.AddRow(std::move(row));
    EmitRows(o, t, json::JsonValue::Array(), L"(empty box)");
    return 0;
}

} // namespace

int CmdBoxSize(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box size <name>");
    const std::wstring& name = pos[0];

    // IPC 优先（box.size，读路径 retry=true；server 侧纯文件扫描，无 SbieSvc）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::Result r = ipcroute::Invoke(ctx.opts, ipc::kOpBoxSize,
                                              params, true, RenderBoxSize);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::BoxInfo bi;
    SbieStatus st = MakeRepo().GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + name + L"' not found");

    model::BoxUsageStats usage;
    st = model::ScanBoxSize(bi.fileRoot, &usage, nullptr);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box size scan failed (file error)");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"name", json::JsonValue(name));
    data.set(L"bytes", json::JsonValue((long long)usage.totalBytes));
    data.set(L"human", json::JsonValue(boxproc::FormatHumanBytes(
                           usage.totalBytes)));
    data.set(L"files", json::JsonValue((long long)usage.files));
    data.set(L"dirs", json::JsonValue((long long)usage.dirs));
    return RenderBoxSize(ctx.opts, data);
}

void RegisterBoxManageCommands()
{
    auto& box = Commands()["box"];
    box["info"] = CmdBoxInfo;
    box["get"] = CmdBoxGet;
    box["set"] = CmdBoxSet;
    box["list-setting"] = CmdBoxListSetting;
    box["rename"] = CmdBoxRename;
    box["delete"] = CmdBoxDelete;
    box["enable"] = CmdBoxEnable;
    box["disable"] = CmdBoxDisable;
    box["clean"] = CmdBoxClean;
    box["size"] = CmdBoxSize;
}

} // namespace sbie::cli
