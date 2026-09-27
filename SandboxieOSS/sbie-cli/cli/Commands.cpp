// Sandboxie-OSS — sbie-cli/cli/Commands.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// status/version/box list/proc list（M1 直连实现，04 §5 可降级直连表内）。
// 接线波次：以上读命令 IPC 优先（ipcroute::Invoke → server op），server
// 缺席/断连时降级直连；RegisterCommands() 末尾统一调用各波次 Register*，
// 覆盖 M1 桩行（server/cfg/template/log/box/proc 写路径命令集）。
// 补缺波次（06 缺口表收口）：box enable/disable/clean、proc kill-all/
// suspend/resume、cfg unset/list-setting/lock/unlock 已接线（box_*.cpp /
// proc_cmd.cpp / cfg_*.cpp）。box size/recover 波次（P0-5/P0-12）：box size
// → box_manage.cpp；box recover list/copy/add → box_recover.cpp。
// P1 清尾波次（docs/04 §15）：force on/off/status（P1-1）、maint
// status/start/stop（P1-2，机器级无 op）→ force_cmd.cpp / maint_cmd.cpp；
// proc kill-all --all（P1-3）与 proc start cwd 显式化（P1-7）在 proc_cmd.cpp。

#include "Commands.h"
#include "Output.h"
#include "ServerConnect.h"
#include "Commands/BoxProcCommands.h"
#include "Commands/CfgTemplateCommands.h"
#include "Commands/IpcRoute.h"
#include "Commands/ServerCommands.h"
#include "../ipcc/SbieIpc.h"

#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/Boxes.h"
#include "../../SbieCore/Model/Processes.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/PathMapper.h"
#include "../../SbieCore/Util/Utf8.h"
#include "../../SbieCore/Util/Version.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace sbie::cli {

namespace {

// PsGetProcessCreateTimeQuadPart（100ns，1601 纪元）→ 本地时间 "YYYY-MM-DD HH:MM:SS"
std::wstring FormatCreateTime(ULONG64 t)
{
    if (t == 0)
        return L"-";
    FILETIME utc{ (DWORD)(t & 0xFFFFFFFF), (DWORD)(t >> 32) };
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local)
        || !FileTimeToSystemTime(&local, &st))
        return L"-";
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring AbiHex(ULONG abi)
{
    wchar_t buf[16];
    swprintf_s(buf, L"0x%lX", abi);
    return buf;
}

// 组装 status/version 共用的三段状态
struct Overall {
    bool dllLoaded = false;
    bool driverAlive = false;
    drv::VersionInfo drvVersion;
    unsigned long featureFlags = 0;
    bool svcConnected = false;
    std::wstring svcVersion;
    ULONG svcAbi = 0;
    size_t boxCount = 0;
    std::wstring loadError;
};

Overall Gather(const CommandContext& ctx)
{
    Overall o;
    o.dllLoaded = drv::LoadSbieDll(ctx.opts.sbieDllPath);
    o.loadError = drv::LastLoadError();
    if (o.dllLoaded) {
        o.drvVersion = drv::GetVersion();
        o.driverAlive = drv::DriverAlive();
        if (o.driverAlive)
            drv::QueryFeatureFlags(&o.featureFlags);
        // SbieSvc：LPC 连接 + GET_VERSION（status 的 service 行）
        svc::SvcClient& svc = svc::SvcClient::Instance();
        o.svcConnected = svc.Connected();
        if (o.svcConnected) {
            std::wstring v;
            ULONG abi = 0;
            if (svc.IniGetVersion(&v, &abi) == SbieStatus::OK) {
                o.svcVersion = v;
                o.svcAbi = abi;
            }
        }
        std::vector<std::wstring> boxes;
        if (drv::EnumBoxes(&boxes, false) == SbieStatus::OK)
            o.boxCount = boxes.size();
    } else if (drv::DriverAlive()) {
        // SbieDll 缺席但驱动在场：部分信息仍可报告
        o.driverAlive = true;
    }
    return o;
}

json::JsonValue FeatureNames(unsigned long f)
{
    json::JsonValue arr = json::JsonValue::Array();
    auto add = [&arr](const wchar_t* n) { arr.pushBack(json::JsonValue(n)); };
    if (f & 0x01) add(L"WFP");
    if (f & 0x02) add(L"OB_CALLBACKS");
    if (f & 0x10) add(L"SBIE_LOGIN");
    if (f & 0x20) add(L"WIN32K_HOOK");
    if (f & 0x10000000) add(L"DYNDATA_OK");
    if (f & 0x20000000) add(L"DYNDATA_EXP");
    if (f & 0x40000000) add(L"NEW_ARCH");
    return arr;
}

// ---- IPC 路径渲染（字段名 = server op 契约，与直连 --json 一致） ----------

const json::JsonValue* Field(const json::JsonValue& o, const wchar_t* k)
{
    return o.isObject() ? o.find(k) : nullptr;
}

std::wstring StrOf(const json::JsonValue& o, const wchar_t* k,
                   const std::wstring& def = L"")
{
    const json::JsonValue* v = Field(o, k);
    return (v && v->isString()) ? v->asString() : def;
}

long long IntOf(const json::JsonValue& o, const wchar_t* k, long long def = 0)
{
    const json::JsonValue* v = Field(o, k);
    return (v && v->isInt()) ? v->asInt() : def;
}

bool BoolOf(const json::JsonValue& o, const wchar_t* k, bool def = false)
{
    const json::JsonValue* v = Field(o, k);
    return (v && v->type() == json::JsonValue::Type::Bool) ? v->asBool()
                                                           : def;
}

// sbie version（IPC data：cli/driver/service 三段）
int RenderVersionIpc(const GlobalOptions& opts, const json::JsonValue& data)
{
    if (opts.json) {
        EmitJsonOk(opts, data);
        return 0;
    }
    const json::JsonValue* drv = Field(data, L"driver");
    const json::JsonValue* svc = Field(data, L"service");
    const json::JsonValue* cli = Field(data, L"cli");
    std::wstring out = L"sbie-cli "
        + (cli ? StrOf(*cli, L"version", kCliVersionW)
               : std::wstring(kCliVersionW))
        + L"\n";
    if (drv && BoolOf(*drv, L"available")) {
        out += L"driver " + StrOf(*drv, L"version") + L" (abi "
             + StrOf(*drv, L"abi_hex") + L", "
             + (BoolOf(*drv, L"alive") ? L"alive" : L"driver not running")
             + L")\n";
    } else {
        out += L"driver unavailable\n";
    }
    if (svc && BoolOf(*svc, L"connected") && Field(*svc, L"version")) {
        std::wstring abi = StrOf(*svc, L"abi_hex");
        if (abi.empty())
            abi = AbiHex((ULONG)IntOf(*svc, L"abi"));   // server 只回 int
        out += L"svc " + StrOf(*svc, L"version") + L" (abi " + abi + L")\n";
    } else {
        out += L"svc not connected\n";
    }
    if (drv && BoolOf(*drv, L"available")
        && IntOf(*drv, L"abi") != (long long)kExpectedAbi)
        Diag(L"warning: SbieDll ABI " + StrOf(*drv, L"abi_hex")
             + L" differs from expected " + AbiHex(kExpectedAbi)
             + L" (baseline 5.73.5)");
    util::PrintUtf8(util::WideToUtf8(out));
    return 0;
}

// sbie status（IPC data：driver/features/service/server/boxes）
int RenderStatusIpc(const GlobalOptions& opts, const json::JsonValue& data)
{
    const json::JsonValue* drv = Field(data, L"driver");
    const json::JsonValue* svc = Field(data, L"service");
    const json::JsonValue* srv = Field(data, L"server");
    const bool alive = drv && BoolOf(*drv, L"alive");
    const bool loaded = drv && BoolOf(*drv, L"sbiedll");

    if (opts.json) {
        EmitJsonOk(opts, data);
        return alive ? 0 : ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    }

    std::wstring out;
    if (loaded) {
        out += L"driver: " + StrOf(*drv, L"version") + L" / abi "
             + StrOf(*drv, L"abi_hex") + L" / "
             + (alive ? L"alive" : L"NOT running") + L"\n";
        std::wstring feats;
        if (const json::JsonValue* fs = Field(*drv, L"features");
            fs && fs->isArray()) {
            for (const json::JsonValue& f : fs->items()) {
                if (!feats.empty())
                    feats += L",";
                feats += f.isString() ? f.asString() : L"?";
            }
        }
        out += L"features: " + (feats.empty() ? L"(none)" : feats) + L"\n";
    } else {
        out += L"driver: unavailable\nfeatures: n/a\n";
    }
    if (svc && BoolOf(*svc, L"connected") && Field(*svc, L"version"))
        out += L"service: connected / " + StrOf(*svc, L"version") + L"\n";
    else
        out += L"service: not connected\n";
    if (srv && BoolOf(*srv, L"running")) {
        const json::JsonValue* idle = Field(*srv, L"idle_remaining_sec");
        out += L"server: running (pid " + std::to_wstring(IntOf(*srv, L"pid"))
             + L", uptime " + std::to_wstring(IntOf(*srv, L"uptime_sec"))
             + L"s, clients " + std::to_wstring(IntOf(*srv, L"clients"))
             + L", idle "
             + (idle && idle->isNull() ? L"no timeout"
                                       : std::to_wstring(IntOf(*srv, L"idle_remaining_sec")) + L"s")
             + L", log pump " + (BoolOf(*srv, L"log_pump") ? L"yes" : L"no")
             + L")\n";
    } else {
        out += L"server: not running\n";
    }
    out += L"boxes: " + std::to_wstring(IntOf(data, L"boxes")) + L"\n";
    util::PrintUtf8(util::WideToUtf8(out));
    return alive ? 0 : ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
}

} // namespace

// ---------------------------------------------------------------------------
// sbie version（04 §4.1；IPC 优先，降级直连；诊断命令——驱动缺席也退出 0）
// ---------------------------------------------------------------------------

int CmdVersion(const CommandContext& ctx)
{
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpVersion, json::JsonValue::Object(), true,
        RenderVersionIpc);
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    Overall o = Gather(ctx);

    if (ctx.opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        json::JsonValue cli = json::JsonValue::Object();
        cli.set(L"version", json::JsonValue(kCliVersionW));
        data.set(L"cli", cli);
        json::JsonValue drv = json::JsonValue::Object();
        drv.set(L"available", json::JsonValue(o.dllLoaded));
        if (o.dllLoaded) {
            drv.set(L"version", json::JsonValue(o.drvVersion.version));
            drv.set(L"abi", json::JsonValue((long long)o.drvVersion.abi));
            drv.set(L"abi_hex", json::JsonValue(AbiHex(o.drvVersion.abi)));
            drv.set(L"alive", json::JsonValue(o.driverAlive));
        }
        data.set(L"driver", drv);
        json::JsonValue svc = json::JsonValue::Object();
        svc.set(L"connected", json::JsonValue(o.svcConnected));
        if (o.svcConnected && !o.svcVersion.empty()) {
            svc.set(L"version", json::JsonValue(o.svcVersion));
            svc.set(L"abi", json::JsonValue((long long)o.svcAbi));
        }
        data.set(L"service", svc);
        EmitJsonOk(ctx.opts, data);
        return 0;
    }

    std::wstring out;
    out += L"sbie-cli " + std::wstring(kCliVersionW) + L"\n";
    if (o.dllLoaded)
        out += L"driver " + o.drvVersion.version + L" (abi " + AbiHex(o.drvVersion.abi)
             + (o.driverAlive ? L", alive" : L", driver not running") + L")\n";
    else
        out += L"driver unavailable (" + o.loadError + L")\n";
    if (o.svcConnected && !o.svcVersion.empty())
        out += L"svc " + o.svcVersion + L" (abi " + AbiHex(o.svcAbi) + L")\n";
    else
        out += L"svc not connected\n";
    if (o.dllLoaded && !drv::AbiMatches())
        Diag(L"warning: SbieDll ABI " + AbiHex(o.drvVersion.abi)
             + L" differs from expected " + AbiHex(kExpectedAbi)
             + L" (baseline 5.73.5)");
    util::PrintUtf8(util::WideToUtf8(out));
    return 0;
}

// ---------------------------------------------------------------------------
// sbie status（04 §4.1；IPC 优先，可降级直连；驱动缺席优雅降级报告，退出码 3）
// ---------------------------------------------------------------------------

int CmdStatus(const CommandContext& ctx)
{
    ipcroute::Result r = ipcroute::Invoke(
        ctx.opts, ipc::kOpStatus, json::JsonValue::Object(), true,
        RenderStatusIpc);
    if (r.verdict == ipcroute::Verdict::Handled)
        return r.exitCode;

    srvconn::NoteDegraded();
    Overall o = Gather(ctx);

    // 降级直连：server 行恒 not running（server 缺席才走到这里）
    if (ctx.opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        json::JsonValue drv = json::JsonValue::Object();
        drv.set(L"alive", json::JsonValue(o.driverAlive));
        drv.set(L"sbiedll", json::JsonValue(o.dllLoaded));
        if (o.dllLoaded) {
            drv.set(L"version", json::JsonValue(o.drvVersion.version));
            drv.set(L"abi", json::JsonValue((long long)o.drvVersion.abi));
            drv.set(L"abi_hex", json::JsonValue(AbiHex(o.drvVersion.abi)));
        }
        drv.set(L"feature_flags", json::JsonValue((long long)o.featureFlags));
        drv.set(L"features", FeatureNames(o.featureFlags));
        data.set(L"driver", drv);
        json::JsonValue svc = json::JsonValue::Object();
        svc.set(L"connected", json::JsonValue(o.svcConnected));
        if (o.svcConnected && !o.svcVersion.empty()) {
            svc.set(L"version", json::JsonValue(o.svcVersion));
            svc.set(L"abi", json::JsonValue((long long)o.svcAbi));
        }
        data.set(L"service", svc);
        json::JsonValue srv = json::JsonValue::Object();
        srv.set(L"running", json::JsonValue(false));
        data.set(L"server", srv);
        data.set(L"boxes", json::JsonValue((long long)o.boxCount));
        EmitJsonOk(ctx.opts, data);
        return o.driverAlive ? 0 : ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    }

    std::wstring out;
    if (o.dllLoaded) {
        out += L"driver: " + o.drvVersion.version + L" / abi " + AbiHex(o.drvVersion.abi)
             + L" / " + (o.driverAlive ? L"alive" : L"NOT running") + L"\n";
        out += L"features: ";
        std::wstring feats;
        unsigned long f = o.featureFlags;
        auto app = [&feats, f](unsigned long bit, const wchar_t* n) {
            if (f & bit) { if (!feats.empty()) feats += L","; feats += n; }
        };
        app(0x01, L"WFP"); app(0x02, L"ObCB"); app(0x10, L"SbieLogin");
        app(0x20, L"Win32kHook"); app(0x10000000, L"DynDataOk");
        app(0x40000000, L"NewArch");
        out += feats.empty() ? L"(none)" : feats;
        out += L"\n";
    } else {
        out += L"driver: unavailable (" + o.loadError + L")\n";
        out += L"features: n/a\n";
    }
    if (o.svcConnected && !o.svcVersion.empty())
        out += L"service: connected / " + o.svcVersion + L"\n";
    else
        out += L"service: not connected\n";
    out += L"server: not running\n";
    out += L"boxes: " + std::to_wstring(o.boxCount) + L"\n";
    util::PrintUtf8(util::WideToUtf8(out));
    return o.driverAlive ? 0 : ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// sbie box list（04 §4.3；--all 含 disabled；IPC 优先，可降级直连）
// ---------------------------------------------------------------------------

int CmdBoxList(const CommandContext& ctx)
{
    // --all：兼容旗标（旧行为"含 disabled"）——验收回传修正（04 §13）：
    // 缺省即枚举全部 box、不按 Enabled 过滤（QSbieAPI GetAllBoxes 语义，
    // disabled 沙箱仍列出、ENABLED 列显示 no）。旧行为缺省过滤 disabled，
    // 导致 `box disable` 后沙箱从默认列表"消失"，被误读为写丢失。
    bool all = false;
    for (auto& a : ctx.args)
        if (a == L"--all")
            all = true;
    (void)all;

    json::JsonValue params = json::JsonValue::Object();
    ipcroute::PSet(&params, L"all", all);
    ipcroute::Result ipcR = ipcroute::Invoke(
        ctx.opts, ipc::kOpBoxList, params, true,
        [](const GlobalOptions& o, const json::JsonValue& data) {
            return ipcroute::RenderRows(
                o,
                { { L"NAME", L"name" },
                  { L"ENABLED", L"enabled" },
                  { L"ACTIVE_PROCS", L"active_procs", true },
                  { L"FILE_ROOT", L"file_root" } },
                data, L"no boxes");
        });
    if (ipcR.verdict == ipcroute::Verdict::Handled)
        return ipcR.exitCode;

    srvconn::NoteDegraded();

    svc::SvcClient& svc = svc::SvcClient::Instance();
    model::BoxRepository repo(nullptr, svc);
    std::vector<model::BoxInfo> boxes = repo.EnumBoxes(false);

    util::TablePrinter t;
    t.AddColumn(L"NAME");
    t.AddColumn(L"ENABLED");
    t.AddColumn(L"ACTIVE_PROCS", true);
    t.AddColumn(L"FILE_ROOT");
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& b : boxes) {
        ULONG procs = 0;
        std::vector<ULONG> pids;
        if (drv::EnumBoxProcesses(b.name, false, &pids) == SbieStatus::OK)
            procs = (ULONG)pids.size();
        t.AddRow({ b.name,
                   b.enabled ? L"yes" : L"no",
                   std::to_wstring(procs),
                   b.fileRoot });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"name", json::JsonValue(b.name));
        r.set(L"enabled", json::JsonValue(b.enabled));
        r.set(L"active_procs", json::JsonValue((long long)procs));
        r.set(L"file_root", json::JsonValue(b.fileRoot));
        r.set(L"reg_root", json::JsonValue(b.regRoot));
        r.set(L"ipc_root", json::JsonValue(b.ipcRoot));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"no boxes");
    return 0;
}

// ---------------------------------------------------------------------------
// sbie proc list（04 §4.4；--box <name> / --all-sessions；IPC 优先，可降级直连）
// ---------------------------------------------------------------------------

int CmdProcList(const CommandContext& ctx)
{
    std::wstring box;
    bool allSessions = false;
    for (size_t i = 0; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--box" && i + 1 < ctx.args.size())
            box = ctx.args[++i];
        else if (ctx.args[i] == L"--all-sessions")
            allSessions = true;
    }

    json::JsonValue params = json::JsonValue::Object();
    if (!box.empty())
        ipcroute::PSet(&params, L"box", box);
    ipcroute::PSet(&params, L"all_sessions", allSessions);
    ipcroute::Result ipcR = ipcroute::Invoke(
        ctx.opts, ipc::kOpProcList, params, true,
        [](const GlobalOptions& o, const json::JsonValue& data) {
            // 表格 IMAGE 列显示基名（--json 给全路径），与直连一致
            return ipcroute::RenderRows(
                o,
                { { L"PID", L"pid", true },
                  { L"BOX", L"box" },
                  { L"IMAGE", L"image" },
                  { L"SESSION", L"session", true },
                  { L"STARTED", L"started" } },
                data, L"no processes",
                [&o](const json::JsonValue& row) {
                    std::vector<std::wstring> line;
                    auto s = [&row](const wchar_t* k) {
                        return ipcroute::CellOf(
                            row.isObject() ? row.find(k) : nullptr);
                    };
                    std::wstring image = s(L"image");
                    if (!o.json && !o.quiet && !image.empty()) {
                        const size_t cut = image.rfind(L'\\');
                        if (cut != std::wstring::npos)
                            image = image.substr(cut + 1);
                    }
                    line = { s(L"pid"), s(L"box"), image, s(L"session"),
                             s(L"started") };
                    return line;
                });
        });
    if (ipcR.verdict == ipcroute::Verdict::Handled)
        return ipcR.exitCode;

    srvconn::NoteDegraded();

    svc::SvcClient& svc = svc::SvcClient::Instance();
    model::ProcessRepository procs(nullptr, svc);
    std::vector<model::ProcEntry> list = procs.Enum(allSessions, box);

    util::TablePrinter t;
    t.AddColumn(L"PID", true);
    t.AddColumn(L"BOX");
    t.AddColumn(L"IMAGE");
    t.AddColumn(L"SESSION", true);
    t.AddColumn(L"STARTED");
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& p : list) {
        std::wstring image = p.image;
        if (!image.empty() && image.rfind(L'\\') != std::wstring::npos
            && !ctx.opts.quiet && !ctx.opts.json) {
            // 表格默认显示基名（对齐资源管理器观感）；--json 给全路径
            image = image.substr(image.rfind(L'\\') + 1);
        }
        t.AddRow({ std::to_wstring(p.pid), p.box, image,
                   std::to_wstring(p.sessionId),
                   FormatCreateTime(p.createTime) });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"pid", json::JsonValue((long long)p.pid));
        r.set(L"box", json::JsonValue(p.box));
        r.set(L"image", json::JsonValue(p.image));
        r.set(L"session", json::JsonValue((long long)p.sessionId));
        r.set(L"started", json::JsonValue(FormatCreateTime(p.createTime)));
        r.set(L"flags", json::JsonValue((long long)p.flags));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"no processes");
    return 0;
}

// ---------------------------------------------------------------------------
// 桩
// ---------------------------------------------------------------------------

int CmdNotImplemented(const CommandContext& ctx)
{
    return EmitError(ctx.opts, SbieStatus::ERR_NOT_IMPLEMENTED,
                     L"command not implemented in this build (planned: "
                     L"server mode / SbieSvc write path wave)");
}

int CmdServerUnavailable(const CommandContext& ctx)
{
    return EmitError(ctx.opts, SbieStatus::SERVER_UNAVAILABLE,
                     L"this command requires the sbie-cli server, which is not "
                     L"implemented in this build");
}

// ---------------------------------------------------------------------------
// 注册表（框架：新命令只加注册项）
// ---------------------------------------------------------------------------

std::map<std::string, std::map<std::string, CommandHandler>>& Commands()
{
    static std::map<std::string, std::map<std::string, CommandHandler>> reg;
    return reg;
}

void RegisterCommands()
{
    auto& reg = Commands();
    auto sub = [&](const char* g, const char* s, CommandHandler h) {
        reg[g][s] = h;
    };
    // 全局（04 §4.1；IPC 优先 + 降级直连）
    sub("status", "", CmdStatus);
    sub("version", "", CmdVersion);
    // server（04 §4.2）→ server_cmd.cpp（RegisterServerCommands）
    // box（04 §4.3）——list 在此（IPC 优先 + 降级）；create/info/get/set/
    // list-setting/rename/delete/enable/disable/clean/size/snapshot/recover
    // → box_*.cpp
    sub("box", "list", CmdBoxList);
    // proc（04 §4.4）——list 在此；info/start/kill/kill-all/suspend/resume
    // → proc_cmd.cpp
    sub("proc", "list", CmdProcList);
    // cfg（04 §4.5）——get/path/reload/list-setting → cfg_read/cfg_reload.cpp；
    // set/unset/lock/unlock → cfg_set.cpp
    // template（04 §4.6）→ template_cmd.cpp；log（04 §4.7）→ log_cmd.cpp

    // ---- 第二波命令集接线（后调用者胜，覆盖上方同键注册项） ----------------
    RegisterServerCommands();        // server start/stop/status
    RegisterCfgTemplateCommands();   // cfg get/path/reload + template 全部 + log
    RegisterBoxCreateCommands();     // box create(--type 预设)/types
    RegisterBoxManageCommands();     // box info/get/set/list-setting/rename/delete
    RegisterBoxSnapshotCommands();   // box snapshot list/take/remove/select/set-info
    RegisterBoxRecoverCommands();    // box recover list/copy/add（P0-12；
                                      //   copy 波 B 增 --move/--no-check）
    RegisterBoxTransferCommands();   // box copy/export/import（07-P1-2/4）
    RegisterProcCommands();          // proc info/start/kill/kill-all/suspend/resume
    RegisterCfgSetCommands();        // cfg set/unset/lock/unlock
    // ---- P1 清尾波次（docs/04 §15；06 缺口表 P1-1/P1-2）----
    RegisterForceCommands();         // force on/off/status（直驱动 + server op）
    RegisterMaintCommands();         // maint status/start/stop（机器级，无 op）
}

} // namespace sbie::cli
