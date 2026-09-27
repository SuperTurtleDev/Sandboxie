// Sandboxie-OSS — sbie-cli/cli/Commands/usb_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie usb status | usb sync [--dry-run]（波 D2 / 07-P2-1，docs/04 §18；
// 规格锚点 docs/07 §3.3——SandMan UpdateForceUSB 守护行为的 CLI 等价：
// 一次性"枚举 USB 卷 → 建 UsbSandbox 箱（若缺）→ 整表写 ForceFolder"；
// 长期守护由用户把本命令挂计划任务，或等 Guardian 域后续扩展）。
//
// 全局键（docs/07 §1.2 Program Control 页）：
//   ForceUsbDrives=y 启用接管；UsbSandbox=目标箱名（缺省 USB_Box）；
//   DisabledForceVolume=HHHH-LLLL 排除清单。
// 退出码：0；3=驱动不可用；4=SbieSvc 不可用（sync 写路径）；7=ForceUsbDrives=n。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/UsbSandbox.h"

#include <cwchar>

namespace sbie::cli {

namespace {

// 卷表行：SERIAL / LABEL / MOUNTS / USB / TAKEN
void AddVolumeRow(util::TablePrinter* t, json::JsonValue* rows,
                  const model::UsbSandboxInfo& info, const model::UsbVolume& v)
{
    std::wstring mounts;
    for (const std::wstring& mp : v.mountPoints) {
        if (!mounts.empty())
            mounts += L";";
        mounts += mp;
    }
    const bool taken = info.VolumeTaken(v);
    if (t)
        t->AddRow({ v.serial.empty() ? L"-" : v.serial,
                    v.label.empty() ? L"-" : v.label,
                    mounts.empty() ? L"-" : mounts,
                    !v.busKnown ? L"unknown"
                                : (v.onUsbBus ? L"yes" : L"no"),
                    taken ? L"yes" : L"no" });
    if (rows) {
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"serial", json::JsonValue(v.serial));
        r.set(L"label", json::JsonValue(v.label));
        json::JsonValue mps = json::JsonValue::Array();
        for (const std::wstring& mp : v.mountPoints)
            mps.pushBack(json::JsonValue(mp));
        r.set(L"mount_points", std::move(mps));
        r.set(L"bus_known", json::JsonValue(v.busKnown));
        r.set(L"on_usb_bus", json::JsonValue(v.onUsbBus));
        r.set(L"taken", json::JsonValue(taken));
        rows->pushBack(std::move(r));
    }
}

std::wstring JoinList(const std::vector<std::wstring>& list)
{
    std::wstring out;
    for (const std::wstring& v : list) {
        if (!out.empty())
            out += L",";
        out += v;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie usb status
// ---------------------------------------------------------------------------

int CmdUsbStatus(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpUsbStatus, json::JsonValue::Object(), true,
        [](const GlobalOptions& o, const json::JsonValue& data) {
            if (o.json) {
                EmitJsonOk(o, data);
                return 0;
            }
            auto str = [&data](const wchar_t* k) {
                const json::JsonValue* v = data.isObject() ? data.find(k)
                                                           : nullptr;
                return (v && v->isString()) ? v->asString()
                                            : std::wstring(L"-");
            };
            auto flag = [&data](const wchar_t* k) {
                const json::JsonValue* v = data.isObject() ? data.find(k)
                                                           : nullptr;
                return v && v->type() == json::JsonValue::Type::Bool
                           ? v->asBool() : false;
            };
            std::wstring out;
            out += std::wstring(L"force_usb_drives: ")
                 + (flag(L"force_usb_drives") ? L"y" : L"n") + L"\n";
            out += L"usb_sandbox: " + str(L"usb_sandbox")
                 + (flag(L"sandbox_exists") ? L" (exists)"
                                            : L" (missing)") + L"\n";
            auto joinArr = [](const json::JsonValue* a) {
                std::wstring s;
                if (a && a->isArray()) {
                    for (const json::JsonValue& e : a->items()) {
                        if (!s.empty())
                            s += L",";
                        s += e.isString() ? e.asString() : L"?";
                    }
                }
                return s.empty() ? std::wstring(L"(none)") : s;
            };
            out += L"disabled_volumes: "
                 + joinArr(data.isObject()
                               ? data.find(L"disabled_volumes") : nullptr)
                 + L"\n";
            out += L"force_folders: "
                 + joinArr(data.isObject()
                               ? data.find(L"force_folders") : nullptr)
                 + L"\n";
            util::PrintUtf8(util::WideToUtf8(out));
            // 卷表（MOUNTS = mount_points 数组连接）
            const json::JsonValue* vols = data.isObject()
                ? data.find(L"volumes") : nullptr;
            if (vols && vols->isArray())
                return ipcroute::RenderRows(
                    o,
                    { { L"SERIAL", L"serial" },
                      { L"LABEL", L"label" },
                      { L"MOUNTS", L"mount_points" },
                      { L"USB", L"on_usb_bus" },
                      { L"TAKEN", L"taken" } },
                    *vols, L"no volumes",
                    [](const json::JsonValue& row) {
                        std::vector<std::wstring> line;
                        auto s = [&row](const wchar_t* k) {
                            return ipcroute::CellOf(
                                row.isObject() ? row.find(k) : nullptr);
                        };
                        std::wstring mounts;
                        if (const json::JsonValue* mps = row.isObject()
                                ? row.find(L"mount_points") : nullptr;
                            mps && mps->isArray()) {
                            for (const json::JsonValue& mp : mps->items()) {
                                if (!mounts.empty())
                                    mounts += L";";
                                mounts += mp.isString() ? mp.asString() : L"?";
                            }
                        }
                        // USB 列：bus_known=false 时如实呈现 unknown
                        std::wstring usb = s(L"on_usb_bus");
                        if (const json::JsonValue* bk = row.isObject()
                                ? row.find(L"bus_known") : nullptr;
                            bk && bk->type() == json::JsonValue::Type::Bool
                            && !bk->asBool())
                            usb = L"unknown";
                        line = { s(L"serial"), s(L"label"),
                                 mounts.empty() ? L"-" : mounts,
                                 usb, s(L"taken") };
                        return line;
                    });
            return 0;
        });
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    model::UsbSandboxInfo info;
    SbieStatus st = model::QueryUsbSandbox(&info);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"usb status failed");

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"force_usb_drives", json::JsonValue(info.forceUsbDrives));
        d.set(L"usb_sandbox", json::JsonValue(info.sandboxName));
        d.set(L"sandbox_exists", json::JsonValue(info.sandboxExists));
        json::JsonValue dv = json::JsonValue::Array();
        for (const std::wstring& v : info.disabledVolumes)
            dv.pushBack(json::JsonValue(v));
        d.set(L"disabled_volumes", std::move(dv));
        json::JsonValue ff = json::JsonValue::Array();
        for (const std::wstring& v : info.forceFolders)
            ff.pushBack(json::JsonValue(v));
        d.set(L"force_folders", std::move(ff));
        json::JsonValue vols = json::JsonValue::Array();
        for (const model::UsbVolume& v : info.volumes) {
            json::JsonValue r2 = json::JsonValue::Object();
            r2.set(L"serial", json::JsonValue(v.serial));
            r2.set(L"label", json::JsonValue(v.label));
            json::JsonValue mps = json::JsonValue::Array();
            for (const std::wstring& mp : v.mountPoints)
                mps.pushBack(json::JsonValue(mp));
            r2.set(L"mount_points", std::move(mps));
            r2.set(L"bus_known", json::JsonValue(v.busKnown));
            r2.set(L"on_usb_bus", json::JsonValue(v.onUsbBus));
            r2.set(L"taken", json::JsonValue(info.VolumeTaken(v)));
            vols.pushBack(std::move(r2));
        }
        d.set(L"volumes", std::move(vols));
        EmitJsonOk(ctx.opts, d);
        return 0;
    }

    std::wstring head;
    head += L"force_usb_drives: " + std::wstring(info.forceUsbDrives ? L"y" : L"n")
          + L"\n";
    head += L"usb_sandbox: " + info.sandboxName
          + (info.sandboxExists ? L" (exists)" : L" (missing)") + L"\n";
    head += L"disabled_volumes: "
          + (info.disabledVolumes.empty() ? L"(none)"
                                          : JoinList(info.disabledVolumes))
          + L"\n";
    head += L"force_folders: "
          + (info.forceFolders.empty() ? L"(none)"
                                       : JoinList(info.forceFolders)) + L"\n";
    util::PrintUtf8(util::WideToUtf8(head));

    util::TablePrinter t;
    t.AddColumn(L"SERIAL");
    t.AddColumn(L"LABEL");
    t.AddColumn(L"MOUNTS");
    t.AddColumn(L"USB");
    t.AddColumn(L"TAKEN");
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::UsbVolume& v : info.volumes)
        AddVolumeRow(&t, &rows, info, v);
    EmitRows(ctx.opts, t, rows, L"no volumes");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie usb sync [--dry-run]
// ---------------------------------------------------------------------------

int CmdUsbSync(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    const bool dryRun = boxproc::HasFlag(ctx.args, L"--dry-run");
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);

    model::UsbSandboxInfo probe;
    if (model::QueryUsbSandbox(&probe) == SbieStatus::OK
        && !probe.forceUsbDrives)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"usb drive sandboxing is disabled"
                         L" (enable with: sbie-cli cfg set ForceUsbDrives y)");

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::PSet(&params, L"dry_run", dryRun);
    if (!pw.empty())
        ipcroute::PSet(&params, L"password", pw);
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpUsbSync, params, false,
        [](const GlobalOptions& o, const json::JsonValue& data) {
            if (o.json) {
                EmitJsonOk(o, data);
                return 0;
            }
            auto str = [&data](const wchar_t* k) {
                const json::JsonValue* v = data.isObject() ? data.find(k)
                                                           : nullptr;
                return (v && v->isString()) ? v->asString()
                                            : std::wstring(L"-");
            };
            EmitMessage(o, str(L"message"));
            // force_folders = 字符串数组（非行对象）：手绘单列表
            const json::JsonValue* ff = data.isObject()
                ? data.find(L"force_folders") : nullptr;
            if (ff && ff->isArray()) {
                util::TablePrinter t;
                t.AddColumn(L"FORCE_FOLDER");
                json::JsonValue rows = json::JsonValue::Array();
                for (const json::JsonValue& e : ff->items()) {
                    const std::wstring v =
                        e.isString() ? e.asString() : std::wstring(L"?");
                    t.AddRow({ v });
                    json::JsonValue one = json::JsonValue::Object();
                    one.set(L"force_folder", json::JsonValue(v));
                    rows.pushBack(std::move(one));
                }
                EmitRows(o, t, rows, L"no usb volumes taken over");
            }
            return 0;
        });
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    model::UsbSyncResult res;
    SbieStatus st = model::SyncUsbSandbox(pw, dryRun, &res);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"usb sync failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    const std::wstring msg = dryRun
        ? L"dry run: " + std::to_wstring(res.added)
              + L" folder(s) would be added, " + std::to_wstring(res.removed)
              + L" removed for box '" + probe.sandboxName + L"'"
        : L"usb sandbox '" + probe.sandboxName + L"' synced ("
              + std::to_wstring(res.added) + L" added, "
              + std::to_wstring(res.removed) + L" removed"
              + (res.boxCreated ? L", box created" : L"") + L")";
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"message", json::JsonValue(msg));
        d.set(L"box", json::JsonValue(probe.sandboxName));
        d.set(L"box_created", json::JsonValue(res.boxCreated));
        d.set(L"dry_run", json::JsonValue(dryRun));
        d.set(L"added", json::JsonValue((long long)res.added));
        d.set(L"removed", json::JsonValue((long long)res.removed));
        json::JsonValue ff = json::JsonValue::Array();
        for (const std::wstring& v : res.written) {
            json::JsonValue one = json::JsonValue::Object();
            one.set(L"force_folder", json::JsonValue(v));
            ff.pushBack(std::move(one));
        }
        d.set(L"force_folders", std::move(ff));
        EmitJsonOk(ctx.opts, d);
        return 0;
    }
    EmitMessage(ctx.opts, msg);
    util::TablePrinter t;
    t.AddColumn(L"FORCE_FOLDER");
    json::JsonValue rows = json::JsonValue::Array();
    for (const std::wstring& v : res.written) {
        t.AddRow({ v });
        json::JsonValue one = json::JsonValue::Object();
        one.set(L"force_folder", json::JsonValue(v));
        rows.pushBack(std::move(one));
    }
    EmitRows(ctx.opts, t, rows, L"no usb volumes taken over");
    return 0;
}

void RegisterUsbCommands()
{
    auto& usb = Commands()["usb"];
    usb["status"] = CmdUsbStatus;
    usb["sync"] = CmdUsbSync;
}

} // namespace sbie::cli
