// Sandboxie-OSS — sbie-cli/cli/Commands/box_d3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 波次 D3 的 box 组增强（docs/04 §19；注册统一在 Commands_D3.cpp——后注册
// 覆盖同名，原实现文件不动，D2 冲突面最小化）：
//
//   * box create 高级旗标（07-P1-1 剩余，07 §5.2 波次 8）：
//       --location <dir>   FileRootPath=<dir>\<name>
//       --temp             AutoDelete=y + AutoRemove=y（一次性箱）
//       --v2-delete        UseFileDeleteV2=y + UseRegDeleteV2=y
//       --auto-recover     AutoRecover=y
//       --block-net        AllowNetworkAccess=n（驱动设备级禁网，
//                          core\drv\file.c:2676 语义）
//       --drop-admin       DropAdminRights=y
//     无高级旗标时原样委托 CmdBoxCreate（IPC 语义不变）；有旗标时纯 client
//     直连 SbieSvc（键面写命令）——server op 装载旗标待 D1/D2 接线
//     （op 规格：box.create 增 location/temp/v2_delete/auto_recover/
//     block_net/drop_admin 六参数）。
//   * box info（07-P2-2 + 06 P2-4）：+type（键面派生七类）/never_delete/
//     auto_delete/empty/initialized 字段。
//   * box list --type <t>（07-P2-2）：按派生类型过滤。
//   * box snapshot default <box> [<id>|--clear]（06 P2-3）：Snapshots.ini
//     [Current] Default=<id>（QSbieAPI SetDefaultSnapshot 同键面）。
//   * box explore <name>（06 P2-13）：资源管理器打开沙箱 FileRoot（SbieCtrl
//     ID_SANDBOX_EXPLORE 同语义——宿主 explorer 打开目录，非沙箱内启动；
//     纯 client ShellExecute，无 server op）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/BoxTransfer.h"
#include "Model/ConfigStore.h"
#include "Model/Snapshots.h"
#include "Model/Templates.h"

#include <cwchar>

#include <shellapi.h>   // ShellExecuteW（box explore）

namespace sbie::cli {

// 原实现（外部链接，box_create.cpp / Commands.cpp / box_manage.cpp /
// box_snapshot.cpp）——无旗标/非 default 动词时原样委托
int CmdBoxCreate(const CommandContext& ctx);
int CmdBoxList(const CommandContext& ctx);
int CmdBoxSnapshot(const CommandContext& ctx);

namespace {

bool DirExistsW(const std::wstring& p)
{
    const DWORD at = GetFileAttributesW(p.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 键面取值（生效视图：含模板回退——类型派生读的就是生效配置）
bool BoxKeyYes(const std::wstring& box, const wchar_t* key)
{
    model::ConfigStore cfg;
    const auto v = cfg.Get(box, key, 0, true /*noExpand*/, false /*noTmpls*/);
    return v.has_value() && !_wcsicmp(v->c_str(), L"y");
}

// 07-P2-2 七类派生（规格：Hardened=UseSecurityMode、+Plus=叠加
// UsePrivacyMode、Compartment=NoSecurityIsolation、Insecure=
// UnsecureDebugging、Private=UseFileImage+Confidential；输出用波 B 预设名）
std::wstring DeriveBoxType(const std::wstring& box)
{
    const bool privacy = BoxKeyYes(box, L"UsePrivacyMode");
    if (BoxKeyYes(box, L"UseFileImage") && BoxKeyYes(box, L"ConfidentialBox"))
        return L"private";
    if (BoxKeyYes(box, L"UseSecurityMode"))
        return privacy ? L"hardened-plus" : L"hardening";
    if (BoxKeyYes(box, L"NoSecurityIsolation"))
        return privacy ? L"app-plus" : L"app";
    if (BoxKeyYes(box, L"UnsecureDebugging"))
        return L"insecure";
    return privacy ? L"standard-plus" : L"standard";
}

bool KnownBoxType(const std::wstring& t)
{
    static const wchar_t* const kTypes[] = {
        L"hardening", L"hardened-plus", L"standard", L"standard-plus",
        L"app", L"app-plus", L"insecure", L"private",
    };
    for (const wchar_t* k : kTypes)
        if (_wcsicmp(t.c_str(), k) == 0)
            return true;
    return false;
}

// Snapshots.ini 的 [Current] Default 键改写（UTF-8 无 BOM 行级——与
// SnapshotManager::SaveIniFile 同编码形态；empty id = 删该键）
bool SetSnapshotsDefaultKey(const std::wstring& iniPath,
                            const std::wstring& id)
{
    // 读原文（不存在 = 无快照文件，[Current] 落新节）
    std::string bytes;
    {
        HANDLE h = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[16384];
            DWORD n = 0;
            while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n)
                bytes.append(buf, n);
            CloseHandle(h);
        }
    }
    std::wstring text = util::Utf8ToWide(bytes);

    // 定位 [Current] 节：替换已有 Default= 行或在该节头后插入
    std::vector<std::wstring> lines;
    {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t eol = text.find(L'\n', pos);
            if (eol == std::wstring::npos) {
                lines.push_back(text.substr(pos));
                break;
            }
            std::wstring ln = text.substr(pos, eol - pos);
            while (!ln.empty() && (ln.back() == L'\r'))
                ln.pop_back();
            lines.push_back(ln);
            pos = eol + 1;
        }
    }
    const std::wstring defLine = L"Default=" + id;
    bool inCurrent = false, replaced = false, hasCurrent = false;
    std::wstring out;
    for (const std::wstring& ln : lines) {
        std::wstring trimmed = ln;
        const size_t b = trimmed.find_first_not_of(L" \t");
        if (b != std::wstring::npos && trimmed[b] == L'[') {
            const bool isCurrent = b + 9 <= trimmed.size()
                                   && _wcsnicmp(trimmed.c_str() + b,
                                                L"[Current]", 9) == 0
                                   && trimmed.find_first_not_of(L" \t", b + 9)
                                          == std::wstring::npos;
            inCurrent = isCurrent;
            out += ln + L"\n";
            if (isCurrent) {
                hasCurrent = true;
                if (!id.empty() && !replaced) {
                    // 节头后立即插入（尚无 Default 行的文件）
                    out += defLine + L"\n";
                    replaced = true;
                }
            }
            continue;
        }
        if (inCurrent && b != std::wstring::npos
            && _wcsnicmp(trimmed.c_str() + b, L"Default=",
                         wcslen(L"Default=")) == 0) {
            replaced = true;
            if (!id.empty())
                out += defLine + L"\n";
            continue;   // id 空 = 删行
        }
        out += ln + L"\n";
    }
    if (!id.empty() && !hasCurrent) {
        if (!out.empty())
            out += L"\n";
        out += L"[Current]\n";
        out += defLine + L"\n";
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r'))
        out.pop_back();
    out += L"\n";

    const std::string utf8 = util::WideToUtf8(out);
    HANDLE h = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written,
                              nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

} // namespace

// ---------------------------------------------------------------------------
// box create 高级旗标（07-P1-1 剩余）
// ---------------------------------------------------------------------------

int CmdBoxCreateD3(const CommandContext& ctx)
{
    const std::wstring location = boxproc::OptionValue(ctx.args,
                                                       L"--location");
    const bool temp = boxproc::HasFlag(ctx.args, L"--temp");
    const bool v2del = boxproc::HasFlag(ctx.args, L"--v2-delete");
    const bool autoRec = boxproc::HasFlag(ctx.args, L"--auto-recover");
    const bool blockNet = boxproc::HasFlag(ctx.args, L"--block-net");
    const bool dropAdmin = boxproc::HasFlag(ctx.args, L"--drop-admin");

    if (location.empty() && !temp && !v2del && !autoRec && !blockNet
        && !dropAdmin)
        return CmdBoxCreate(ctx);   // 原路径（IPC/直连语义不变）

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (advanced create flags; server op not"
             L" wired - see docs/04 section 19)");

    // 首位置参数 = 箱名（跳过值型旗标的跟随值——--location 不在
    // boxproc::Positional 的值旗标表内，本地解析以免把路径误当箱名）
    std::wstring name;
    {
        static const wchar_t* const kValFlags[] = {
            L"--location", L"--type", L"--template", L"--password",
            L"--dir", L"--sbie-dll-path",
        };
        for (size_t i = 2; i < ctx.args.size(); ++i) {
            const std::wstring& a = ctx.args[i];
            bool valFlag = false;
            for (const wchar_t* f : kValFlags)
                if (a == f) { valFlag = true; break; }
            if (valFlag) {
                ++i;
                continue;
            }
            if (!a.empty() && a[0] != L'-') {
                name = a;
                break;
            }
        }
    }
    if (name.empty())
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box create <name> [--type <t>] "
                         L"[--location <dir>] [--temp] [--v2-delete] "
                         L"[--auto-recover] [--block-net] [--drop-admin] "
                         L"[--template <tpl>]...");
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid box name '" + name + L"' (max 38 chars of "
                         L"A-Z a-z 0-9 _; reserved words excluded)");
    if (!location.empty()
        && GetFileAttributesW(location.c_str()) != INVALID_FILE_ATTRIBUTES
        && !DirExistsW(location))
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"--location exists but is not a directory");
    // 盘相对形（"D:foo"——缺 "\"）拒绝：FileRootPath 语义要求绝对路径，
    // 盘相对会随进程 CWD 漂移（实测坑：shell 参数转换吞反斜杠后静默落盘）
    if (!location.empty() && location.size() >= 2 && location[1] == L':'
        && (location.size() == 2 || location[2] != L'\\'))
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"--location must be absolute (X:\\...; got '"
                             + location + L"')");

    // --type 预设兼容（与旗标叠加）
    std::wstring typeStr = boxproc::OptionValue(ctx.args, L"--type");
    const model::BoxTypePreset* preset = nullptr;
    if (!typeStr.empty()) {
        preset = model::FindBoxTypePreset(typeStr);
        if (!preset)
            return EmitError(ctx.opts, SbieStatus::INVALID,
                             L"unknown box type '" + typeStr
                             + L"' (see: sbie-cli box types)");
    }

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);

    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                         L"box create requires SbieSvc (write path)");

    // 键组（顺序写入，refresh 只在收尾——与 ApplyBoxTypeKeys 同款节流）
    std::vector<std::pair<std::wstring, std::wstring>> keys;
    if (preset)
        for (const auto& k : preset->keys)
            keys.emplace_back(k.key, k.value);
    if (!location.empty()) {
        std::wstring root = location;
        while (!root.empty() && root.back() == L'\\')
            root.pop_back();
        keys.emplace_back(L"FileRootPath", root + L"\\" + name);
    }
    if (temp) {
        keys.emplace_back(L"AutoDelete", L"y");
        keys.emplace_back(L"AutoRemove", L"y");
    }
    if (v2del) {
        keys.emplace_back(L"UseFileDeleteV2", L"y");
        keys.emplace_back(L"UseRegDeleteV2", L"y");
    }
    if (autoRec)
        keys.emplace_back(L"AutoRecover", L"y");
    if (blockNet)
        keys.emplace_back(L"AllowNetworkAccess", L"n");
    if (dropAdmin)
        keys.emplace_back(L"DropAdminRights", L"y");

    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box create failed");
    if (exists)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' already exists");

    st = svc.IniSetSetting(name, L"Enabled", L"y",
                           svc::SvcClient::SetMode::Update,
                           keys.empty() /*无键时 refresh 收在 Enabled*/, pw);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box create failed"
                             + std::wstring(boxproc::PasswordHint(st, pw)));

    for (size_t i = 0; i < keys.size(); ++i) {
        // append=true 的预设键（Template=RpcPortBindingsExt）用 Append 模式
        const bool append = preset && i < preset->keys.size()
                            && preset->keys[i].append;
        st = svc.IniSetSetting(
            name, keys[i].first, keys[i].second,
            append ? svc::SvcClient::SetMode::Append
                   : svc::SvcClient::SetMode::Update,
            i + 1 == keys.size() /*最后一个键 refresh 收尾*/, pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"box create failed writing '"
                                 + keys[i].first + L"'"
                                 + boxproc::PasswordHint(st, pw));
    }

    // 可选模板激活（与原实现同序）
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

    EmitMessage(ctx.opts, L"box '" + name + L"' created"
                            + (preset ? L" (type: "
                                        + std::wstring(preset->type) + L")"
                                      : L""));
    return 0;
}

// ---------------------------------------------------------------------------
// box info（07-P2-2 type 派生 + 06 P2-4 empty/initialized）
// ---------------------------------------------------------------------------

namespace {

struct BoxExtras {
    std::wstring type;
    bool neverDelete = false;
    bool autoDelete = false;
    bool empty = true;
    bool initialized = false;
};

BoxExtras GatherExtras(const std::wstring& name, const std::wstring& fileRoot)
{
    BoxExtras e;
    e.type = DeriveBoxType(name);
    e.neverDelete = BoxKeyYes(name, L"NeverDelete");
    e.autoDelete = BoxKeyYes(name, L"AutoDelete");
    // 06 P2-4：IsEmpty = FileRoot 不存在；IsInitialized = FileRoot 下存在
    // drive/user/share 标准子目录之一（QSbieAPI 语义）
    e.empty = !DirExistsW(fileRoot);
    if (!e.empty) {
        e.initialized = DirExistsW(fileRoot + L"\\drive")
                        || DirExistsW(fileRoot + L"\\user")
                        || DirExistsW(fileRoot + L"\\share");
    }
    return e;
}

int RenderBoxInfoD3Ipc(const GlobalOptions& o, const json::JsonValue& data)
{
    std::wstring name, fileRoot;
    if (const json::JsonValue* v = data.find(L"name"); v && v->isString())
        name = v->asString();
    if (const json::JsonValue* v = data.find(L"file_root"); v && v->isString())
        fileRoot = v->asString();
    const BoxExtras e = GatherExtras(name, fileRoot);

    json::JsonValue obj = data.isObject() ? data : json::JsonValue::Object();
    obj.set(L"type", json::JsonValue(e.type));
    obj.set(L"never_delete", json::JsonValue(e.neverDelete));
    obj.set(L"auto_delete", json::JsonValue(e.autoDelete));
    obj.set(L"empty", json::JsonValue(e.empty));
    obj.set(L"initialized", json::JsonValue(e.initialized));
    return ipcroute::RenderKv(
        o,
        { { L"name", L"name" },
          { L"type", L"type" },
          { L"enabled", L"enabled" },
          { L"file_root", L"file_root" },
          { L"reg_root", L"reg_root" },
          { L"ipc_root", L"ipc_root" },
          { L"has_processes", L"has_processes" },
          { L"has_snapshots", L"has_snapshots" },
          { L"never_delete", L"never_delete" },
          { L"auto_delete", L"auto_delete" },
          { L"empty", L"empty" },
          { L"initialized", L"initialized" } },
        obj);
}

} // namespace

int CmdBoxInfoD3(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box info <name>");

    // IPC 优先（box.info 基础字段），派生字段本地补算（纯键面/文件事实）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", pos[0]);
        ipcroute::Result r = ipcroute::Invoke(ctx.opts, ipc::kOpBoxInfo,
                                              params, true, RenderBoxInfoD3Ipc);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    SbieStatus st = repo.GetInfo(pos[0], &bi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + pos[0] + L"' not found");

    const BoxExtras e = GatherExtras(bi.name, bi.fileRoot);
    model::SnapshotManager sm(bi);

    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"name", bi.name },
        { L"type", e.type },
        { L"enabled", bi.enabled ? L"yes" : L"no" },
        { L"file_root", bi.fileRoot },
        { L"reg_root", bi.regRoot },
        { L"ipc_root", bi.ipcRoot },
        { L"has_processes", bi.hasProcesses ? L"yes" : L"no" },
        { L"has_snapshots", sm.HasAny() ? L"yes" : L"no" },
        { L"never_delete", e.neverDelete ? L"yes" : L"no" },
        { L"auto_delete", e.autoDelete ? L"yes" : L"no" },
        { L"empty", e.empty ? L"yes" : L"no" },
        { L"initialized", e.initialized ? L"yes" : L"no" },
    };
    json::JsonValue obj = json::JsonValue::Object();
    obj.set(L"name", json::JsonValue(bi.name));
    obj.set(L"type", json::JsonValue(e.type));
    obj.set(L"enabled", json::JsonValue(bi.enabled));
    obj.set(L"file_root", json::JsonValue(bi.fileRoot));
    obj.set(L"reg_root", json::JsonValue(bi.regRoot));
    obj.set(L"ipc_root", json::JsonValue(bi.ipcRoot));
    obj.set(L"has_processes", json::JsonValue(bi.hasProcesses));
    obj.set(L"has_snapshots", json::JsonValue(sm.HasAny()));
    obj.set(L"never_delete", json::JsonValue(e.neverDelete));
    obj.set(L"auto_delete", json::JsonValue(e.autoDelete));
    obj.set(L"empty", json::JsonValue(e.empty));
    obj.set(L"initialized", json::JsonValue(e.initialized));
    EmitKv(ctx.opts, kv, obj);
    return 0;
}

// ---------------------------------------------------------------------------
// box list --type <t>（07-P2-2）
// ---------------------------------------------------------------------------

int CmdBoxListD3(const CommandContext& ctx)
{
    const std::wstring typeFilter = boxproc::OptionValue(ctx.args,
                                                         L"--type");
    if (typeFilter.empty())
        return CmdBoxList(ctx);
    if (!KnownBoxType(typeFilter))
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"unknown type '" + typeFilter
                         + L"' (hardening|hardened-plus|standard|"
                           L"standard-plus|app|app-plus|insecure|private)");

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    // 派生过滤 = 键面读，本地事实（IPC box.list 无类型列——过滤后仍走本地
    // 渲染，列契约与原 box list 一致）
    srvconn::NoteDegraded();
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    std::vector<model::BoxInfo> boxes = repo.EnumBoxes(false);

    util::TablePrinter t;
    t.AddColumn(L"NAME");
    t.AddColumn(L"ENABLED");
    t.AddColumn(L"ACTIVE_PROCS", true);
    t.AddColumn(L"FILE_ROOT");
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& b : boxes) {
        if (_wcsicmp(DeriveBoxType(b.name).c_str(), typeFilter.c_str()) != 0)
            continue;
        ULONG procs = 0;
        std::vector<ULONG> pids;
        if (drv::EnumBoxProcesses(b.name, false, &pids) == SbieStatus::OK)
            procs = (ULONG)pids.size();
        t.AddRow({ b.name, b.enabled ? L"yes" : L"no",
                   std::to_wstring(procs), b.fileRoot });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"name", json::JsonValue(b.name));
        r.set(L"type", json::JsonValue(DeriveBoxType(b.name)));
        r.set(L"enabled", json::JsonValue(b.enabled));
        r.set(L"active_procs", json::JsonValue((long long)procs));
        r.set(L"file_root", json::JsonValue(b.fileRoot));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"no boxes");
    return 0;
}

// ---------------------------------------------------------------------------
// box snapshot default <box> [<id>|--clear]（06 P2-3）
// ---------------------------------------------------------------------------

int CmdBoxSnapshotDefault(const CommandContext& ctx)
{
    // rest = [box, <id>...] + --clear
    std::vector<std::wstring> pos;
    bool clear = false;
    for (size_t i = 3; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--clear")
            clear = true;
        else if (ctx.args[i] == L"--json" || ctx.args[i] == L"--quiet"
                 || ctx.args[i] == L"-q")
            continue;
        else if (!ctx.args[i].empty() && ctx.args[i][0] == L'-')
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
        else
            pos.push_back(ctx.args[i]);
    }
    if (pos.empty() || pos.size() > 2 || (clear && pos.size() > 1))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box snapshot default <box> "
                         L"[<snapshot-id>] [--clear]");

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    if (repo.GetInfo(pos[0], &bi) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + pos[0] + L"' not found");

    model::SnapshotManager sm(bi);
    std::wstring currentId, defaultId;
    std::vector<model::SnapshotInfo> snaps = sm.List(&currentId, &defaultId);

    const std::wstring iniPath = bi.fileRoot + L"\\Snapshots.ini";
    if (pos.size() == 1 && !clear) {
        // 显示
        auto nameOf = [&](const std::wstring& id) {
            if (id.empty())
                return std::wstring(L"-");
            for (const auto& s : snaps)
                if (_wcsicmp(s.id.c_str(), id.c_str()) == 0)
                    return s.name;
            return id;
        };
        if (ctx.opts.json) {
            json::JsonValue d = json::JsonValue::Object();
            d.set(L"box", json::JsonValue(bi.name));
            d.set(L"current", json::JsonValue(currentId));
            d.set(L"default", json::JsonValue(defaultId));
            EmitJsonOk(ctx.opts, d);
        } else {
            EmitKv(ctx.opts,
                   { { L"current",
                       currentId + (currentId.empty()
                                        ? L""
                                        : L" (" + nameOf(currentId) + L")") },
                     { L"default",
                       defaultId + (defaultId.empty()
                                        ? L""
                                        : L" (" + nameOf(defaultId) + L")") } },
                   json::JsonValue::Object());
        }
        return 0;
    }

    const std::wstring id = clear ? std::wstring() : pos[1];
    if (!clear) {
        bool hit = false;
        for (const auto& s : snaps)
            if (_wcsicmp(s.id.c_str(), id.c_str()) == 0)
                hit = true;
        if (!hit)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"snapshot not found: " + id);
    }
    // [Current] Default=<id>（QSbieAPI SetDefaultSnapshot 键面；UTF-8 无 BOM
    // 与 SnapshotManager::SaveIniFile 同形态——WritePrivateProfileStringW 的
    // ANSI 往返会破坏非 ASCII 快照名，故行级改写）
    if (!SetSnapshotsDefaultKey(iniPath, clear ? std::wstring() : id))
        return EmitError(ctx.opts, SbieStatus::GENERIC,
                         L"failed to write " + iniPath);
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"box", json::JsonValue(bi.name));
        d.set(L"default", json::JsonValue(clear ? L"" : id));
        d.set(L"message",
              json::JsonValue(clear ? L"default snapshot marker cleared"
                                    : L"default snapshot set to " + id));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts, clear ? L"default snapshot marker cleared"
                                    : L"default snapshot set to " + id);
    }
    return 0;
}

int CmdBoxSnapshotD3(const CommandContext& ctx)
{
    if (ctx.args.size() > 2 && ctx.args[2] == L"default")
        return CmdBoxSnapshotDefault(ctx);
    return CmdBoxSnapshot(ctx);
}

// ---------------------------------------------------------------------------
// box explore <name>（06 P2-13）
// ---------------------------------------------------------------------------

int CmdBoxExplore(const CommandContext& ctx)
{
    std::vector<std::wstring> pos;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        const std::wstring& a = ctx.args[i];
        if (a == L"--json" || a == L"--quiet" || a == L"-q")
            continue;
        if (!a.empty() && a[0] == L'-')
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + a);
        pos.push_back(a);
    }
    if (pos.size() != 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box explore <name>");

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (host explorer; no server op needed)");

    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    if (repo.GetInfo(pos[0], &bi) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + pos[0] + L"' not found");

    if (!DirExistsW(bi.fileRoot))
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + pos[0] + L"' is empty (no file root at "
                           + bi.fileRoot + L")");

    // SbieCtrl ID_SANDBOX_EXPLORE 同语义：宿主 explorer 打开 FileRoot
    // （返回值 > 32 = 成功，ShellExecuteW 惯例）
    const HINSTANCE hr = ShellExecuteW(nullptr, L"open",
                                       L"explorer.exe",
                                       bi.fileRoot.c_str(), nullptr, SW_SHOWNORMAL);
    if ((ULONG_PTR)hr <= 32)
        return EmitError(ctx.opts, SbieStatus::GENERIC,
                         L"failed to open explorer on " + bi.fileRoot);

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"box", json::JsonValue(bi.name));
        d.set(L"path", json::JsonValue(bi.fileRoot));
        d.set(L"message",
              json::JsonValue(L"exploring " + bi.fileRoot));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts, L"exploring " + bi.fileRoot);
    }
    return 0;
}

} // namespace sbie::cli
