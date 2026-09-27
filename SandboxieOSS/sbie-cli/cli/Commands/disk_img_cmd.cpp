// Sandboxie-OSS — sbie-cli/cli/Commands/disk_img_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie img list|status [<box>]|create <box>|mount <box>|unmount <box>
// sbie ramdisk status
// （波 D2，docs/04 §18；可行性结论与规格锚点见 Model/DiskImage.h 头注）
//
// 执行者语义（对用户可见的三个要点）：
//   * `box set UseFileImage=y / UseRamDisk=y` 写键即生效——SbieSvc 在箱内
//     首进程启动路径上自动挂载（AcquireBoxRoot），本命令组不重复该职责；
//   * img mount/unmount 是显式挂载面（加密箱需口令：自动挂载路径不传口令，
//     必须先显式 mount——与 SandMan 交互同语义）；
//   * ImDisk 驱动/ImBox.exe 属 SandboxieTools 运行时：缺席时 mount/create
//     报 DRIVER_UNAVAILABLE(3)，status 呈现 "unknown (ImDisk driver not
//     available)"——环境事实而非命令失败。
//
// 退出码：0；3=驱动/ImDisk 不可用；4=SbieSvc 不可用；5=box 未挂载/不存在；
// 6=权限/口令；7=参数（size<256MB、ForceUsbDrives=n 等）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/DiskImage.h"
#include "Model/UsbSandbox.h"

namespace sbie::cli {

namespace {

// img status 的挂载状态单元格（known=false 的两种来源合一呈现）
std::wstring MountCell(const model::ImMountState& m)
{
    if (!m.known)
        return L"unknown (ImDisk driver not available)";
    return m.mounted ? L"yes" : L"no";
}

json::JsonValue MountJson(const model::ImMountState& m)
{
    json::JsonValue o = json::JsonValue::Object();
    o.set(L"known", json::JsonValue(m.known));
    o.set(L"mounted", json::JsonValue(m.mounted));
    if (m.known && m.mounted) {
        o.set(L"disk_root", json::JsonValue(m.diskRoot));
        o.set(L"disk_size", json::JsonValue((long long)m.diskSize));
        o.set(L"used_size", json::JsonValue((long long)m.usedSize));
    }
    return o;
}

void AddImageRow(util::TablePrinter* t, json::JsonValue* rows,
                  const model::BoxImageInfo& i)
{
    if (t)
        t->AddRow({ i.box,
                    i.useFileImage ? L"yes" : L"no",
                    i.useRamDisk ? L"yes" : L"no",
                    (i.confidential ? L"confidential"
                                    : (i.lessConfidential ? L"less" : L"-")),
                    i.enableEfs ? L"yes" : L"no",
                    i.imageFile.empty() ? L"-" : i.imageFile,
                    i.imageExists ? boxproc::FormatHumanBytes(i.imageBytes)
                                  : std::wstring(L"missing"),
                    MountCell(i.mount) });
    if (rows) {
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"box", json::JsonValue(i.box));
        r.set(L"use_file_image", json::JsonValue(i.useFileImage));
        r.set(L"use_ram_disk", json::JsonValue(i.useRamDisk));
        r.set(L"confidential",
              json::JsonValue(i.confidential ? L"confidential"
                              : i.lessConfidential ? L"less" : L"none"));
        r.set(L"enable_efs", json::JsonValue(i.enableEfs));
        r.set(L"image_file", json::JsonValue(i.imageFile));
        r.set(L"image_exists", json::JsonValue(i.imageExists));
        r.set(L"image_bytes", json::JsonValue((long long)i.imageBytes));
        r.set(L"mount", MountJson(i.mount));
        rows->pushBack(std::move(r));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// sbie img list（已挂载的映像/RAM 盘根；MSGID_IMBOX_ENUM）
// ---------------------------------------------------------------------------

int CmdImgList(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpImgList, params, true,
        [](const GlobalOptions& o, const json::JsonValue& data) {
            return ipcroute::RenderRows(
                o, { { L"REG_ROOT", L"reg_root" } }, data, L"no mounted roots");
        });
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    std::vector<std::wstring> roots;
    SbieStatus st = model::EnumMountedRoots(&roots);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"img list failed"
                         + std::wstring(st == SbieStatus::DRIVER_UNAVAILABLE
                             ? L" (ImDisk driver not available)"
                             : L""));
    util::TablePrinter t;
    t.AddColumn(L"REG_ROOT");
    json::JsonValue rows = json::JsonValue::Array();
    for (const std::wstring& root : roots) {
        t.AddRow({ root });
        rows.pushBack(json::JsonValue(root));
    }
    EmitRows(ctx.opts, t, rows, L"no mounted roots");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie img status [<box>]（聚合：键面 + 镜像文件 + 挂载状态）
// ---------------------------------------------------------------------------

int CmdImgStatus(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);

    json::JsonValue params = json::JsonValue::Object();
    if (!pos.empty())
        ipcroute::PSet(&params, L"box", pos[0]);
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpImgStatus, params, true,
        [&pos](const GlobalOptions& o, const json::JsonValue& data) {
            if (!pos.empty()) {
                // 单箱：键值行呈现
                return ipcroute::RenderKv(
                    o,
                    { { L"box", L"box" },
                      { L"use_file_image", L"use_file_image" },
                      { L"use_ram_disk", L"use_ram_disk" },
                      { L"confidential", L"confidential" },
                      { L"enable_efs", L"enable_efs" },
                      { L"force_protection_on_mount",
                        L"force_protection_on_mount" },
                      { L"image_file", L"image_file" },
                      { L"image_exists", L"image_exists" },
                      { L"image_bytes", L"image_bytes" },
                      { L"mounted", L"mounted" } },
                    data);
            }
            return ipcroute::RenderRows(
                o,
                { { L"BOX", L"box" },
                  { L"IMG", L"use_file_image" },
                  { L"RAMDISK", L"use_ram_disk" },
                  { L"CONFIDENTIAL", L"confidential" },
                  { L"EFS", L"enable_efs" },
                  { L"IMAGE_FILE", L"image_file" },
                  { L"IMAGE_SIZE", L"image_bytes" },
                  { L"MOUNTED", L"mounted" } },
                data, L"no boxes",
                [](const json::JsonValue& row) {
                    std::vector<std::wstring> line;
                    auto s = [&row](const wchar_t* k) {
                        return ipcroute::CellOf(
                            row.isObject() ? row.find(k) : nullptr);
                    };
                    // IMAGE_SIZE 列人性化（--json 仍给原始 bytes，见 server data）
                    const json::JsonValue* bytes = row.isObject()
                        ? row.find(L"image_bytes") : nullptr;
                    std::wstring size = L"-";
                    if (bytes && bytes->isInt()) {
                        const bool exists = [&row] {
                            const json::JsonValue* e = row.find(L"image_exists");
                            return e && e->type() == json::JsonValue::Type::Bool
                                       ? e->asBool() : false;
                        }();
                        size = exists
                            ? boxproc::FormatHumanBytes(
                                  (unsigned long long)bytes->asInt())
                            : L"missing";
                    }
                    line = { s(L"box"), s(L"use_file_image"),
                             s(L"use_ram_disk"), s(L"confidential"),
                             s(L"enable_efs"), s(L"image_file"), size,
                             s(L"mounted") };
                    return line;
                });
        });
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();

    if (!pos.empty()) {
        model::BoxImageInfo info;
        SbieStatus st = model::QueryBoxImage(pos[0], &info);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box '" + pos[0] + L"' not found");
        if (ctx.opts.json) {
            json::JsonValue d = json::JsonValue::Object();
            d.set(L"box", json::JsonValue(info.box));
            d.set(L"use_file_image", json::JsonValue(info.useFileImage));
            d.set(L"use_ram_disk", json::JsonValue(info.useRamDisk));
            d.set(L"confidential",
                  json::JsonValue(info.confidential ? L"confidential"
                                  : info.lessConfidential ? L"less"
                                                          : L"none"));
            d.set(L"enable_efs", json::JsonValue(info.enableEfs));
            d.set(L"force_protection_on_mount",
                  json::JsonValue(info.forceProtectionOnMount));
            d.set(L"image_file", json::JsonValue(info.imageFile));
            d.set(L"image_exists", json::JsonValue(info.imageExists));
            d.set(L"image_bytes", json::JsonValue((long long)info.imageBytes));
            d.set(L"mounted",
                  info.mount.known
                      ? json::JsonValue(info.mount.mounted)
                      : json::JsonValue());   // null = 未知（ImDisk 缺席等）
            d.set(L"mount", MountJson(info.mount));
            EmitJsonOk(ctx.opts, d);
            return 0;
        }
        std::vector<std::pair<std::wstring, std::wstring>> kv = {
            { L"box", info.box },
            { L"use_file_image", info.useFileImage ? L"y" : L"n" },
            { L"use_ram_disk", info.useRamDisk ? L"y" : L"n" },
            { L"confidential", info.confidential ? L"confidential"
                              : info.lessConfidential ? L"less" : L"none" },
            { L"enable_efs", info.enableEfs ? L"y" : L"n" },
            { L"force_protection_on_mount",
              info.forceProtectionOnMount ? L"y" : L"n" },
            { L"image_file", info.imageFile.empty() ? L"-" : info.imageFile },
            { L"image_exists", info.imageExists ? L"yes" : L"no" },
            { L"image_bytes", info.imageExists
                                  ? boxproc::FormatHumanBytes(info.imageBytes)
                                  : L"-" },
            { L"mounted", MountCell(info.mount) },
        };
        if (info.mount.known && info.mount.mounted) {
            kv.push_back({ L"disk_root", info.mount.diskRoot });
            kv.push_back({ L"disk_size",
                           boxproc::FormatHumanBytes(info.mount.diskSize) });
            kv.push_back({ L"used_size",
                           boxproc::FormatHumanBytes(info.mount.usedSize) });
        }
        json::JsonValue obj = json::JsonValue::Object();
        EmitKv(ctx.opts, kv, obj);
        return 0;
    }

    // 全箱表
    util::TablePrinter t;
    t.AddColumn(L"BOX");
    t.AddColumn(L"IMG");
    t.AddColumn(L"RAMDISK");
    t.AddColumn(L"CONFIDENTIAL");
    t.AddColumn(L"EFS");
    t.AddColumn(L"IMAGE_FILE");
    t.AddColumn(L"IMAGE_SIZE");
    t.AddColumn(L"MOUNTED");
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::BoxImageInfo& i : model::EnumBoxImages())
        AddImageRow(&t, &rows, i);
    EmitRows(ctx.opts, t, rows, L"no boxes");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie img create <box> --size-mb <N> [--password <pw>]
// ---------------------------------------------------------------------------

int CmdImgCreate(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() != 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli img create <box> --size-mb <N>"
                         L" [--password <pw>]");
    const std::wstring box = pos[0];

    unsigned long sizeMb = 0;
    const std::wstring sizeStr = boxproc::OptionValue(ctx.args, L"--size-mb");
    if (sizeStr.empty())
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"--size-mb <N> is required (minimum 256 MB)");
    if (!boxproc::ParseUlong(sizeStr, &sizeMb) || sizeMb < 256)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid --size-mb '" + sizeStr
                         + L"' (minimum 256 MB)");

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    const unsigned long long sizeKb = (unsigned long long)sizeMb * 1024;

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::PSet(&params, L"box", box);
    ipcroute::PSet(&params, L"size_kb", (long long)sizeKb);
    if (!pw.empty())
        ipcroute::PSet(&params, L"password", pw);
    const std::wstring okMsg = L"image created for box '" + box
        + L"' (" + std::to_wstring(sizeMb) + L" MB"
        + (pw.empty() ? L"" : L", AES") + L")";
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpImgCreate, params, false,
        [okMsg](const GlobalOptions& o, const json::JsonValue& data) {
            if (o.json) {
                json::JsonValue d = data.isObject() ? data
                                                    : json::JsonValue::Object();
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

    srvconn::NoteDegraded();
    SbieStatus st = model::CreateBoxImage(box, sizeKb, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"img create failed for box '" + box + L"'"
                         + std::wstring(st == SbieStatus::DRIVER_UNAVAILABLE
                             ? L" (ImDisk driver not available)"
                             : L" (SbieSvc MountManager)"));
    EmitMessage(ctx.opts, okMsg);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie img mount <box> [--password <pw>] [--protect|--no-protect]
//                        [--admin-only|--no-admin-only] [--auto-unmount]
// ---------------------------------------------------------------------------

int CmdImgMount(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() != 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli img mount <box> [--password <pw>]"
                         L" [--protect|--no-protect] [--auto-unmount]");
    const std::wstring box = pos[0];

    std::optional<bool> protect, adminOnly;
    if (boxproc::HasFlag(ctx.args, L"--protect"))
        protect = true;
    else if (boxproc::HasFlag(ctx.args, L"--no-protect"))
        protect = false;
    if (boxproc::HasFlag(ctx.args, L"--admin-only"))
        adminOnly = true;
    else if (boxproc::HasFlag(ctx.args, L"--no-admin-only"))
        adminOnly = false;
    const bool autoUnmount = boxproc::HasFlag(ctx.args, L"--auto-unmount");
    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::PSet(&params, L"box", box);
    if (!pw.empty())
        ipcroute::PSet(&params, L"password", pw);
    if (protect.has_value())
        ipcroute::PSet(&params, L"protect", *protect);
    if (adminOnly.has_value())
        ipcroute::PSet(&params, L"admin_only", *adminOnly);
    ipcroute::PSet(&params, L"auto_unmount", autoUnmount);
    const std::wstring okMsg = L"box '" + box + L"' image mounted"
        + (autoUnmount ? L" (auto unmount on box close)" : L"");
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpImgMount, params, false,
        [okMsg](const GlobalOptions& o, const json::JsonValue& data) {
            if (o.json) {
                json::JsonValue d = data.isObject() ? data
                                                    : json::JsonValue::Object();
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

    srvconn::NoteDegraded();
    SbieStatus st = model::MountBoxImage(box, pw, protect, adminOnly,
                                         autoUnmount);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"img mount failed for box '" + box + L"'"
                         + std::wstring(st == SbieStatus::DRIVER_UNAVAILABLE
                             ? L" (ImDisk driver not available)"
                             : st == SbieStatus::NOT_FOUND
                                   ? L" (image file not mounted yet?)"
                                   : L" (SbieSvc MountManager)"));
    EmitMessage(ctx.opts, okMsg);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie img unmount <box>
// ---------------------------------------------------------------------------

int CmdImgUnmount(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() != 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli img unmount <box>");
    const std::wstring box = pos[0];

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::PSet(&params, L"box", box);
    const std::wstring okMsg = L"box '" + box + L"' image unmounted";
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpImgUnmount, params, false,
        [okMsg](const GlobalOptions& o, const json::JsonValue& data) {
            if (o.json) {
                json::JsonValue d = data.isObject() ? data
                                                    : json::JsonValue::Object();
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

    srvconn::NoteDegraded();
    SbieStatus st = model::UnmountBoxImage(box);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"img unmount failed for box '" + box + L"'"
                         + std::wstring(st == SbieStatus::NOT_FOUND
                             ? L" (root not mounted)"
                             : L""));
    EmitMessage(ctx.opts, okMsg);
    return 0;
}

// ---------------------------------------------------------------------------
// sbie ramdisk status（全局键 + 使用箱 + 共享盘挂载状态）
// ---------------------------------------------------------------------------

int CmdRamDiskStatus(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpRamDiskStatus, json::JsonValue::Object(), true,
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
            auto num = [&data](const wchar_t* k) {
                const json::JsonValue* v = data.isObject() ? data.find(k)
                                                           : nullptr;
                return (v && v->isInt()) ? v->asInt() : 0ll;
            };
            // boxes（数组）→ 逗号连接
            std::wstring boxes;
            if (const json::JsonValue* b = data.isObject()
                    ? data.find(L"boxes") : nullptr;
                b && b->isArray()) {
                for (const json::JsonValue& e : b->items()) {
                    if (!boxes.empty())
                        boxes += L",";
                    boxes += e.isString() ? e.asString() : L"?";
                }
            }
            const json::JsonValue* mounted = data.isObject()
                ? data.find(L"mounted") : nullptr;
            const bool mountedKnown = mounted
                && mounted->type() == json::JsonValue::Type::Bool;
            std::wstring mountCell = mountedKnown
                ? (mounted->asBool() ? L"yes" : L"no")
                : L"unknown (ImDisk driver not available)";
            std::vector<std::pair<std::wstring, std::wstring>> kv = {
                { L"size_kb", num(L"size_kb")
                      ? std::to_wstring(num(L"size_kb"))
                      : str(L"size_human") },
                { L"size_human", str(L"size_human") },
                { L"size_below_minimum",
                  flag(L"size_below_minimum")
                      ? L"yes (SbieSvc requires >= 102400 Kb)"
                      : L"no" },
                { L"letter", str(L"letter") },
                { L"boxes", boxes.empty() ? L"(none)" : boxes },
                { L"mounted", mountCell },
            };
            const json::JsonValue* m = data.isObject()
                ? data.find(L"mount") : nullptr;
            if (mountedKnown && mounted->asBool() && m && m->isObject()) {
                if (const json::JsonValue* dr = m->find(L"disk_root");
                    dr && dr->isString())
                    kv.push_back({ L"disk_root", dr->asString() });
                if (const json::JsonValue* ds = m->find(L"disk_size");
                    ds && ds->isInt())
                    kv.push_back({ L"disk_size",
                                   boxproc::FormatHumanBytes(
                                       (unsigned long long)ds->asInt()) });
                if (const json::JsonValue* us = m->find(L"used_size");
                    us && us->isInt())
                    kv.push_back({ L"used_size",
                                   boxproc::FormatHumanBytes(
                                       (unsigned long long)us->asInt()) });
            }
            json::JsonValue obj = json::JsonValue::Object();
            EmitKv(o, kv, obj);
            return 0;
        });
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    model::RamDiskInfo info;
    SbieStatus st = model::QueryRamDisk(&info);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"ramdisk status failed");

    std::wstring boxes;
    for (const std::wstring& b : info.boxes) {
        if (!boxes.empty())
            boxes += L",";
        boxes += b;
    }
    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"size_kb", info.sizeKb ? std::to_wstring(info.sizeKb)
                                  : std::wstring(L"(not configured)") },
        { L"size_human", info.sizeKb
                              ? boxproc::FormatHumanBytes(info.sizeKb * 1024)
                              : L"-" },
        { L"size_below_minimum",
          info.sizeBelowMinimum ? L"yes (SbieSvc requires >= 102400 Kb)"
                                : L"no" },
        { L"letter", info.letter.empty() ? L"(auto)" : info.letter },
        { L"boxes", boxes.empty() ? L"(none)" : boxes },
        { L"mounted", MountCell(info.mount) },
    };
    if (info.mount.known && info.mount.mounted) {
        kv.push_back({ L"disk_root", info.mount.diskRoot });
        kv.push_back({ L"disk_size",
                       boxproc::FormatHumanBytes(info.mount.diskSize) });
        kv.push_back({ L"used_size",
                       boxproc::FormatHumanBytes(info.mount.usedSize) });
    }
    json::JsonValue obj = json::JsonValue::Object();   // 非 --json 不用
    EmitKv(ctx.opts, kv, obj);
    return 0;
}

void RegisterDiskImageCommands()
{
    auto& img = Commands()["img"];
    img["list"] = CmdImgList;
    img["status"] = CmdImgStatus;
    img["create"] = CmdImgCreate;
    img["mount"] = CmdImgMount;
    img["unmount"] = CmdImgUnmount;
    Commands()["ramdisk"]["status"] = CmdRamDiskStatus;
}

} // namespace sbie::cli
