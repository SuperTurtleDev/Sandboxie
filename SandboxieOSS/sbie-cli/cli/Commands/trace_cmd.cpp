// Sandboxie-OSS — sbie-cli/cli/Commands/trace_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie trace watch|dump（波 D1，docs/04 §20；07 深度差距 06-P2-1 收口）。
//   * watch：实时流——server 在场时经专用连接订阅 trace.watch，消费 server
//     TracePump 推送的 {"op":"trace.event"} 帧（msgid=0，PeekNamedPipe 轮询
//     保证 Ctrl+C 可退）；--no-server / server 缺席降级为本进程
//     MonitorDirectSession 自拉（开启监控→轮询 GET2→退出关回）。
//   * dump：[--last N] 取 server 环形缓冲近期条目（服务端过滤 box/type/pid）；
//     直连降级 = 当次拉取驱动环残留（刚开启时为空——监控环只在开启期间
//     积累，直连收集属 watch 职责）。
//   * 过滤参数 watch/dump 一致：--box <名> / --type <缩写> / --pid <pid>。
//     watch 过滤在 client 侧做（推送帧自带全字段）；dump 过滤在 server 侧。
//   * 行格式：[hh:mm:ss] <类型缩写>[/.子类型][ (U)] <status> <pid> <box> <值>
//     （Model::FormatMonitorLine，对齐 core trace 语义）。--json：NDJSON
//     （watch 每行一条对象；dump 为 {count,entries} 包络）。
//   * 坑：API_MONITOR_GET2 是排空式读取（驱动端逐条 pop）——server 泵与
//     直连 watch 不可同时读同会话环（条目会被瓜分）。--no-server 用于
//     server 停止时的直连形态，见 docs/04 §20。
// Ctrl+C 退出码 0（log watch 先例）。

#include "../Commands.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "CfgTemplateCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/DriverApi/DriverApi.h"
#include "../../../SbieCore/Model/Monitor.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

// vendor：MONITOR_* 常量
#include "api_defs.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <string>
#include <vector>

namespace sbie::cli {

namespace {

volatile LONG g_traceStop = 0;

BOOL WINAPI TraceCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT
        || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_traceStop, 1);
        return TRUE;
    }
    return FALSE;
}

// ---- 过滤器与 --type 解析 --------------------------------------------------

struct TraceFilter {
    std::wstring box;    // 空 = 不过滤
    ULONG pid = 0;       // 0 = 不过滤
    ULONG typeCode = 0;  // MONITOR_*；0 = 不过滤（按 MONITOR_TYPE_MASK 匹配）

    bool Matches(ULONG type, ULONG entryPid, const std::wstring& entryBox) const
    {
        if (typeCode && (type & MONITOR_TYPE_MASK) != typeCode)
            return false;
        if (pid && entryPid != pid)
            return false;
        if (!box.empty() && entryBox != box)
            return false;   // 未知 box（"-"）同样不匹配显式过滤
        return true;
    }
};

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

// --type 取值表（缩写对齐 Model::MonitorTypeName；别名收编常见旧称）
bool ParseTraceType(const std::wstring& in, ULONG* out)
{
    static const struct { const wchar_t* name; ULONG code; } kTypes[] = {
        { L"apicall", MONITOR_APICALL }, { L"api", MONITOR_APICALL },
        { L"syscall", MONITOR_SYSCALL },
        { L"pipe", MONITOR_PIPE },
        { L"ipc", MONITOR_IPC },
        { L"rpc", MONITOR_RPC },
        { L"winclass", MONITOR_WINCLASS }, { L"class", MONITOR_WINCLASS },
        { L"drive", MONITOR_DRIVE },
        { L"comclass", MONITOR_COMCLASS }, { L"com", MONITOR_COMCLASS },
        { L"rtclass", MONITOR_RTCLASS },
        { L"ignore", MONITOR_IGNORE },
        { L"image", MONITOR_IMAGE },
        { L"file", MONITOR_FILE },
        { L"key", MONITOR_KEY }, { L"registry", MONITOR_KEY },
        { L"socket", MONITOR_NETFW }, { L"netfw", MONITOR_NETFW },
        { L"dns", MONITOR_DNS },
        { L"scm", MONITOR_SCM },
        { L"hook", MONITOR_HOOK },
        { L"debug", MONITOR_OTHER }, { L"other", MONITOR_OTHER },
    };
    const std::wstring n = Lower(in);
    for (const auto& t : kTypes) {
        if (n == t.name) {
            *out = t.code;
            return true;
        }
    }
    return false;
}

// 公共参数解析（watch/dump 同一集）。返回 0 继续；非 0 = 已输出的退出码。
int ParseTraceArgs(const std::vector<std::wstring>& rest,
                   const GlobalOptions& o, TraceFilter* f, ULONG* last,
                   bool dumpMode)
{
    for (size_t i = 0; i < rest.size(); ++i) {
        const std::wstring& a = rest[i];
        if (a == L"--box" && i + 1 < rest.size()) {
            f->box = rest[++i];
            if (f->box.empty())
                return EmitError(o, SbieStatus::USAGE, L"invalid --box");
        } else if (a == L"--pid" && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            f->pid = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0' || f->pid == 0)
                return EmitError(o, SbieStatus::USAGE, L"invalid --pid");
        } else if (a == L"--type" && i + 1 < rest.size()) {
            if (!ParseTraceType(rest[++i], &f->typeCode))
                return EmitError(o, SbieStatus::USAGE,
                                 L"unknown --type '" + rest[i]
                                     + L"' (apicall syscall pipe ipc rpc"
                                       L" winclass drive comclass rtclass"
                                       L" ignore image file key socket dns"
                                       L" scm hook debug)");
        } else if (dumpMode && a == L"--last" && i + 1 < rest.size()) {
            wchar_t* end = nullptr;
            *last = wcstoul(rest[++i].c_str(), &end, 10);
            if (!end || *end != L'\0' || *last == 0)
                return EmitError(o, SbieStatus::USAGE, L"invalid --last value");
        } else if (dumpMode && a == L"--raw") {
            // 接受但忽略（值列本就含原始路径/消息）
        } else if (!dumpMode && a == L"--follow") {
            // 默认即跟随；接受显式旗标
        } else {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        }
    }
    return 0;
}

// ---- 输出 ------------------------------------------------------------------

std::wstring NowHms()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 直连条目输出（Model 全字段渲染）
void EmitDirectEntry(const GlobalOptions& o, const TraceFilter& f,
                     const model::MonitorEntry& e, const std::wstring& box)
{
    if (!f.Matches(e.type, e.pid, box))
        return;
    const std::wstring time = model::MonitorTimeText(e.timestamp);
    if (o.json) {
        // NDJSON：与 server trace.event data 字段同构
        json::JsonValue j = json::JsonValue::Object();
        j.set(L"type", json::JsonValue(model::MonitorTypeName(e.type)));
        j.set(L"type_code",
              json::JsonValue((long long)(e.type & MONITOR_TYPE_MASK)));
        j.set(L"status", json::JsonValue(model::MonitorStatusText(e.type)));
        j.set(L"pid", json::JsonValue((long long)e.pid));
        j.set(L"tid", json::JsonValue((long long)e.tid));
        j.set(L"box", json::JsonValue(box));
        j.set(L"time", json::JsonValue(time));
        j.set(L"timestamp", json::JsonValue((long long)e.timestamp));
        j.set(L"name", json::JsonValue(e.strings.size() > 0
                                           ? e.strings[0]
                                           : std::wstring()));
        j.set(L"message", json::JsonValue(e.strings.size() > 1
                                              ? e.strings[1]
                                              : std::wstring()));
        util::PrintLineUtf8(json::SerializeUtf8(j));
    } else {
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + (time == L"-" ? NowHms() : time) + L"] "
            + model::FormatMonitorLine(e, box)));
    }
    fflush(stdout);   // 流式命令：重定向下逐行可见
}

// ---- server 推送事件（trace.event data：见 TracePump EntryJson） -------------

struct ServerTraceEvent {
    std::wstring type, status, box, time, name, message;
    ULONG typeCode = 0, pid = 0, tid = 0;
    bool Parse(const json::JsonValue& d)
    {
        if (!d.isObject())
            return false;
        const json::JsonValue* v = d.find(L"type");
        if (v && v->isString())
            type = v->asString();
        v = d.find(L"status");
        if (v && v->isString())
            status = v->asString();
        v = d.find(L"box");
        if (v && v->isString())
            box = v->asString();
        v = d.find(L"time");
        if (v && v->isString())
            time = v->asString();
        v = d.find(L"name");
        if (v && v->isString())
            name = v->asString();
        v = d.find(L"message");
        if (v && v->isString())
            message = v->asString();
        v = d.find(L"type_code");
        if (v && v->isInt())
            typeCode = (ULONG)v->asInt();
        v = d.find(L"pid");
        if (v && v->isInt())
            pid = (ULONG)v->asInt();
        v = d.find(L"tid");
        if (v && v->isInt())
            tid = (ULONG)v->asInt();
        return true;
    }
};

void EmitServerTraceEvent(const GlobalOptions& o, const TraceFilter& f,
                          const ServerTraceEvent& e)
{
    if (!f.Matches(e.typeCode, e.pid, e.box))
        return;
    const std::wstring time = e.time.empty() ? NowHms() : e.time;
    if (o.json) {
        json::JsonValue j = json::JsonValue::Object();
        j.set(L"type", json::JsonValue(e.type));
        j.set(L"type_code", json::JsonValue((long long)e.typeCode));
        j.set(L"status", json::JsonValue(e.status));
        j.set(L"pid", json::JsonValue((long long)e.pid));
        j.set(L"tid", json::JsonValue((long long)e.tid));
        j.set(L"box", json::JsonValue(e.box));
        j.set(L"time", json::JsonValue(time));
        j.set(L"name", json::JsonValue(e.name));
        j.set(L"message", json::JsonValue(e.message));
        util::PrintLineUtf8(json::SerializeUtf8(j));
    } else {
        std::wstring value = e.name;
        if (!e.message.empty()) {
            if (!value.empty())
                value += L' ';
            value += e.message;
        }
        if (value.empty())
            value = L"(empty)";
        const std::wstring box = e.box.empty() ? L"-" : e.box;
        const std::wstring st = e.status.empty() ? L"-" : e.status;
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + time + L"] " + e.type + L" " + st + L" "
            + std::to_wstring(e.pid) + L" " + box + L" " + value));
    }
    fflush(stdout);
}

// ---- watch：server 订阅路径（log watch WatchViaServer 同款框架） ------------
// 返回 0 = 已完成（退出 0）；-1 = 订阅未成立，降级直连；>0 = 退出码。
int WatchTraceViaServer(const GlobalOptions& o, const TraceFilter& f)
{
    if (!srvconn::HasServer()) {
        if (o.showTransport)
            Diag(L"transport: direct (server not connected)");
        return -1;
    }

    ipc::PipeClient pipe;
    if (!pipe.Open(1000)) {
        if (o.showTransport)
            Diag(L"transport: direct (server probe failed)");
        return -1;
    }
    std::string reply;
    if (!pipe.RoundTrip(ipc::kOpTraceWatch, "{}", &reply)) {
        if (o.showTransport)
            Diag(L"transport: direct (subscribe failed)");
        return -1;
    }
    json::JsonValue env;
    SbieStatus jerr = SbieStatus::OK;
    if (!json::Parse(reply, &env, &jerr) || !env.isObject()) {
        if (o.showTransport)
            Diag(L"transport: direct (malformed subscribe reply)");
        return -1;
    }
    const json::JsonValue* okf = env.find(L"ok");
    if (!okf || !okf->asBool()) {
        // 订阅被拒（trace pump 未激活，如驱动缺席）——交回直连路径给精确诊断
        if (o.showTransport)
            Diag(L"transport: direct (subscription rejected)");
        return -1;
    }
    if (o.showTransport)
        Diag(L"transport: ipc (trace subscription)");
    if (!o.json && !o.quiet)
        Diag(L"watching sandbox trace events (Ctrl+C to stop)");
    SetConsoleCtrlHandler(TraceCtrlHandler, TRUE);

    bool connectionLost = false;
    while (InterlockedCompareExchange(&g_traceStop, 0, 0) == 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe.Handle(), nullptr, 0, nullptr, &avail,
                           nullptr)) {
            connectionLost = true;   // server 死/管道断
            break;
        }
        if (avail == 0) {
            Sleep(100);
            continue;
        }
        ipc::FrameHeader hdr;
        std::vector<uint8_t> payload;
        if (!ipc::ReadFrame(pipe.Handle(), &hdr, &payload)) {
            connectionLost = true;
            break;
        }
        if (hdr.msgid != 0)
            continue;   // 非 0 = 对某请求的应答（本连接不再发请求，忽略）
        json::JsonValue push;
        if (!json::Parse(std::string((const char*)payload.data(),
                                     payload.size()),
                         &push, &jerr)
            || !push.isObject())
            continue;
        const json::JsonValue* opf = push.find(L"op");
        if (!opf || !opf->isString() || opf->asString() != L"trace.event")
            continue;
        ServerTraceEvent e;
        if (const json::JsonValue* d = push.find(L"data"); d && e.Parse(*d))
            EmitServerTraceEvent(o, f, e);
    }

    SetConsoleCtrlHandler(TraceCtrlHandler, FALSE);
    if (connectionLost) {
        return EmitError(o, SbieStatus::SERVER_UNAVAILABLE,
                         L"server connection lost");
    }
    return 0;   // Ctrl+C = 0
}

} // namespace

// ---------------------------------------------------------------------------
// sbie trace watch（04 §20；--box/--type/--pid 过滤，Ctrl+C 退出 0）
// ---------------------------------------------------------------------------

int CmdTraceWatch(const CommandContext& ctx)
{
    GlobalOptions o;
    const std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    TraceFilter f;
    ULONG last = 0;
    int rc = ParseTraceArgs(rest, o, &f, &last, false);
    if (rc != 0)
        return rc;

    // IPC 优先：server TracePump 订阅推送；缺席/被拒降级直连自拉
    {
        int irc = WatchTraceViaServer(o, f);
        if (irc >= 0)
            return irc;
    }

    srvconn::NoteDegraded();
    if (!drv::LoadSbieDll(o.sbieDllPath))
        return EmitError(o, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!drv::DriverAlive())
        return EmitError(o, SbieStatus::DRIVER_UNAVAILABLE, L"driver not running");
    if (o.showTransport)
        Diag(L"transport: direct (self pump via API_MONITOR_GET2)");

    model::MonitorDirectSession session;
    SbieStatus st = session.Start();
    if (st != SbieStatus::OK)
        return EmitError(o, st, L"monitor control failed (driver rejected"
                                 L" API_MONITOR_CONTROL)");

    model::MonitorPidResolver resolver;
    SetConsoleCtrlHandler(TraceCtrlHandler, TRUE);
    if (!o.json && !o.quiet)
        Diag(L"watching sandbox trace events, direct mode (Ctrl+C to stop)");

    std::vector<model::MonitorEntry> entries;
    while (InterlockedCompareExchange(&g_traceStop, 0, 0) == 0) {
        st = session.Poll(&entries);
        if (st != SbieStatus::OK) {
            // 监控被并发关闭等：重建（重启开关后继续）
            session.Stop();
            st = session.Start();
            if (st != SbieStatus::OK)
                break;
            continue;
        }
        for (const auto& e : entries) {
            std::wstring box;
            resolver.Lookup(e.pid, &box);
            EmitDirectEntry(o, f, e, box);
        }
        entries.clear();
        Sleep(200);
    }

    SetConsoleCtrlHandler(TraceCtrlHandler, FALSE);
    session.Stop();
    return 0;   // Ctrl+C = 0
}

// ---------------------------------------------------------------------------
// sbie trace dump（04 §20；[--last N=100] + 过滤，取环形缓冲近期条目）
// ---------------------------------------------------------------------------

int CmdTraceDump(const CommandContext& ctx)
{
    GlobalOptions o;
    const std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    TraceFilter f;
    ULONG last = 100;
    int rc = ParseTraceArgs(rest, o, &f, &last, true);
    if (rc != 0)
        return rc;

    // IPC 优先（trace.dump：server 环形缓冲，服务端过滤）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"last", (long long)last);
        if (!f.box.empty())
            ipcroute::PSet(&params, L"box", f.box);
        if (f.typeCode)
            ipcroute::PSet(&params, L"type_code", (long long)f.typeCode);
        if (f.pid)
            ipcroute::PSet(&params, L"pid", (long long)f.pid);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpTraceDump, params, true,
            [&f](const GlobalOptions& op, const json::JsonValue& data) {
                std::vector<ServerTraceEvent> events;
                if (data.isArray()) {
                    for (const json::JsonValue& e : data.items()) {
                        ServerTraceEvent ev;
                        if (ev.Parse(e))
                            events.push_back(std::move(ev));
                    }
                }
                if (events.empty())
                    return EmitError(op, SbieStatus::NOT_FOUND,
                                     L"trace buffer empty (no matching"
                                     L" entries)");
                if (op.json) {
                    json::JsonValue arr = json::JsonValue::Array();
                    for (const ServerTraceEvent& e : events) {
                        json::JsonValue je = json::JsonValue::Object();
                        je.set(L"type", json::JsonValue(e.type));
                        je.set(L"type_code",
                               json::JsonValue((long long)e.typeCode));
                        je.set(L"status", json::JsonValue(e.status));
                        je.set(L"pid", json::JsonValue((long long)e.pid));
                        je.set(L"tid", json::JsonValue((long long)e.tid));
                        je.set(L"box", json::JsonValue(e.box));
                        je.set(L"time", json::JsonValue(e.time));
                        je.set(L"name", json::JsonValue(e.name));
                        je.set(L"message", json::JsonValue(e.message));
                        arr.pushBack(std::move(je));
                    }
                    json::JsonValue d = json::JsonValue::Object();
                    d.set(L"count",
                          json::JsonValue((long long)events.size()));
                    d.set(L"entries", std::move(arr));
                    EmitJsonOk(op, d);
                    return 0;
                }
                for (const ServerTraceEvent& e : events)
                    EmitServerTraceEvent(op, f, e);
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
    if (o.showTransport)
        Diag(L"transport: direct (one-shot API_MONITOR_GET2 drain)");

    // 直连：监控环只在开启期间积累——开启（他人已开不动）→ 当次拉取残留 →
    // 关回。刚开启时环为空属正常语义（收集属 watch 职责）
    model::MonitorDirectSession session;
    SbieStatus st = session.Start();
    if (st != SbieStatus::OK)
        return EmitError(o, st, L"monitor control failed (driver rejected"
                                 L" API_MONITOR_CONTROL)");

    model::MonitorPidResolver resolver;
    std::vector<model::MonitorEntry> entries;
    st = session.Poll(&entries);
    session.Stop();
    if (st != SbieStatus::OK)
        return EmitError(o, st, L"API_MONITOR_GET2 failed");

    std::deque<const model::MonitorEntry*> ring;
    for (const auto& e : entries) {
        std::wstring box;
        resolver.Lookup(e.pid, &box);
        if (f.Matches(e.type, e.pid, box))
            ring.push_back(&e);
    }
    while (ring.size() > last)
        ring.pop_front();
    if (ring.empty())
        return EmitError(o, SbieStatus::NOT_FOUND,
                         L"trace buffer empty (monitoring starts now;"
                         L" use trace watch to collect)");

    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const model::MonitorEntry* e : ring) {
            std::wstring box;
            resolver.Lookup(e->pid, &box);
            json::JsonValue je = json::JsonValue::Object();
            je.set(L"type", json::JsonValue(model::MonitorTypeName(e->type)));
            je.set(L"type_code",
                   json::JsonValue((long long)(e->type & MONITOR_TYPE_MASK)));
            je.set(L"status",
                   json::JsonValue(model::MonitorStatusText(e->type)));
            je.set(L"pid", json::JsonValue((long long)e->pid));
            je.set(L"tid", json::JsonValue((long long)e->tid));
            je.set(L"box", json::JsonValue(box));
            je.set(L"time",
                   json::JsonValue(model::MonitorTimeText(e->timestamp)));
            je.set(L"timestamp", json::JsonValue((long long)e->timestamp));
            je.set(L"name", json::JsonValue(e->strings.size() > 0
                                                ? e->strings[0]
                                                : std::wstring()));
            je.set(L"message", json::JsonValue(e->strings.size() > 1
                                                   ? e->strings[1]
                                                   : std::wstring()));
            arr.pushBack(std::move(je));
        }
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"count", json::JsonValue((long long)ring.size()));
        d.set(L"entries", std::move(arr));
        EmitJsonOk(o, d);
        return 0;
    }
    for (const model::MonitorEntry* e : ring) {
        std::wstring box;
        resolver.Lookup(e->pid, &box);
        EmitDirectEntry(o, TraceFilter() /*已过滤*/, *e, box);
    }
    return 0;
}

} // namespace sbie::cli
