// Sandboxie-OSS — sbie-cli/cli/Commands/proc_d3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 波次 D3 小件（docs/04 §19；注册统一在 Commands_D3.cpp）：
//
//   * proc suspend-box <box> | --all   （06 P2-6 + 07 补充的全局形）
//     proc resume-box  <box> | --all
//     SbieSvc MSGID_PROCESS_SUSPEND_RESUME_ALL（SvcClient::SuspendResumeAll
//     已实现未接线——本波接线；全局形 = EnumBoxes 逐箱）。
//     波次 E（08-P2-4）：server op proc.suspendBox / proc.resumeBox 已接线
//     （params {box?}，box 空 = 全局）——IPC 优先，降级直连 SbieSvc 消息面。
//   * proc info 增强（06 P2-5，本波接手续完）：基础字段后追加派生列——
//     flags 位名解码（SBIE_FLAG_*，vendor/api_flags.h 常量表）、image type
//     （SbieApi_QueryProcessInfo 'gpit' → GPL core dll.h:85-121 的
//     DLL_IMAGE_* 枚举名；(ULONG)-1 = 未初始化，process.c:686）、elevated
//     令牌（Win32 OpenProcessToken TokenElevation——非驱动面，'ptok' 句柄
//     路径 docs/02 §3.3 明示勿用）与 wow64（IsWow64Process）。
//     IPC 路径同样增强（proc.info 回传数据上本地补列——与 box info D3 同
//     模式）；无 server op 变更。
//   * proc exempt <pid> <on|off|get> [--what internet|spooler]（06 P2-14）：
//     API_PROCESS_EXEMPTION_CONTROL（api_defs.h +65）直投 ioctl——
//     action_id 'inet'=AllowInternetAccess / 'splr'=ipc_allowSpoolerPrintToFile
//     （GPL core api.c:1108-1163：调用方须非沙箱、目标须沙箱进程；
//     Process_Find 失败回 STATUS_NOT_FOUND）。纯 client，无 server op。
//   * cfg whoami（06 P2-12）：SbieSvc IniGetUser 三元组
//     （用户名 / 节名 / 是否管理员）——多用户环境排障（写键落在哪个用户节）。
//     读命令，client 直连 SbieSvc。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/Boxes.h"
#include "Model/Processes.h"

// vendor ABI 头（api_defs.h：API_PROCESS_EXEMPTION_CONTROL_ARGS 等；
// api_flags.h：SBIE_FLAG_* 位常量——均为 GPL core 直接复制不改，01-license-map）
#include "api_defs.h"
#include "api_flags.h"

#include <cwchar>
#include <cstring>
#include <iterator>
#include <utility>

namespace sbie::cli {

namespace {

void NoteDirect(const GlobalOptions& o, const wchar_t* what)
{
    if (o.showTransport)
        Diag(std::wstring(L"transport: direct (") + what + L")");
}

// <box> | --all 解析 + 目标箱集；返回 0 = 成功，否则已 EmitError 的退出码
int CollectBoxTargets(const CommandContext& ctx, std::wstring* box,
                      std::vector<std::wstring>* targets)
{
    std::vector<std::wstring> pos;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--all" || ctx.args[i] == L"--json"
            || ctx.args[i] == L"--quiet" || ctx.args[i] == L"-q")
            continue;
        if (!ctx.args[i].empty() && ctx.args[i][0] == L'-')
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
        pos.push_back(ctx.args[i]);
    }
    const bool all = boxproc::HasFlag(ctx.args, L"--all");
    if (pos.empty() && !all)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc suspend-box <box>|--all");
    if (!pos.empty() && all)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"--all takes no box argument");
    *box = pos.empty() ? std::wstring() : pos[0];
    if (all) {
        if (drv::EnumBoxes(targets, false) != SbieStatus::OK)
            return EmitError(ctx.opts, SbieStatus::DRIVER_UNAVAILABLE,
                             L"failed to enumerate boxes");
    } else {
        bool enabled = false, exists = false;
        if (!Ok(drv::IsBoxEnabled(*box, &enabled, &exists)) || !exists)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"box not found: " + *box);
        targets->push_back(*box);
    }
    return 0;
}

int RunSuspendBox(const CommandContext& ctx, bool suspend)
{
    const wchar_t* verb = suspend ? L"suspend-box" : L"resume-box";
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::wstring box;
    std::vector<std::wstring> targets;
    const int parseRc = CollectBoxTargets(ctx, &box, &targets);
    if (parseRc != 0)
        return parseRc;

    // IPC 优先（proc.suspendBox / proc.resumeBox——波次 E 08-P2-4 收口了 D3
    // 的"server op 规格待接线"自注；box 空 = 全局）。写路径非幂等 retry=false；
    // data 与下方直连路径同形（box/boxes/suspended/count/message）。
    {
        json::JsonValue params = json::JsonValue::Object();
        if (!box.empty())
            ipcroute::PSet(&params, L"box", box);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, suspend ? ipc::kOpProcSuspendBox : ipc::kOpProcResumeBox,
            params, false,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                if (o.json) {
                    EmitJsonOk(o, data);
                    return 0;
                }
                const json::JsonValue* m = data.isObject()
                    ? data.find(L"message") : nullptr;
                util::PrintLineUtf8(util::WideToUtf8(
                    m && m->isString() ? m->asString()
                                       : std::wstring(L"0 process(es)")));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连降级（server 缺席 / op 未实现）
    NoteDirect(ctx.opts, L"SbieSvc SuspendResumeAll");
    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                         L"proc " + std::wstring(verb)
                             + L" requires SbieSvc");

    ULONG total = 0;
    json::JsonValue boxes = json::JsonValue::Array();
    for (const auto& b : targets) {
        std::vector<ULONG> pids;
        if (drv::EnumBoxProcesses(b, false, &pids) != SbieStatus::OK)
            continue;
        const SbieStatus st = svc.SuspendResumeAll(b, suspend);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             std::wstring(verb) + L" failed for box '" + b
                                 + L"' (SbieSvc required)");
        total += (ULONG)pids.size();
        boxes.pushBack(json::JsonValue(b));
    }

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        if (!box.empty())
            d.set(L"box", json::JsonValue(box));
        d.set(L"boxes", std::move(boxes));
        d.set(L"suspended", json::JsonValue(suspend));
        d.set(L"count", json::JsonValue((long long)total));
        d.set(L"message",
              json::JsonValue(std::wstring(verb) + L": "
                              + std::to_wstring(total)
                              + L" process(es) in "
                              + std::to_wstring(targets.size())
                              + L" box(es)"));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts,
                    std::wstring(verb) + L": " + std::to_wstring(total)
                        + L" process(es) in "
                        + std::to_wstring(targets.size()) + L" box(es)");
    }
    return 0;
}

} // namespace

int CmdProcSuspendBox(const CommandContext& ctx)
{
    return RunSuspendBox(ctx, true);
}

int CmdProcResumeBox(const CommandContext& ctx)
{
    return RunSuspendBox(ctx, false);
}

// ---------------------------------------------------------------------------
// sbie cfg whoami（06 P2-12）
// ---------------------------------------------------------------------------

int CmdCfgWhoami(const CommandContext& ctx)
{
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--json" || ctx.args[i] == L"--quiet"
            || ctx.args[i] == L"-q")
            continue;
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unexpected argument: " + ctx.args[i]);
    }
    NoteDirect(ctx.opts, L"SbieSvc IniGetUser");

    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                         L"cfg whoami requires SbieSvc");
    bool admin = false;
    std::wstring section, name;
    const SbieStatus st = svc.IniGetUser(&admin, &section, &name);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"whoami failed (SbieSvc)");

    // 用户节是否存在（EnumConfSections 全节枚举——用户节不是 box，
    // IsBoxEnabled 对它恒 false）
    bool sectionExists = false;
    if (drv::Loaded() || drv::LoadSbieDll(ctx.opts.sbieDllPath)) {
        std::vector<std::wstring> secs;
        if (Ok(drv::EnumConfSections(&secs)))
            for (const auto& s : secs)
                if (_wcsicmp(s.c_str(), section.c_str()) == 0)
                    sectionExists = true;
    }

    EmitKv(ctx.opts,
           { { L"name", name },
             { L"section", section },
             { L"admin", admin ? L"yes" : L"no" },
             { L"section_exists", sectionExists ? L"yes" : L"no" } },
           [&] {
               json::JsonValue o = json::JsonValue::Object();
               o.set(L"name", json::JsonValue(name));
               o.set(L"section", json::JsonValue(section));
               o.set(L"admin", json::JsonValue(admin));
               o.set(L"section_exists", json::JsonValue(sectionExists));
               return o;
           }());
    return 0;
}

// ---------------------------------------------------------------------------
// proc info 增强（06 P2-5）：flags 位名 + image type + elevated/wow64
// ---------------------------------------------------------------------------

namespace {

// SBIE_FLAG_* 位名（vendor/api_flags.h:92-125，只列非废弃位）
struct FlagBit {
    ULONG bit;
    const wchar_t* name;
};
const FlagBit kFlagBits[] = {
    { SBIE_FLAG_VALID_PROCESS,        L"valid_process" },
    { SBIE_FLAG_FORCED_PROCESS,       L"forced" },
    { SBIE_FLAG_PROCESS_IS_START_EXE, L"start_exe" },
    { SBIE_FLAG_PARENT_WAS_START_EXE, L"parent_start_exe" },
    { SBIE_FLAG_IMAGE_FROM_SBIE_DIR,  L"image_from_sbie_dir" },
    { SBIE_FLAG_IMAGE_FROM_SANDBOX,   L"image_from_sandbox" },
    { SBIE_FLAG_DROP_RIGHTS,          L"drop_rights" },
    { SBIE_FLAG_RIGHTS_DROPPED,       L"rights_dropped" },
    { SBIE_FLAG_FAKE_ADMIN,           L"fake_admin" },
    { SBIE_FLAG_OPEN_ALL_WIN_CLASS,   L"open_all_win_class" },
    { SBIE_FLAG_WIN32K_HOOKABLE,      L"win32k_hookable" },
    { SBIE_FLAG_PROCESS_IN_APP_PKG,   L"in_app_pkg" },
    { SBIE_FLAG_APP_COMPARTMENT,      L"app_compartment" },
    { SBIE_FLAG_PRIVACY_MODE,         L"privacy_mode" },
    { SBIE_FLAG_RULE_SPECIFICITY,     L"rule_specificity" },
    { SBIE_FLAG_PROCESS_IN_PCA_JOB,   L"in_pca_job" },
    { SBIE_FLAG_CREATE_CONSOLE_HIDE,  L"create_console_hide" },
    { SBIE_FLAG_CREATE_CONSOLE_SHOW,  L"create_console_show" },
    { SBIE_FLAG_PROTECTED_PROCESS,    L"protected_process" },
    { SBIE_FLAG_HOST_INJECT_PROCESS,  L"host_inject" },
};

// DLL_IMAGE_* 枚举名（GPL core dll.h:85-121；键面来源 01-license-map 允许的
// core 引用，非 SandMan）。detected_image_type 初值 (ULONG)-1 = 未初始化
// （process.c:686）。
const wchar_t* const kImageTypeNames[] = {
    L"unspecified",              L"sandboxie_rpcss",
    L"sandboxie_dcomlaunch",     L"sandboxie_crypto",
    L"sandboxie_wuau",           L"sandboxie_bits",
    L"sandboxie_sbiesvc",        L"msi_installer",
    L"trusted_installer",        L"wuauclt",
    L"shell_explorer",           L"internet_explorer",
    L"mozilla_firefox",          L"windows_media_player",
    L"nullsoft_winamp",          L"pandora_kmplayer",
    L"windows_live_mail",        L"service_model_reg",
    L"rundll32",                 L"dllhost",
    L"dllhost_wininet_cache",    L"wisptis",
    L"google_chrome",            L"google_update",
    L"acrobat_reader",           L"office_outlook",
    L"office_excel",             L"flash_player_sandbox_obsolete",
    L"plugin_container",         L"other_web_browser",
    L"other_mail_client",        L"mozilla_thunderbird",
};

struct ProcExtras {
    long long imageTypeId = 0;
    std::wstring imageType;          // 名或 "#<n>"；空 = 查询不可用
    std::vector<std::wstring> flags; // 位名列表
    int elevated = -1;               // -1 = 未知（句柄打开失败）
    int wow64 = -1;
};

ProcExtras GatherProcExtras(ULONG pid, ULONG flags)
{
    ProcExtras e;
    for (const FlagBit& f : kFlagBits)
        if ((flags & f.bit) != 0)
            e.flags.push_back(f.name);

    if (drv::ApiP()->SbieApi_QueryProcessInfo) {
        // 'gpit' = proc->detected_image_type（process_api.c:546-549）。
        // 02 §7 坑 4：返回 0 与"失败"不可区分——QSbieAPI 同款含糊，接受。
        const ULONG64 v = drv::ApiP()->SbieApi_QueryProcessInfo(
            (HANDLE)(ULONG_PTR)pid, 'gpit');
        e.imageTypeId = (long long)v;
        if (v == (ULONG64)(ULONG)-1)
            e.imageType = L"uninitialized";
        else if (v < std::size(kImageTypeNames))
            e.imageType = kImageTypeNames[(size_t)v];
        else
            e.imageType = L"#" + std::to_wstring((unsigned long long)v);
    }

    // 令牌提升位与 wow64：普通 Win32 查询（客户端视角，非驱动面）
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        HANDLE tok = nullptr;
        if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION te{};
            DWORD cb = 0;
            if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb))
                e.elevated = te.TokenIsElevated ? 1 : 0;
            CloseHandle(tok);
        }
        BOOL w = FALSE;
        if (IsWow64Process(h, &w))
            e.wow64 = w ? 1 : 0;
        CloseHandle(h);
    }
    return e;
}

// 追加进 JSON 对象 + 键值表（IPC 与直连两路径共用）
void ApplyProcExtras(const ProcExtras& e, json::JsonValue* obj,
                     std::vector<std::pair<std::wstring, std::wstring>>* kv)
{
    std::wstring joined;
    for (const auto& f : e.flags) {
        if (!joined.empty())
            joined += L",";
        joined += f;
    }
    json::JsonValue fl = json::JsonValue::Array();
    for (const auto& f : e.flags)
        fl.pushBack(json::JsonValue(f));
    obj->set(L"image_type", json::JsonValue(e.imageType));
    obj->set(L"image_type_id", json::JsonValue(e.imageTypeId));
    obj->set(L"flags_decoded", std::move(fl));
    obj->set(L"elevated", json::JsonValue((long long)e.elevated));
    obj->set(L"wow64", json::JsonValue((long long)e.wow64));
    kv->emplace_back(L"image_type",
                     e.imageType.empty()
                         ? L"-" + std::to_wstring(e.imageTypeId)
                         : e.imageType + L" (" + std::to_wstring(e.imageTypeId)
                               + L")");
    kv->emplace_back(L"flags_decoded", joined.empty() ? L"-" : joined);
    kv->emplace_back(L"elevated",
                     e.elevated < 0 ? L"?" : (e.elevated ? L"yes" : L"no"));
    kv->emplace_back(L"wow64",
                     e.wow64 < 0 ? L"?" : (e.wow64 ? L"yes" : L"no"));
}

// IPC proc.info 回传 → 增强渲染（06 P2-5 列附加在 server 数据之后）
int RenderProcInfoD3Ipc(const GlobalOptions& o, const json::JsonValue& data)
{
    ULONG pid = 0, flags = 0;
    if (const json::JsonValue* v = data.find(L"pid");
        v && v->isInt())
        pid = (ULONG)v->asInt();
    if (const json::JsonValue* v = data.find(L"flags");
        v && v->isInt())
        flags = (ULONG)v->asInt();

    const ProcExtras e = GatherProcExtras(pid, flags);
    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"pid", std::to_wstring(pid) },
        { L"box", L"" },
        { L"image", L"" },
        { L"command_line", L"" },
        { L"working_dir", L"" },
        { L"parent_pid", L"" },
        { L"session", L"" },
        { L"started", L"" },
        { L"suspended", L"" },
        { L"flags", std::to_wstring(flags) },
    };
    json::JsonValue obj = data.isObject() ? data : json::JsonValue::Object();
    ApplyProcExtras(e, &obj, &kv);
    return ipcroute::RenderKv(
        o,
        { { L"pid", L"pid" },
          { L"box", L"box" },
          { L"image", L"image" },
          { L"command_line", L"cmdline" },
          { L"working_dir", L"workdir" },
          { L"parent_pid", L"parent_pid" },
          { L"session", L"session" },
          { L"started", L"started" },
          { L"suspended", L"suspended" },
          { L"flags", L"flags" },
          { L"image_type", L"image_type" },
          { L"flags_decoded", L"flags_decoded" },
          { L"elevated", L"elevated" },
          { L"wow64", L"wow64" } },
        obj);
}

} // namespace

int CmdProcInfoD3(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc info <pid>");
    ULONG pid = 0;
    if (!boxproc::ParseUlong(pos[0], &pid))
        return EmitError(ctx.opts, SbieStatus::USAGE, L"bad pid: " + pos[0]);

    // IPC 优先（proc.info 基础字段），派生列本地补算
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"pid", (long long)pid);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpProcInfo, params, true, RenderProcInfoD3Ipc);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();

    drv::ProcQuery q;
    if (drv::QueryProcessById(pid, &q) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"pid " + std::to_wstring(pid)
                             + L" is not a running sandboxed process");

    model::ProcessRepository repo(nullptr, svc::SvcClient::Instance());
    svc::ProcInfo pi;
    SbieStatus st = repo.Info(pid, &pi);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"proc info failed (SbieSvc required)");

    const ProcExtras e = GatherProcExtras(pid, pi.flags);

    wchar_t flagsHex[16];
    swprintf_s(flagsHex, L"0x%08lX", pi.flags);
    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        { L"pid", std::to_wstring(pid) },
        { L"box", q.box },
        { L"image", pi.image.empty() ? q.image : pi.image },
        { L"command_line", pi.cmdline },
        { L"working_dir", pi.workdir },
        { L"parent_pid", std::to_wstring(pi.parentId) },
        { L"session", std::to_wstring(q.sessionId) },
        { L"started", boxproc::FormatCreateTime(q.createTime) },
        { L"suspended", pi.suspended ? L"yes" : L"no" },
        { L"flags", std::to_wstring(pi.flags) },
        { L"flags_hex", flagsHex },
    };
    json::JsonValue obj = json::JsonValue::Object();
    obj.set(L"pid", json::JsonValue((long long)pid));
    obj.set(L"box", json::JsonValue(q.box));
    obj.set(L"image", json::JsonValue(pi.image.empty() ? q.image : pi.image));
    obj.set(L"cmdline", json::JsonValue(pi.cmdline));
    obj.set(L"workdir", json::JsonValue(pi.workdir));
    obj.set(L"parent_pid", json::JsonValue((long long)pi.parentId));
    obj.set(L"session", json::JsonValue((long long)q.sessionId));
    obj.set(L"started",
            json::JsonValue(boxproc::FormatCreateTime(q.createTime)));
    obj.set(L"suspended", json::JsonValue(pi.suspended));
    obj.set(L"flags", json::JsonValue((long long)pi.flags));
    obj.set(L"flags_hex", json::JsonValue(flagsHex));

    ApplyProcExtras(e, &obj, &kv);
    EmitKv(ctx.opts, kv, obj);
    return 0;
}

// ---------------------------------------------------------------------------
// proc exempt <pid> <on|off|get> [--what internet|spooler]（06 P2-14）
// ---------------------------------------------------------------------------

int CmdProcExempt(const CommandContext& ctx)
{
    // 参数：<pid> <on|off|get>；--what internet（缺省）| spooler
    std::vector<std::wstring> pos;
    std::wstring what = L"internet";
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        const std::wstring& a = ctx.args[i];
        if (a == L"--what" && i + 1 < ctx.args.size()) {
            what = ctx.args[++i];
        } else if (a == L"--json" || a == L"--quiet" || a == L"-q") {
            ;
        } else if (!a.empty() && a[0] == L'-') {
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"unknown option: " + a);
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() != 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli proc exempt <pid> <on|off|get> "
                         L"[--what internet|spooler]");
    ULONG pid = 0;
    if (!boxproc::ParseUlong(pos[0], &pid))
        return EmitError(ctx.opts, SbieStatus::USAGE, L"bad pid: " + pos[0]);
    int set = -1;   // -1 = get
    if (pos[1] == L"on")
        set = 1;
    else if (pos[1] == L"off")
        set = 0;
    else if (pos[1] != L"get")
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"action must be on|off|get");
    ULONG action = 0;
    if (what == L"internet")
        action = 'inet';
    else if (what == L"spooler")
        action = 'splr';
    else
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"--what must be internet or spooler");

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (SbieApi ioctl API_PROCESS_EXEMPTION_"
             L"CONTROL; no server op needed)");

    if (!drv::ApiP()->SbieApi_Ioctl)
        return EmitError(ctx.opts, SbieStatus::ERR_SBIEDLL,
                         L"SbieDll.dll bindings unavailable");

    // API_PROCESS_EXEMPTION_CONTROL（api_defs.h:430-435；驱动端语义
    // api.c:1108-1163：set/get 指针可同帧，set 优先执行）
    ULONG setValue = set < 0 ? 0 : (set ? 1u : 0u);
    ULONG getValue = 0;
    __declspec(align(8)) ULONG64 parms[API_NUM_ARGS];
    API_PROCESS_EXEMPTION_CONTROL_ARGS* args =
        (API_PROCESS_EXEMPTION_CONTROL_ARGS*)parms;
    memset(parms, 0, sizeof(parms));
    args->func_code   = API_PROCESS_EXEMPTION_CONTROL;
    args->process_id.val = (HANDLE)(ULONG_PTR)pid;
    args->action_id.val  = action;
    args->set_flag.val   = set < 0 ? nullptr : &setValue;
    args->get_flag.val   = &getValue;
    const LONG rc = drv::ApiP()->SbieApi_Ioctl(parms);
    if (rc < 0) {
        if ((ULONG)rc == 0xC0000022UL /*STATUS_ACCESS_DENIED*/)
            return EmitError(ctx.opts, SbieStatus::ACCESS_DENIED,
                             L"exemption control denied (sandboxed caller or "
                               L"driver restriction)");
        if ((ULONG)rc == 0xC0000225UL /*STATUS_NOT_FOUND*/
            || (ULONG)rc == 0xC0000001UL /*STATUS_UNSUCCESSFUL*/
            || (ULONG)rc == 0xC0000004UL /*STATUS_INFO_LENGTH_MISMATCH*/)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"pid " + std::to_wstring(pid)
                                 + L" is not a running sandboxed process");
        return EmitError(ctx.opts, FromNtStatus(rc),
                         L"exemption control failed: " + NtStatusText(rc),
                         nullptr);
    }

    const bool state = getValue != 0;
    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"pid", json::JsonValue((long long)pid));
        d.set(L"what", json::JsonValue(what));
        d.set(L"state", json::JsonValue(state));
        d.set(L"message",
              json::JsonValue(L"pid " + std::to_wstring(pid) + L" " + what
                              + L" exemption: " + (state ? L"on" : L"off")));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts,
                    L"pid " + std::to_wstring(pid) + L" " + what
                        + L" exemption: " + (state ? L"on" : L"off"));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// proc start 伪命令路由（07-P2-3 验收发现：裸转发不可用——实测修复）
// ---------------------------------------------------------------------------

// 原实现（proc_cmd.cpp）——非伪命令形态原样委托
int CmdProcStart(const CommandContext& ctx);

// Start.exe 伪命令关键字（GPL core start.cpp:432-438；RunSandboxed 直投
// CreateProcess 对伪串必然 GENERIC——伪命令必须经 Start.exe 解释。QSbieAPI
// 同款差异：RunStartExe 组 "Start.exe /box:<box> <cmd>"、RunSandboxed 不组）
bool IsStartExePseudo(const std::wstring& tok)
{
    return tok == L"default_browser" || tok == L"mail_agent"
           || tok == L"run_dialog" || tok == L"auto_run";
}

int CmdProcStartD3(const CommandContext& ctx)
{
    // 首命令 token（box 之后的第一个位置参数）= 伪关键字 → 改写为
    // "Start.exe <pseudo>" 两 token 后走原实现（其余参数原样）
    static const wchar_t* const kValueFlags[] = {
        L"--index", L"--dir", L"--info", L"--name", L"--section",
        L"--template", L"--password", L"--sbie-dll-path", L"--to", L"--type",
        L"--size-mb",
    };
    size_t cmdTok = std::wstring::npos;
    size_t seenPos = 0;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        bool valueFlag = false;
        for (const wchar_t* f : kValueFlags)
            if (ctx.args[i] == f) { valueFlag = true; break; }
        if (valueFlag) {
            ++i;
            continue;
        }
        if (ctx.args[i].empty() || ctx.args[i][0] == L'-')
            continue;
        if (seenPos == 0) {
            ++seenPos;   // 首位置参数 = box 名
            continue;
        }
        cmdTok = i;      // 第二位置参数 = 首命令 token
        break;
    }
    if (cmdTok == std::wstring::npos
        || !IsStartExePseudo(ctx.args[cmdTok]))
        return CmdProcStart(ctx);

    CommandContext rewritten = ctx;
    rewritten.args = ctx.args;
    rewritten.args[cmdTok] = L"Start.exe";
    rewritten.args.insert(rewritten.args.begin() + (long)cmdTok + 1,
                          ctx.args[cmdTok]);
    if (ctx.opts.showTransport)
        Diag(L"proc start: Start.exe pseudo command rewritten to 'Start.exe "
             + ctx.args[cmdTok] + L"' (07-P2-3)");
    return CmdProcStart(rewritten);
}

} // namespace sbie::cli
