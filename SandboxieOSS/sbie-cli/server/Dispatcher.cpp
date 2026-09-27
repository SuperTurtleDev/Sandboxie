// Sandboxie-OSS — sbie-cli/server/Dispatcher.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// op → SbieCore 转发 → JSON data（M2 server 波次）。
// 行集类 op 的 data = 对象数组（字段名 = 04 §7.2 snake_case，与 client
// 直连模式的 --json 输出一致——接线 agent 按 data 数组渲染表格）。

#include "Dispatcher.h"
#include "Guardian.h"
#include "LogPump.h"
#include "ServerState.h"
#include "SvcProxy.h"
#include "TracePump.h"
#include "../ipcc/SbieIpc.h"
#include "../ipcc/TmplHide.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/Boxes.h"
#include "../../SbieCore/Model/BoxTransfer.h"
#include "../../SbieCore/Model/BoxUsage.h"
#include "../../SbieCore/Model/ConfigStore.h"
#include "../../SbieCore/Model/DiskImage.h"
#include "../../SbieCore/Model/Monitor.h"
#include "../../SbieCore/Model/Processes.h"
#include "../../SbieCore/Model/Recovery.h"
#include "../../SbieCore/Model/Snapshots.h"
#include "../../SbieCore/Model/Templates.h"
#include "../../SbieCore/Model/UsbSandbox.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/PathMapper.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"
#include "../../SbieCore/Util/Version.h"

// vendor：CONF_GET_NO_* 标志（list-setting 的 --all 反向语义枚举）
#include "api_flags.h"

#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sbie::server {

std::map<std::string, Handler>& Registry()
{
    static std::map<std::string, Handler> reg;
    return reg;
}

namespace {

// ---- 参数提取小工具 ----

bool GetStr(const json::JsonValue& params, const wchar_t* key, std::wstring* out)
{
    const json::JsonValue* v = params.find(key);
    if (!v || !v->isString())
        return false;
    *out = v->asString();
    return true;
}

bool GetBool(const json::JsonValue& params, const wchar_t* key, bool def)
{
    const json::JsonValue* v = params.find(key);
    return v ? v->asBool() : def;
}

long long GetInt(const json::JsonValue& params, const wchar_t* key,
                 long long def)
{
    const json::JsonValue* v = params.find(key);
    return (v && v->isInt()) ? v->asInt() : def;
}

// 100ns/1601 纪元（PsGetProcessCreateTimeQuadPart）→ 本地 "YYYY-MM-DD HH:MM:SS"
//（与 client Commands.cpp FormatCreateTime 同构——02 §7.6）
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

// driver + service 状态采集（status/version 共用）。SbieSvc LPC 全部经
// SvcProxy 专职线程（03 §1）。
struct DrvSvc {
    bool dllLoaded = false;
    bool driverAlive = false;
    drv::VersionInfo drvVersion;
    unsigned long featureFlags = 0;
    bool svcConnected = false;
    std::wstring svcVersion;
    ULONG svcAbi = 0;
};

DrvSvc GatherDrvSvc()
{
    DrvSvc o;
    o.dllLoaded = drv::Loaded() || drv::LoadSbieDll();
    if (!o.dllLoaded)
        return o;
    o.drvVersion = drv::GetVersion();
    o.driverAlive = drv::DriverAlive();
    if (o.driverAlive)
        drv::QueryFeatureFlags(&o.featureFlags);
    SvcCall([&o] {
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
        return SbieStatus::OK;
    });
    return o;
}

// ---- handlers（每个 op 独立函数；只加注册项即可扩命令） ----

OpResult HStatus(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    DrvSvc o = GatherDrvSvc();

    size_t boxCount = 0;
    if (o.dllLoaded) {
        std::vector<std::wstring> boxes;
        if (drv::EnumBoxes(&boxes, false) == SbieStatus::OK)
            boxCount = boxes.size();
    }

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
    srv.set(L"running", json::JsonValue(true));
    srv.set(L"pid", json::JsonValue((long long)ServerState::Get().Pid()));
    srv.set(L"uptime_sec",
            json::JsonValue((long long)ServerState::Get().UptimeSec()));
    srv.set(L"clients",
            json::JsonValue((long long)ServerState::Get().ActiveClients()));
    ULONGLONG idle = ServerState::Get().IdleRemainingMs();
    if (idle == ~0ULL)
        srv.set(L"idle_remaining_sec", json::JsonValue()); // null = 无限
    else
        srv.set(L"idle_remaining_sec",
                json::JsonValue((long long)(idle / 1000)));
    srv.set(L"log_pump", json::JsonValue(ServerState::Get().LogPumpActive()));
    srv.set(L"trace_pump", json::JsonValue(ServerState::Get().TracePumpActive()));
    // 空箱守护监视器（波次 A，07-P0-2）：运行状态 + 已处理空箱事件计数
    //（client `server status` 的 GUARDIANS 列）
    srv.set(L"guardians", json::JsonValue(GuardiansActive()));
    srv.set(L"guardian_fires",
            json::JsonValue((long long)GuardiansFired()));
    data.set(L"server", srv);

    data.set(L"boxes", json::JsonValue((long long)boxCount));
    return OpResult::Succeed(std::move(data));
}

OpResult HVersion(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    DrvSvc o = GatherDrvSvc();
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
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxList(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    // "all" 仅为 wire 兼容保留（client 仍发送）——枚举恒不按 Enabled 过滤
    //（验收回传修正，04 §13：QSbieAPI GetAllBoxes 语义，disabled 沙箱仍
    // 列出、ENABLED 列显示 no；旧行为缺省过滤致 disable 后"列表消失"误读）
    (void)GetBool(params, L"all", false);
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    std::vector<model::BoxInfo> boxes = repo.EnumBoxes(false);
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& b : boxes) {
        // 活动进程数按需重查（BoxInfo.hasProcesses 只有布尔语义——与
        // client 直连 CmdBoxList 的 EnumBoxProcesses 计数对齐）
        ULONG procs = 0;
        std::vector<ULONG> pids;
        if (drv::EnumBoxProcesses(b.name, false, &pids) == SbieStatus::OK)
            procs = (ULONG)pids.size();
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"name", json::JsonValue(b.name));
        r.set(L"enabled", json::JsonValue(b.enabled));
        r.set(L"active_procs", json::JsonValue((long long)procs));
        r.set(L"file_root", json::JsonValue(b.fileRoot));
        r.set(L"reg_root", json::JsonValue(b.regRoot));
        r.set(L"ipc_root", json::JsonValue(b.ipcRoot));
        rows.pushBack(std::move(r));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HBoxInfo(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    SbieStatus st = repo.GetInfo(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND, L"box not found: " + name);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"name", json::JsonValue(bi.name));
    data.set(L"enabled", json::JsonValue(bi.enabled));
    data.set(L"exists", json::JsonValue(bi.exists));
    data.set(L"file_root", json::JsonValue(bi.fileRoot));
    data.set(L"reg_root", json::JsonValue(bi.regRoot));
    data.set(L"ipc_root", json::JsonValue(bi.ipcRoot));
    data.set(L"has_processes", json::JsonValue(bi.hasProcesses));
    return OpResult::Succeed(std::move(data));
}

// box.get / cfg.get 共用：给定 section+setting，收集全部 index 值。
// noexpand 透传（04 §11 遗留 3 消除）：缺省 true = 恒不展开（--raw 语义，
// 与本 op 既有行为一致）；client 后续波次带 raw/noexpand 参数即区分
// %env% 展开（QueryConfText 的 CONF_GET_NO_EXPAND 反向）。
OpResult ConfValues(const std::wstring& section, const std::wstring& setting,
                    long long index, bool noExpand)
{
    model::ConfigStore cfg;
    json::JsonValue rows = json::JsonValue::Array();
    if (index >= 0) {
        auto v = cfg.Get(section, setting, (ULONG)index, noExpand, true);
        if (!v.has_value())
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"setting not found: " + section + L"/"
                                  + setting);
        rows.pushBack(json::JsonValue(*v));
    } else {
        auto all = cfg.GetList(section, setting, noExpand, true);
        if (all.empty())
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"setting not found: " + section + L"/"
                                  + setting);
        for (auto& v : all)
            rows.pushBack(json::JsonValue(v));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HBoxGet(const json::JsonValue& params,
                 const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring setting;
    if (!GetStr(params, L"setting", &setting) || setting.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'setting'");
    const bool noExpand = GetBool(params, L"noexpand",
                                  GetBool(params, L"raw", true));
    return ConfValues(name, setting, GetInt(params, L"index", -1), noExpand);
}

OpResult HBoxListSetting(const json::JsonValue& params,
                         const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    // 04 §11 遗留 2 消除：no_tmpls 参数化（缺省 true = 排除模板注入项，
    // 与本 op 既有行为一致；false = 含模板注入项，box list-setting --all）。
    const bool noTmpls = GetBool(params, L"no_tmpls", true);
    json::JsonValue rows = json::JsonValue::Array();
    if (noTmpls) {
        model::ConfigStore cfg;
        for (auto& n : cfg.ListSettings(name))
            rows.pushBack(json::JsonValue(n));
        return OpResult::Succeed(std::move(rows));
    }
    // NO_TEMPLS 反向：直接枚举（ConfigStore::ListSettings 恒带 NO_TEMPLS，
    // 不可参数化——驱动 Conf_Get_Setting_Name 的 flag 位由本处自组）
    if (name.size() > 64)
        return OpResult::Fail(SbieStatus::INVALID, L"box name too long");
    for (ULONG i = 0; i < 4096; ++i) {
        WCHAR buf[130] = L"";
        ULONG flags = CONF_GET_NO_EXPAND;   // --all 仅去 NO_TEMPLS
        (void)drv::ApiP()->SbieApi_QueryConf(name.c_str(), nullptr,
                                             i | flags, buf, sizeof(buf));
        if (buf[0] == L'\0')
            break;
        rows.pushBack(json::JsonValue(buf));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HProcList(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring box;
    GetStr(params, L"box", &box); // 可选
    const bool allSessions = GetBool(params, L"all_sessions", false);
    model::ProcessRepository procs(nullptr, svc::SvcClient::Instance());
    std::vector<model::ProcEntry> list = procs.Enum(allSessions, box);
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& p : list) {
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"pid", json::JsonValue((long long)p.pid));
        r.set(L"box", json::JsonValue(p.box));
        r.set(L"image", json::JsonValue(p.image));
        r.set(L"session", json::JsonValue((long long)p.sessionId));
        r.set(L"started", json::JsonValue(FormatCreateTime(p.createTime)));
        r.set(L"flags", json::JsonValue((long long)p.flags));
        rows.pushBack(std::move(r));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HProcInfo(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    const long long pid = GetInt(params, L"pid", 0);
    if (pid <= 0 || pid > 0xFFFFFFFFLL)
        return OpResult::Fail(SbieStatus::INVALID, L"missing/invalid param 'pid'");

    // 驱动侧五元组（进程存在性以此为准——02 §7.4）
    drv::ProcQuery q;
    SbieStatus st = drv::QueryProcessById((ULONG)pid, &q);
    if (st != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"process not found (or not sandboxed): pid "
                              + std::to_wstring((unsigned long)pid));

    // SbieSvc 侧详情（经专职线程；缺席时仅回驱动字段）
    svc::ProcInfo pi;
    bool haveSvc = false;
    SbieStatus svcSt = SvcCall([&] {
        return svc::SvcClient::Instance().GetProcInfo((ULONG)pid, 7, &pi);
    });
    haveSvc = (svcSt == SbieStatus::OK);

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"pid", json::JsonValue(pid));
    data.set(L"box", json::JsonValue(q.box));
    data.set(L"image", json::JsonValue(q.image));
    data.set(L"session", json::JsonValue((long long)q.sessionId));
    data.set(L"started", json::JsonValue(FormatCreateTime(q.createTime)));
    data.set(L"service_detail", json::JsonValue(haveSvc));
    if (haveSvc) {
        data.set(L"parent_pid", json::JsonValue((long long)pi.parentId));
        data.set(L"flags", json::JsonValue((long long)pi.flags));
        data.set(L"suspended", json::JsonValue(pi.suspended));
        data.set(L"cmdline", json::JsonValue(pi.cmdline));
        data.set(L"workdir", json::JsonValue(pi.workdir));
    }
    return OpResult::Succeed(std::move(data));
}

OpResult HCfgGet(const json::JsonValue& params,
                 const std::shared_ptr<Connection>&)
{
    std::wstring setting;
    if (!GetStr(params, L"setting", &setting) || setting.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'setting'");
    std::wstring section = L"GlobalSettings";
    GetStr(params, L"section", &section); // 可选，缺省 GlobalSettings
    const bool noExpand = GetBool(params, L"noexpand",
                                  GetBool(params, L"raw", true));
    return ConfValues(section, setting, GetInt(params, L"index", -1),
                      noExpand);
}

OpResult HCfgListSetting(const json::JsonValue& params,
                         const std::shared_ptr<Connection>&)
{
    std::wstring section = L"GlobalSettings";
    GetStr(params, L"section", &section); // 可选
    model::ConfigStore cfg;
    auto names = cfg.ListSettings(section);
    json::JsonValue rows = json::JsonValue::Array();
    for (auto& n : names)
        rows.pushBack(json::JsonValue(n));
    return OpResult::Succeed(std::move(rows));
}

OpResult HCfgPath(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    std::wstring path;
    bool isHome = false;
    SbieStatus st = SvcCall([&] {
        return svc::SvcClient::Instance().IniGetPath(&path, &isHome);
    });
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"SbieSvc IniGetPath failed", 0);
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"path", json::JsonValue(path));
    data.set(L"home", json::JsonValue(isHome));
    return OpResult::Succeed(std::move(data));
}

OpResult HCfgReload(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    const bool reconfigure = GetBool(params, L"reconfigure", false);
    SbieStatus st = drv::ReloadConf(0, reconfigure);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"SbieApi_ReloadConf failed");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"reloaded", json::JsonValue(true));
    return OpResult::Succeed(std::move(data));
}

OpResult HLogDump(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    if (!ServerState::Get().LogPumpActive())
        return OpResult::Fail(SbieStatus::SERVER_UNAVAILABLE,
                              L"log pump unavailable (session leader not set"
                              L" or driver absent)");
    const long long last = GetInt(params, L"last", 100);
    json::JsonValue rows = LogDumpJson(last > 0 ? (size_t)last : 0);
    return OpResult::Succeed(std::move(rows));
}

OpResult HLogWatch(const json::JsonValue&, const std::shared_ptr<Connection>& conn)
{
    if (!ServerState::Get().LogPumpActive())
        return OpResult::Fail(SbieStatus::SERVER_UNAVAILABLE,
                              L"log pump unavailable (session leader not set"
                              L" or driver absent)");
    ServerState::Get().AddLogSubscriber(conn);
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"subscribed", json::JsonValue(true));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// trace/资源监控 op（波 D1，04 §20）。泵常驻（TracePump：MonitorControl 开启
// 本会话监控 → API_MONITOR_GET2 循环拉取 → 环形缓冲 + trace.event 推送）。
// watch 订阅登记与 log.watch 同款（连接切订阅者轮询模式）；过滤在 client 侧
// 做（推送帧自带全字段，各订阅者自行筛——SandMan TraceView 同语义）。dump
// 服务端过滤（last/box/type_code/pid）。
// ---------------------------------------------------------------------------

OpResult HTraceDump(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    if (!TracePumpRunning())
        return OpResult::Fail(SbieStatus::SERVER_UNAVAILABLE,
                              L"trace pump unavailable (driver absent or"
                              L" monitor control rejected)");
    const long long last = GetInt(params, L"last", 100);
    std::wstring box;
    GetStr(params, L"box", &box);
    const long long typeCode = GetInt(params, L"type_code", 0);
    const long long pid = GetInt(params, L"pid", 0);
    json::JsonValue rows = TraceDumpJson(
        last > 0 ? (size_t)last : 0, box,
        typeCode > 0 ? (ULONG)typeCode : 0,
        pid > 0 ? (ULONG)pid : 0);
    return OpResult::Succeed(std::move(rows));
}

OpResult HTraceWatch(const json::JsonValue&, const std::shared_ptr<Connection>& conn)
{
    if (!TracePumpRunning())
        return OpResult::Fail(SbieStatus::SERVER_UNAVAILABLE,
                              L"trace pump unavailable (driver absent or"
                              L" monitor control rejected)");
    ServerState::Get().AddTraceSubscriber(conn);
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"subscribed", json::JsonValue(true));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// 写路径 op（server 写路径波次，04 §12）。契约：
//   * 参数名与 client IpcRoute 预接线逐一对应（对拍表见 04 §12）；
//   * 一切触 SbieSvc 的 Model 调用（ConfigStore::Set*/Delete、BoxRepository
//     的 Create/Rename/Delete/SetEnabled、ProcessRepository 的 Start/Kill、
//     TemplateRegistry 的 Apply/Revoke/Info[含 FindSandboxieIni→IniGetPath]）
//     一律经 SvcCall 专职线程（03 §1 线程亲和，SvcProxy.h）；
//   * 纯驱动/文件系统读写（GetInfo/GetList/List/Take/Remove/Select/SetInfo）
//     在 worker 线程直接执行；
//   * data.message 与 client 直连路径的输出文案一致（RenderGenericOk 渲染）。
// ---------------------------------------------------------------------------

// unix 秒 → ISO-8601 本地时间（与 client BoxProcCommands::FormatUnixSeconds
// 同构——box.snap.list 的 DATE 列两侧一致）
std::wstring FormatUnixSeconds(ULONGLONG secs)
{
    if (secs == 0)
        return L"-";
    ULARGE_INTEGER u{};
    u.QuadPart = secs * 10000000ull + 116444736000000000ull;
    FILETIME utc{ u.LowPart, u.HighPart };
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local)
        || !FileTimeToSystemTime(&local, &st))
        return L"-";
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 密码：params.password > server 进程的 SBIE_PASS（server 由首个 client 派生
// 时继承其环境；后续 client 不同密码需经 params 传递——client 侧接线为后续
// 波次，04 §12 坑记录）
std::wstring ResolvePasswordParam(const json::JsonValue& params)
{
    std::wstring pw;
    if (GetStr(params, L"password", &pw))
        return pw;
    wchar_t buf[256] = L"";
    DWORD n = GetEnvironmentVariableW(L"SBIE_PASS", buf, 256);
    if (n > 0 && n < 256)
        return buf;
    return L"";
}

// 写命令的 WRONG_PASSWORD 提示后缀（P0-8：unlock 仅验证、密码不缓存——
// 后续写逐次携带 --password/SBIE_PASS；cli 侧 boxproc::PasswordHint 同款）
std::wstring PasswordHintText(SbieStatus st, const std::wstring& pw)
{
    if (st == SbieStatus::ACCESS_DENIED && pw.empty())
        return L" (config is locked: pass --password <pw> or set SBIE_PASS)";
    return L"";
}

// mode 参数（box.set / cfg.set）：update|append|insert，缺省 update
bool ParseSetMode(const json::JsonValue& params,
                  svc::SvcClient::SetMode* mode)
{
    std::wstring m;
    if (!GetStr(params, L"mode", &m) || m.empty()) {
        *mode = svc::SvcClient::SetMode::Update;
        return true;
    }
    if (m == L"update")
        *mode = svc::SvcClient::SetMode::Update;
    else if (m == L"append")
        *mode = svc::SvcClient::SetMode::Append;
    else if (m == L"insert")
        *mode = svc::SvcClient::SetMode::Insert;
    else
        return false;
    return true;
}

// SbieSvc 不可达的统一文案（与 client 直连分支同款）
OpResult FailSvcDown(const wchar_t* op)
{
    return OpResult::Fail(SbieStatus::ERR_SVC_TRANSPORT,
                          std::wstring(op) + L" requires SbieSvc"
                                             L" (write path)");
}

// box 存在性（纯驱动读，worker 线程安全）
SbieStatus GetBoxInfoSafe(const std::wstring& name, model::BoxInfo* out)
{
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    return repo.GetInfo(name, out);   // GetInfo 不触 SbieSvc（Boxes.cpp）
}

// 内容清理执行器（波次 A，07-P0-1/07-P0-2）：NeverDelete 检查 → OnBoxDelete
// 触发器（noTriggers=false）→ CleanContents/RemoveRoot。文件递归删除器与
// clean 语义现统一在 server\Guardian.cpp（本文件原同构副本已并入）。
SbieStatus PurgeBoxContents(const std::wstring& name, bool removeRoot,
                            bool noTriggers)
{
    return ExecuteBoxPurge(name,
                           removeRoot ? PurgeMode::RemoveRoot
                                      : PurgeMode::CleanContents,
                           noTriggers);
}

// NeverDelete 保护读取（纯驱动缓存读，worker 线程安全）
bool NeverDeleteProtected(const std::wstring& box)
{
    auto nd = model::ConfigStore().Get(box, L"NeverDelete", 0, true, true);
    return nd.has_value() && *nd == L"y";
}

OpResult HBoxCreate(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"invalid box name '" + name + L"' (max 38"
                              L" chars of A-Z a-z 0-9 _; reserved words"
                              L" excluded)");
    std::vector<std::wstring> tpls;
    if (const json::JsonValue* a = params.find(L"templates");
        a && a->isArray()) {
        for (const json::JsonValue& t : a->items())
            if (t.isString())
                tpls.push_back(t.asString());
    }
    // 波 B（07-P1-1）：--type 预设（缺省空 = 现行为 Enabled=y）
    std::wstring typeStr;
    GetStr(params, L"type", &typeStr);
    const model::BoxTypePreset* preset = nullptr;
    if (!typeStr.empty()) {
        preset = model::FindBoxTypePreset(typeStr);
        if (!preset)
            return OpResult::Fail(SbieStatus::INVALID,
                                  L"unknown box type '" + typeStr
                                  + L"' (see: sbie-cli box types)");
    }

    // pw-aware（P0-11）：Create = SET Enabled=y（Model BoxRepository::Create
    // 同款 IniSetSetting，密码透传）；--type = 预设键组（Enabled 之后）；
    // 模板 = Info 存在性检查 + Append Template=<名>（与 client 直连 /
    // template apply 同序）
    const std::wstring pw = ResolvePasswordParam(params);

    bool enabled = false, exists = false;
    SbieStatus chk = drv::IsBoxEnabled(name, &enabled, &exists);
    if (chk != SbieStatus::OK)
        return OpResult::Fail(chk, L"query failed for box: " + name);
    if (exists)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' already exists");

    std::wstring failedTpl;
    SbieStatus st = SvcCall([&] {
        svc::SvcClient& svc = svc::SvcClient::Instance();
        SbieStatus s = svc.IniSetSetting(name, L"Enabled", L"y",
                                         svc::SvcClient::SetMode::Update,
                                         true, pw);
        if (s != SbieStatus::OK)
            return s;
        if (preset) {
            s = model::ApplyBoxTypeKeys(name, *preset, pw);
            if (s != SbieStatus::OK)
                return s;
        }
        for (const std::wstring& tpl : tpls) {
            model::TemplateRegistry treg(nullptr, svc);
            std::vector<std::pair<std::wstring, std::wstring>> probe;
            SbieStatus ts = treg.Info(tpl, &probe);   // 含 IniGetPath（SbieSvc）
            if (ts == SbieStatus::OK)
                ts = svc.IniSetSetting(name, L"Template", tpl,
                                       svc::SvcClient::SetMode::Append,
                                       true, pw);
            if (ts != SbieStatus::OK) {
                failedTpl = tpl;
                return ts;
            }
        }
        return SbieStatus::OK;
    });
    if (st == SbieStatus::NOT_FOUND && !failedTpl.empty())
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"template '" + failedTpl + L"' not found");
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' already exists");
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box create");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box create failed"
                              + std::wstring(PasswordHintText(st, pw)));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message",
             json::JsonValue(preset
                 ? L"box '" + name + L"' created (type: "
                   + std::wstring(preset->type) + L")"
                 : L"box '" + name + L"' created"));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxSet(const json::JsonValue& params,
                 const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring setting;
    if (!GetStr(params, L"setting", &setting) || setting.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'setting'");
    std::wstring value;
    GetStr(params, L"value", &value);   // 空串 = 置空（0x1811 空 value 删值）
    svc::SvcClient::SetMode mode;
    if (!ParseSetMode(params, &mode))
        return OpResult::Fail(SbieStatus::INVALID,
                              L"bad 'mode' (update|append|insert)");
    const std::wstring pw = ResolvePasswordParam(params);
    const bool refresh = GetBool(params, L"refresh", true);

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    // index 组合语义（client 直连 --index 同款：替换第 i 个值，其余重放；
    // client 预接线暂未发送 index——server 侧先行支持，04 §12 对拍表）
    const long long index = GetInt(params, L"index", -1);
    if (index >= 0) {
        model::ConfigStore cfgRead;
        std::vector<std::wstring> vals = cfgRead.GetList(name, setting);
        if ((size_t)index >= vals.size())
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"setting '" + setting + L"' has "
                                  + std::to_wstring(vals.size())
                                  + L" value(s); index "
                                  + std::to_wstring(index)
                                  + L" out of range");
        vals[(size_t)index] = value;
        st = SvcCall([&] {
            model::ConfigStore cfg;
            SbieStatus s = cfg.Set(name, setting, vals[0], false, pw);
            for (size_t i = 1; i < vals.size() && s == SbieStatus::OK; ++i)
                s = cfg.SetAppend(name, setting, vals[i],
                                  refresh && i + 1 == vals.size(), pw);
            return s;
        });
    } else if (mode == svc::SvcClient::SetMode::Append) {
        st = SvcCall([&] {
            return model::ConfigStore().SetAppend(name, setting, value,
                                                  refresh, pw);
        });
    } else if (mode == svc::SvcClient::SetMode::Insert) {
        st = SvcCall([&] {
            return model::ConfigStore().SetInsert(name, setting, value,
                                                  refresh, pw);
        });
    } else {
        st = SvcCall([&] {
            return model::ConfigStore().Set(name, setting, value, refresh, pw);
        });
    }
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box set");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box set failed" + PasswordHintText(st, pw));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"set"));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxDelete(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    const bool delFiles = GetBool(params, L"files", false);
    const bool keepSection = GetBool(params, L"keep_section", false);
    // no_triggers（波次 A，07-P0-1）：--no-triggers 逃生旗标——跳过内容删除
    // 前的 OnBoxDelete 触发器（仅 --files 路径执行触发器）
    const bool noTriggers = GetBool(params, L"no_triggers", false);
    // pw-aware（P0-11）：节删除经 ConfigStore::Delete 带 password 形参
    //（Model BoxRepository::Delete 内部固定空密码）；检查与目录删除逻辑
    // 与 Model Delete 同序（存在 → BOX_BUSY → NeverDelete → 触发器 → 目录 → 节）
    const std::wstring pw = ResolvePasswordParam(params);

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    if (bi.hasProcesses)
        return OpResult::Fail(SbieStatus::BOX_BUSY,
                              L"box '" + name + L"' has running processes;"
                              L" terminate them first (proc kill-all /"
                              L" proc kill)");

    if (delFiles) {
        if (NeverDeleteProtected(name))
            return OpResult::Fail(SbieStatus::ACCESS_DENIED,
                                  L"box '" + name + L"' is protected"
                                  L" (NeverDelete=y)");
        // 内容删除（含 OnBoxDelete 触发器——删除内容前逐条执行，07-P0-1）
        st = PurgeBoxContents(name, true /*RemoveRoot*/, noTriggers);
        if (st != SbieStatus::OK)
            return OpResult::Fail(st, L"box delete failed (file error)");
    }

    if (!keepSection) {
        st = SvcCall([&] {
            return model::ConfigStore().Delete(name, L"*", std::nullopt,
                                               true, pw);
        });
        if (st == SbieStatus::ERR_SVC_TRANSPORT)
            return FailSvcDown(L"box delete");
        if (st != SbieStatus::OK)
            return OpResult::Fail(st,
                                  L"box delete failed"
                                  + PasswordHintText(st, pw));
    }

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"box '" + name + L"' deleted"));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxRename(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring oldN;
    if (!GetStr(params, L"old", &oldN) || oldN.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'old'");
    std::wstring newN;
    if (!GetStr(params, L"new", &newN) || newN.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'new'");
    if (model::BoxRepository::ValidateName(newN) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"invalid new box name '" + newN + L"' (max 38"
                              L" chars of A-Z a-z 0-9 _; reserved words"
                              L" excluded)");
    // pw-aware（P0-11）：整节替换 + 删旧节两步均带密码（Model Rename 的
    // pw 等价转录，04 §8.15 协议技巧）
    const std::wstring pw = ResolvePasswordParam(params);

    SbieStatus st = SvcCall([&] {
        bool enabled = false, exists = false;
        if (drv::IsBoxEnabled(oldN, &enabled, &exists) != SbieStatus::OK
            || !exists)
            return SbieStatus::NOT_FOUND;
        if (drv::IsBoxEnabled(newN, &enabled, &exists) != SbieStatus::OK
            || exists)
            return SbieStatus::NOT_FOUND;

        model::ConfigStore cfg;
        std::vector<std::wstring> settings = cfg.ListSettings(oldN);
        std::wstring sectionData;
        for (const std::wstring& key : settings) {
            for (const std::wstring& v : cfg.GetList(oldN, key))
                sectionData += key + L"=" + v + L"\n";
        }
        if (sectionData.empty())
            sectionData = L"Enabled=y\n";   // 空节兜底（保持 Create 语义）

        SbieStatus s = cfg.Set(newN, L"", sectionData, false, pw);
        if (s == SbieStatus::OK) {
            s = cfg.Delete(oldN, L"*", std::nullopt, true, pw);
            if (s != SbieStatus::OK)
                cfg.Delete(newN, L"*", std::nullopt, false, pw); // 回滚
        }
        return s;
    });
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + oldN + L"' not found, or '" + newN
                              + L"' already exists");
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box rename");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box rename failed" + PasswordHintText(st, pw));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message",
             json::JsonValue(L"box '" + oldN + L"' renamed to '" + newN
                                 + L"'"));
    return OpResult::Succeed(std::move(data));
}

// 组 SnapshotManager（worker 线程；GetInfo 纯驱动读）
SbieStatus MakeSnapMgr(const std::wstring& name, model::SnapshotManager* out)
{
    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st != SbieStatus::OK)
        return st;
    *out = model::SnapshotManager(bi);
    return SbieStatus::OK;
}

OpResult HBoxSnapList(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    std::wstring cur, def;
    std::vector<model::SnapshotInfo> snaps = sm.List(&cur, &def);
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::SnapshotInfo& s : snaps) {
        const bool isCur = _wcsicmp(s.id.c_str(), cur.c_str()) == 0;
        const bool isDef = _wcsicmp(s.id.c_str(), def.c_str()) == 0;
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"id", json::JsonValue(s.id));
        r.set(L"name", json::JsonValue(s.name));
        r.set(L"date", json::JsonValue(FormatUnixSeconds(s.date)));
        r.set(L"current", json::JsonValue(isCur));
        r.set(L"default", json::JsonValue(isDef));
        r.set(L"parent_id", json::JsonValue(s.parentId));
        r.set(L"info", json::JsonValue(s.info));
        rows.pushBack(std::move(r));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HBoxSnapTake(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring snapName;
    if (!GetStr(params, L"snap_name", &snapName) || snapName.empty())
        return OpResult::Fail(SbieStatus::USAGE,
                              L"missing param 'snap_name'");
    std::wstring info;
    GetStr(params, L"info", &info);   // 可选：take 后补写 Description

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    st = sm.Take(snapName);   // 纯文件操作（要求沙箱内无进程）
    if (st == SbieStatus::BOX_BUSY)
        return OpResult::Fail(SbieStatus::BOX_BUSY,
                              L"box '" + name + L"' has running processes");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"cannot take snapshot (box not initialized"
                              L" or file error)");
    // Take 契约不回 ID：读回 Current（即新快照）；--info 补写（client 直连
    // 同款两步）
    std::wstring cur;
    sm.List(&cur, nullptr);
    if (!info.empty())
        (void)sm.SetInfo(cur, std::nullopt, info);

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"id", json::JsonValue(cur));
    data.set(L"message",
             json::JsonValue(L"snapshot #" + cur + L" taken"));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxSnapRemove(const json::JsonValue& params,
                        const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring id;
    if (!GetStr(params, L"id", &id) || id.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'id'");

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    st = sm.Remove(id);
    if (st == SbieStatus::BOX_BUSY)
        return OpResult::Fail(SbieStatus::BOX_BUSY,
                              L"box '" + name + L"' has running processes");
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"snapshot '" + id + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"cannot remove snapshot (shared parent or"
                              L" file error)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"snapshot #" + id + L" removed"));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxSnapSelect(const json::JsonValue& params,
                        const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring id;
    if (!GetStr(params, L"id", &id) || id.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'id'");

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    st = sm.Select(id);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"snapshot '" + id + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"snapshot select failed");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"switched to snapshot #" + id));
    return OpResult::Succeed(std::move(data));
}

OpResult HBoxSnapSetInfo(const json::JsonValue& params,
                         const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring id;
    if (!GetStr(params, L"id", &id) || id.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'id'");
    // new_name / new_info：字段在场即改（client 仅在旗标给定时发送）
    std::optional<std::wstring> newName, newInfo;
    std::wstring v;
    if (GetStr(params, L"new_name", &v) && !v.empty())
        newName = v;
    if (GetStr(params, L"new_info", &v) && !v.empty())
        newInfo = v;

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    st = sm.SetInfo(id, newName, newInfo);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"snapshot '" + id + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"snapshot set-info failed");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"updated"));
    return OpResult::Succeed(std::move(data));
}

// box.snap.default（波次 E，08-P2-5）：读形态 params {name} →
// {box, current, default}；写形态 {name, id} 或 {name, clear:true} →
// {box, default, message}。写经 Model SnapshotManager::SetDefault
//（Load→改→Save 整文件 UTF-8 无 BOM，非 ASCII 快照名安全）。
OpResult HBoxSnapDefault(const json::JsonValue& params,
                         const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring id;
    GetStr(params, L"id", &id);
    const bool clear = GetBool(params, L"clear", false);
    if (!id.empty() && clear)
        return OpResult::Fail(SbieStatus::USAGE,
                              L"'id' and 'clear' are mutually exclusive");

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box '" + name + L"' not found");

    std::wstring currentId, defaultId;
    std::vector<model::SnapshotInfo> snaps = sm.List(&currentId, &defaultId);

    if (id.empty() && !clear) {
        // 读形态（client 直连同款：current/default 两行 + 快照名）
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"box", json::JsonValue(name));
        data.set(L"current", json::JsonValue(currentId));
        data.set(L"default", json::JsonValue(defaultId));
        return OpResult::Succeed(std::move(data));
    }

    if (!clear) {
        bool hit = false;
        for (const auto& s : snaps)
            if (_wcsicmp(s.id.c_str(), id.c_str()) == 0)
                hit = true;
        if (!hit)
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"snapshot not found: " + id);
    }
    st = sm.SetDefault(clear ? std::wstring() : id);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"failed to write Snapshots.ini [Current]"
                                  L" Default");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"box", json::JsonValue(name));
    data.set(L"default", json::JsonValue(clear ? L"" : id));
    data.set(L"message",
             json::JsonValue(clear ? L"default snapshot marker cleared"
                                   : L"default snapshot set to " + id));
    return OpResult::Succeed(std::move(data));
}

OpResult HProcStart(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'box'");
    std::wstring cmd;
    if (!GetStr(params, L"cmd", &cmd) || cmd.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'cmd'");
    std::wstring dir;
    GetStr(params, L"dir", &dir);   // 可选；空 = server 当前目录（Model 层
                                    // 以调用方 cwd 代入——直连为 client cwd，
                                    // 坑记录 04 §12）
    const bool elevated = GetBool(params, L"elevated", false);

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(box, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + box + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + box);

    svc::RunResult rr{};
    st = SvcCall([&] {
        model::ProcessRepository repo(nullptr, svc::SvcClient::Instance());
        return repo.Start(box, cmd, dir, elevated, &rr);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"proc start");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"proc start failed");

    json::JsonValue data = json::JsonValue::Object();
    if (elevated || !rr.hProcess) {
        // 降级路径无句柄/PID（SbieDll_RunStartExe 不外露）——与 client 直连
        // 文案一致
        data.set(L"elevated", json::JsonValue(true));
        data.set(L"message",
                 json::JsonValue(L"start requested (elevated; pid not"
                                 L" tracked)"));
        return OpResult::Succeed(std::move(data));
    }
    // --wait 需要进程句柄，client 恒走直连（proc_cmd.cpp）；server 侧句柄
    // 用完即关（03 §8.4）
    CloseHandle(rr.hProcess);
    data.set(L"pid", json::JsonValue((long long)rr.pid));
    data.set(L"message", json::JsonValue(std::to_wstring(rr.pid)));
    return OpResult::Succeed(std::move(data));
}

OpResult HProcKill(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    const long long pid = GetInt(params, L"pid", 0);
    if (pid <= 0 || pid > 0xFFFFFFFFLL)
        return OpResult::Fail(SbieStatus::INVALID, L"missing/invalid param 'pid'");
    // 存在性（在沙箱内）判定：QueryProcessEx2（与 client 直连一致）
    drv::ProcQuery q;
    if (drv::QueryProcessById((ULONG)pid, &q) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"pid " + std::to_wstring((unsigned long)pid)
                              + L" is not a running sandboxed process");

    SbieStatus st = SvcCall([&] {
        model::ProcessRepository repo(nullptr, svc::SvcClient::Instance());
        return repo.Kill((ULONG)pid);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"proc kill");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"proc kill failed (SbieSvc required)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"pid "
                                         + std::to_wstring((unsigned long)pid)
                                         + L" killed"));
    return OpResult::Succeed(std::move(data));
}

OpResult HCfgSet(const json::JsonValue& params,
                 const std::shared_ptr<Connection>&)
{
    std::wstring setting;
    if (!GetStr(params, L"setting", &setting) || setting.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'setting'");
    std::wstring value;
    GetStr(params, L"value", &value);
    std::wstring section = L"GlobalSettings";
    GetStr(params, L"section", &section);
    svc::SvcClient::SetMode mode;
    if (!ParseSetMode(params, &mode))
        return OpResult::Fail(SbieStatus::INVALID,
                              L"bad 'mode' (update|append|insert)");
    const std::wstring pw = ResolvePasswordParam(params);
    const bool refresh = GetBool(params, L"refresh", true);

    SbieStatus st = SvcCall([&] {
        model::ConfigStore cfg;
        switch (mode) {
        case svc::SvcClient::SetMode::Append:
            return cfg.SetAppend(section, setting, value, refresh, pw);
        case svc::SvcClient::SetMode::Insert:
            return cfg.SetInsert(section, setting, value, refresh, pw);
        default:
            return cfg.Set(section, setting, value, refresh, pw);
        }
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"cfg set");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"cfg set failed" + PasswordHintText(st, pw));
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"set"));
    return OpResult::Succeed(std::move(data));
}

// tpl.*（波次 E，06 §5 遗留收口）。注意 TemplateRegistry 的 List/Info 都可能
// 内部触 SbieSvc（FindSandboxieIni→IniGetPath）——按 03 §1 线程亲和，凡触
// svc::SvcClient 的工作一律经 SvcCall 专职线程（实测坑：worker 线程直调
// LPC 端口会挂起，见 docs/04 §22.4）。
OpResult HTplList(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring clazz = L"*";
    GetStr(params, L"class", &clazz);   // 可选：L"*" = 全部
    const bool all = GetBool(params, L"all", false);   // 08-P2-7 --all
    std::vector<model::TemplateInfo> list;
    std::wstring iniPath;   // Sandboxie.ini 权威路径（Tmpl.Hide 本地节扫描用）
    SbieStatus st = SvcCall([&] {
        model::TemplateRegistry treg(nullptr, svc::SvcClient::Instance());
        list = treg.List(clazz);
        bool isHome = false;
        svc::SvcClient::Instance().IniGetPath(&iniPath, &isHome);  // 尽力而为
        return SbieStatus::OK;
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"template list");
    // 08-P2-7：默认滤 Tmpl.Hide=y（ipcc/TmplHide；Sandboxie.ini 路径已在
    // SvcCall 内取得——助手在 worker 线程不得自行触 SbieSvc，§22.4）
    const std::vector<std::wstring> hidden =
        all ? std::vector<std::wstring>() : tmplhide::HiddenNames(iniPath);
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::TemplateInfo& ti : list) {
        if (tmplhide::Contains(hidden, ti.name))
            continue;
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"name", json::JsonValue(ti.name));
        // 空 class 不置键（CellOf 缺键 → "-"，与 client 直连的呈现对齐）
        if (!ti.clazz.empty())
            r.set(L"class", json::JsonValue(ti.clazz));
        r.set(L"description", json::JsonValue(ti.descr));
        rows.pushBack(std::move(r));
    }
    return OpResult::Succeed(std::move(rows));
}

OpResult HTplInfo(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    model::TemplateRegistry treg(nullptr, svc::SvcClient::Instance());
    std::vector<std::pair<std::wstring, std::wstring>> settings;
    // Info 内部含 IniGetPath（SbieSvc LPC）→ SvcProxy 专职线程（03 §1）
    SbieStatus st = SvcCall([&] { return treg.Info(name, &settings); });
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"template not found: " + name);
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
    return OpResult::Succeed(std::move(data));
}

// tpl.check（波次 E，06 §5 遗留收口）：与 client 直连同构——盘上
// Sandboxie.ini 三节（box / DefaultTemplates / GlobalSettings）的 Template=
// 值分档对照，known = TemplateRegistry 全目录命中。来源分档必须读盘上 ini
// （驱动缓存查询带 GlobalSettings 回退，不能用于分档）。svc 面（IniGetPath
// + List 的内部 IniGetPath）合并进单个 SvcCall（03 §1 线程亲和）；盘上节
// 读（GetPrivateProfileSectionW）为纯文件 IO，留在 worker 线程。
OpResult HTplCheck(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'box'");
    bool enabled = false, exists = false;
    if (drv::IsBoxEnabled(box, &enabled, &exists) != SbieStatus::OK || !exists)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box not found: " + box);

    std::wstring iniPath;
    std::vector<model::TemplateInfo> catalog;
    SbieStatus st = SvcCall([&] {
        bool isHome = false;
        SbieStatus s = svc::SvcClient::Instance().IniGetPath(&iniPath,
                                                             &isHome);
        if (s != SbieStatus::OK)
            return s;
        model::TemplateRegistry treg(nullptr, svc::SvcClient::Instance());
        catalog = treg.List(L"*");
        return SbieStatus::OK;
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"template check");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"template check requires SbieSvc to locate"
                                  L" Sandboxie.ini");
    auto known = [&catalog](const std::wstring& n) {
        for (const auto& ti : catalog)
            if (_wcsicmp(ti.name.c_str(), n.c_str()) == 0)
                return true;
        return false;
    };

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

    json::JsonValue jrows = json::JsonValue::Array();
    for (const auto& r : rows) {
        json::JsonValue j = json::JsonValue::Object();
        j.set(L"template", json::JsonValue(r.tmpl));
        j.set(L"source", json::JsonValue(r.source));
        j.set(L"exists", json::JsonValue(r.exists));
        jrows.pushBack(std::move(j));
    }
    return OpResult::Succeed(std::move(jrows));
}

OpResult HTplApply(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'box'");
    std::wstring tmpl;
    if (!GetStr(params, L"name", &tmpl) || tmpl.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");

    bool enabled = false, exists = false;
    if (drv::IsBoxEnabled(box, &enabled, &exists) != SbieStatus::OK
        || !exists)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box not found: " + box);

    SbieStatus st = SvcCall([&] {
        model::TemplateRegistry treg(nullptr, svc::SvcClient::Instance());
        std::vector<std::pair<std::wstring, std::wstring>> probe;
        SbieStatus s = treg.Info(tmpl, &probe);   // 含 IniGetPath（SbieSvc）
        if (s != SbieStatus::OK)
            return s;
        // pw-aware（P0-11）：Append Template=<名>，密码透传（Model Apply 的
        // IniSetSetting 等价；client 直连 template_cmd.cpp 同款）
        return svc::SvcClient::Instance().IniSetSetting(
            box, L"Template", tmpl, svc::SvcClient::SetMode::Append, true,
            ResolvePasswordParam(params));
    });
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"template '" + tmpl + L"' not found");
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"template apply");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"apply failed (locked config? use"
                              L" --password)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"applied"));
    return OpResult::Succeed(std::move(data));
}

OpResult HTplRevoke(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'box'");
    std::wstring tmpl;
    if (!GetStr(params, L"name", &tmpl) || tmpl.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");

    bool enabled = false, exists = false;
    if (drv::IsBoxEnabled(box, &enabled, &exists) != SbieStatus::OK
        || !exists)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box not found: " + box);

    // 已激活对照（驱动缓存读，worker 线程）
    std::vector<std::wstring> applied;
    drv::QueryConfList(box, L"Template", true, true, &applied);
    bool active = false;
    for (const auto& v : applied)
        if (v == tmpl)
            active = true;
    if (!active)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"template not active on box " + box + L": "
                              + tmpl);

    SbieStatus st = SvcCall([&] {
        // pw-aware（P0-11）：0x1814 携带 value = RemoveValue（只删该值，
        // §8.22 语义），密码透传（Model Revoke 的 IniSetSetting 等价）
        return svc::SvcClient::Instance().IniSetSetting(
            box, L"Template", tmpl, svc::SvcClient::SetMode::Delete, true,
            ResolvePasswordParam(params));
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"template revoke");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"revoke failed (locked config? use"
                              L" --password)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"revoked"));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// 补缺波次 op（06 缺口表 P0-1..P0-9 收口；client IpcRoute 已预接线）。
// 契约同上：触 SbieSvc 的调用经 SvcProxy 专职线程；纯驱动/文件操作在
// worker 线程直执；data.message 与 client 直连路径输出文案一致。
// ---------------------------------------------------------------------------

// proc.killAll（P0-1；P1-3 参数化 box 可空 = 全局）：
//   * box 给定 —— 计数 = 请求时该 box 进程数，KillBox（不查
//     ExcludeFromTerminateAll——QSbieAPI 单箱 TerminateAll 亦不查，
//     SbieAPI.cpp:1764-1777）；
//   * box 空（`proc kill-all --all`）—— EnumBoxes 循环 KillBox（仅启用 box；
//     进程只可能运行于启用 box，与 box.list 缺省口径一致），计数 = 各 box
//     请求时进程数之和；ExcludeFromTerminateAll=y 的 box 跳过，除非
//     no_exceptions（08-P1-1，对齐 QSbieAPI TerminateAll(bNoExceptions)，
//     SbieAPI.cpp:1786-1792）。
OpResult HProcKillAll(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    std::wstring box;
    GetStr(params, L"box", &box);   // 可选：空 = 全局（P1-3）
    const bool noExceptions = GetBool(params, L"no_exceptions", false);

    std::vector<std::wstring> targets;
    size_t skipped = 0;
    if (!box.empty()) {
        model::BoxInfo bi;
        SbieStatus st = GetBoxInfoSafe(box, &bi);
        if (st == SbieStatus::NOT_FOUND)
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"box '" + box + L"' not found");
        if (st != SbieStatus::OK)
            return OpResult::Fail(st, L"query failed for box: " + box);
        targets.push_back(box);
    } else {
        model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
        for (const model::BoxInfo& bi : repo.EnumBoxes(false)) {   // 启用中的 box
            if (!noExceptions) {
                const auto v = model::ConfigStore().Get(
                    bi.name, L"ExcludeFromTerminateAll", 0, true, true);
                if (v.has_value() && !_wcsicmp(v->c_str(), L"y")) {
                    ++skipped;   // 08-P1-1：全局终止的荣誉键
                    continue;
                }
            }
            targets.push_back(bi.name);
        }
    }

    size_t total = 0;
    for (const std::wstring& b : targets) {
        std::vector<ULONG> pids;
        (void)drv::EnumBoxProcesses(b, false, &pids);   // 计数（失败=0）
        SbieStatus st = SvcCall([&b] {
            model::ProcessRepository repo(nullptr, svc::SvcClient::Instance());
            return repo.KillBox(b);
        });
        if (st == SbieStatus::ERR_SVC_TRANSPORT)
            return FailSvcDown(L"proc kill-all");
        if (st != SbieStatus::OK)
            return OpResult::Fail(st,
                                  L"proc kill-all failed for box '" + b
                                  + L"' (SbieSvc required)");
        total += pids.size();
    }

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"count", json::JsonValue((long long)total));
    data.set(L"boxes", json::JsonValue((long long)targets.size()));
    data.set(L"skipped", json::JsonValue((long long)skipped));
    data.set(L"message",
             json::JsonValue(std::to_wstring(total)
                             + L" process(es) terminated"
                             + (box.empty()
                                    ? L" (" + std::to_wstring(targets.size())
                                          + L" box(es))"
                                          + (skipped
                                                 ? L", "
                                                       + std::to_wstring(skipped)
                                                       + L" box(es) skipped"
                                                         L" (ExcludeFrom"
                                                         L"TerminateAll)"
                                                 : std::wstring())
                                    : L"")));
    return OpResult::Succeed(std::move(data));
}

// proc.suspend / proc.resume（P0-2）：参数 pid；存在性经 QueryProcessEx2
OpResult HProcSuspendResume(const json::JsonValue& params, bool suspend)
{
    const long long pid = GetInt(params, L"pid", 0);
    if (pid <= 0 || pid > 0xFFFFFFFFLL)
        return OpResult::Fail(SbieStatus::INVALID, L"missing/invalid param 'pid'");
    drv::ProcQuery q;
    if (drv::QueryProcessById((ULONG)pid, &q) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"pid " + std::to_wstring((unsigned long)pid)
                              + L" is not a running sandboxed process");

    SbieStatus st = SvcCall([&] {
        model::ProcessRepository repo(nullptr, svc::SvcClient::Instance());
        return suspend ? repo.Suspend((ULONG)pid) : repo.Resume((ULONG)pid);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(suspend ? L"proc suspend" : L"proc resume");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"proc suspend/resume failed"
                                     L" (SbieSvc required)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"pid", json::JsonValue(pid));
    data.set(L"message",
             json::JsonValue(L"pid " + std::to_wstring((unsigned long)pid)
                             + (suspend ? L" suspended" : L" resumed")));
    return OpResult::Succeed(std::move(data));
}

OpResult HProcSuspend(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    return HProcSuspendResume(params, true);
}

OpResult HProcResume(const json::JsonValue& params,
                     const std::shared_ptr<Connection>&)
{
    return HProcSuspendResume(params, false);
}

// proc.suspendBox / proc.resumeBox（波次 E，08-P2-4；D3 自注待办收口）：
// params {box?}——box 空 = 全局（EnumBoxes 仅启用 box，与 killAll 全局口径
// 一致）；触 SbieSvc 的 SuspendResumeAll 一律经 SvcCall 专职线程。data 与
// client 直连路径同形（box/boxes/suspended/count/message）。
OpResult HProcSuspendBoxImpl(const json::JsonValue& params, bool suspend)
{
    const wchar_t* verb = suspend ? L"suspend-box" : L"resume-box";
    std::wstring box;
    GetStr(params, L"box", &box);   // 可选：空 = 全局

    std::vector<std::wstring> targets;
    if (!box.empty()) {
        model::BoxInfo bi;
        SbieStatus st = GetBoxInfoSafe(box, &bi);
        if (st == SbieStatus::NOT_FOUND)
            return OpResult::Fail(SbieStatus::NOT_FOUND,
                                  L"box '" + box + L"' not found");
        if (st != SbieStatus::OK)
            return OpResult::Fail(st, L"query failed for box: " + box);
        targets.push_back(box);
    } else {
        model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
        for (const model::BoxInfo& bi : repo.EnumBoxes(false))
            targets.push_back(bi.name);
    }

    ULONG total = 0;
    json::JsonValue boxes = json::JsonValue::Array();
    for (const std::wstring& b : targets) {
        std::vector<ULONG> pids;
        if (drv::EnumBoxProcesses(b, false, &pids) != SbieStatus::OK)
            continue;   // 计数（失败=0，与 client 直连同款）
        SbieStatus st = SvcCall([&b, suspend] {
            return svc::SvcClient::Instance().SuspendResumeAll(b, suspend);
        });
        if (st == SbieStatus::ERR_SVC_TRANSPORT)
            return FailSvcDown(verb);
        if (st != SbieStatus::OK)
            return OpResult::Fail(st,
                                  std::wstring(verb) + L" failed for box '"
                                      + b + L"' (SbieSvc required)");
        total += (ULONG)pids.size();
        boxes.pushBack(json::JsonValue(b));
    }

    json::JsonValue data = json::JsonValue::Object();
    if (!box.empty())
        data.set(L"box", json::JsonValue(box));
    data.set(L"boxes", std::move(boxes));
    data.set(L"suspended", json::JsonValue(suspend));
    data.set(L"count", json::JsonValue((long long)total));
    data.set(L"message",
             json::JsonValue(std::wstring(verb) + L": "
                             + std::to_wstring(total) + L" process(es) in "
                             + std::to_wstring(targets.size())
                             + L" box(es)"));
    return OpResult::Succeed(std::move(data));
}

OpResult HProcSuspendBox(const json::JsonValue& params,
                         const std::shared_ptr<Connection>&)
{
    return HProcSuspendBoxImpl(params, true);
}

OpResult HProcResumeBox(const json::JsonValue& params,
                        const std::shared_ptr<Connection>&)
{
    return HProcSuspendBoxImpl(params, false);
}

// box.setEnabled（P0-3）：参数 name/enabled/password?；Enabled=y/n
// （Model SetEnabled 的 pw 等价——IniSetSetting 密码透传）
OpResult HBoxSetEnabled(const json::JsonValue& params,
                        const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    const json::JsonValue* on = params.find(L"enabled");
    const bool enabled = on && on->type() == json::JsonValue::Type::Bool
        ? on->asBool() : true;

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    const std::wstring pw = ResolvePasswordParam(params);
    st = SvcCall([&] {
        return svc::SvcClient::Instance().IniSetSetting(
            name, L"Enabled", enabled ? L"y" : L"n",
            svc::SvcClient::SetMode::Update, true, pw);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box enable/disable");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box enable/disable failed"
                              + PasswordHintText(st, pw));

    // 写后回读校验（验收回传加固，04 §13）：IniSetSetting(refresh=true) 的
    // LPC 回复在 SbieSvc 内部已完成 SaveIni + SbieApi_ReloadConf（同步），
    // 故此刻驱动缓存必含新值；读回不符 = 写路径异常，显式报错而非静默
    // "成功"——把"disable 静默未生效"这类问题变成可见失败。
    {
        auto v = model::ConfigStore().Get(name, L"Enabled", 0, true, true);
        const bool nowOn = v.has_value() && (*v == L"y" || *v == L"Y");
        if (nowOn != enabled)
            return OpResult::Fail(SbieStatus::GENERIC,
                                  L"box enable/disable verification failed:"
                                  L" Enabled is still '"
                                  + (v.has_value() ? *v : std::wstring(L""))
                                  + L"' after write (ini/driver cache"
                                    L" mismatch)");
    }
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message",
             json::JsonValue(enabled ? L"enabled" : L"disabled"));
    return OpResult::Succeed(std::move(data));
}

// box.clean（P0-4）：清空 FileRoot 内容、保留目录与 ini 节。
// 语义（06 §P0-4 建议，与 box delete 一致）：NeverDelete → ACCESS_DENIED；
// 有活动进程 → BOX_BUSY（提示 proc kill-all）；纯文件操作（worker 线程，
// 不触 SbieSvc）。波次 A（07-P0-1）：清空前逐条执行 OnBoxDelete 触发器
//（no_triggers=true 跳过——--no-triggers 逃生旗标）；守护监视器的
// AutoDelete 行为复用同一执行器（Guardian.cpp ExecuteBoxPurge）。
OpResult HBoxClean(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    const bool noTriggers = GetBool(params, L"no_triggers", false);

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    if (NeverDeleteProtected(name))
        return OpResult::Fail(SbieStatus::ACCESS_DENIED,
                              L"box '" + name + L"' is protected"
                              L" (NeverDelete=y)");
    if (bi.hasProcesses)
        return OpResult::Fail(SbieStatus::BOX_BUSY,
                              L"box '" + name + L"' has running processes;"
                              L" terminate them first (proc kill-all /"
                              L" proc kill)");

    st = PurgeBoxContents(name, false /*CleanContents*/, noTriggers);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box clean failed (file error; retry"
                              L" after handles release)");
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"cleaned"));
    return OpResult::Succeed(std::move(data));
}

// cfg.unset（P0-6）：参数 setting/section/index?/refresh?/password?。
// --index = 删该值（List→重放）；缺省 = 删整 setting（0x1814 空 value）
OpResult HCfgUnset(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring setting;
    if (!GetStr(params, L"setting", &setting) || setting.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'setting'");
    std::wstring section = L"GlobalSettings";
    GetStr(params, L"section", &section);
    const long long index = GetInt(params, L"index", -1);
    const bool refresh = GetBool(params, L"refresh", true);
    const std::wstring pw = ResolvePasswordParam(params);

    // 整 setting 删除的存在性前置（rc 5；--index 越界由 Delete 内部判）
    model::ConfigStore cfgRead;
    if (index < 0 && cfgRead.GetList(section, setting).empty())
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"setting not found: [" + section + L"] "
                              + setting);

    SbieStatus st = SvcCall([&] {
        model::ConfigStore cfg;
        return cfg.Delete(section, setting,
                          index >= 0 ? std::optional<ULONG>((ULONG)index)
                                     : std::nullopt,
                          refresh, pw);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"cfg unset");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"cfg unset failed" + PasswordHintText(st, pw));
    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(L"unset"));
    return OpResult::Succeed(std::move(data));
}

// cfg.lock（P0-7）：SET_PASSWORD（0x1807）。参数 new_password + password
// （旧密码，缺省空——变更已设密码需提供）；空新密码 = 解除锁定
OpResult HCfgLock(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring newPw;
    if (!GetStr(params, L"new_password", &newPw))
        return OpResult::Fail(SbieStatus::USAGE,
                              L"missing param 'new_password'");
    if (newPw.size() > 64)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"password exceeds 64 characters");
    const std::wstring oldPw = ResolvePasswordParam(params);
    if (oldPw.size() > 64)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"password exceeds 64 characters");

    SbieStatus st = SvcCall([&] {
        return svc::SvcClient::Instance().SetPassword(oldPw, newPw);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"cfg lock");
    if (st == SbieStatus::ACCESS_DENIED)
        return OpResult::Fail(SbieStatus::ACCESS_DENIED,
                              L"wrong old password (pass --password"
                              L" <current-pw> or set SBIE_PASS)");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"cfg lock failed");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message",
             json::JsonValue(newPw.empty() ? L"config lock removed"
                                           : L"config locked"));
    return OpResult::Succeed(std::move(data));
}

// cfg.unlock（P0-8）：TEST_PASSWORD（0x1808）验证。连接级缓存语义
//（06 §P0-8 建议）：CLI 一命令一进程，密码不缓存——unlock 仅验证，后续写
// 逐次携带 --password/SBIE_PASS；解除锁定 = cfg.lock 空新密码 + 旧密码
OpResult HCfgUnlock(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring pw;
    if (!GetStr(params, L"password", &pw) || pw.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'password'");
    if (pw.size() > 64)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"password exceeds 64 characters");

    if (!model::ConfigStore().Locked()) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"locked", json::JsonValue(false));
        data.set(L"message", json::JsonValue(L"config is not locked"));
        return OpResult::Succeed(std::move(data));
    }

    SbieStatus st = SvcCall([&] {
        return svc::SvcClient::Instance().TestPassword(pw);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"cfg unlock");
    if (st == SbieStatus::ACCESS_DENIED)
        return OpResult::Fail(SbieStatus::ACCESS_DENIED,
                              L"wrong password (unlock verifies only; to"
                              L" remove the lock use: cfg lock \"\""
                              L" --password <pw>)");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"cfg unlock failed");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"locked", json::JsonValue(true));
    data.set(L"message",
             json::JsonValue(L"password verified (not cached; subsequent"
                             L" writes need --password or SBIE_PASS)"));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// box size / recover 波次 op（06 §P0-5/P0-12；docs/04 §14）。
//   * box.size / recover.list / recover.copy：纯文件系统操作（worker 线程
//     直执，不触 SbieSvc——ConfigStore 读走驱动缓存）；
//   * recover.add：RecoverFolder 写值经 SbieSvc（SvcProxy 专职线程）。
// 真实文件 IO（recover 的拷出）在 server 侧完成——"server 负责所有真实
// 操作"模型；无 server 时 client 直连 Model 等价执行（两侧同 Recovery 实现）。
// ---------------------------------------------------------------------------

// 字节数人性化（cli\Commands\BoxProcCommands.h::FormatHumanBytes 同构副本，
// 模块隔离 04 §1）
std::wstring FormatHumanBytes(unsigned long long bytes)
{
    if (bytes < 1024)
        return std::to_wstring(bytes) + L" B";
    static const wchar_t* const units[] = { L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    size_t u = 0;
    do {
        v /= 1024.0;
        ++u;
    } while (v >= 1024.0 && u < 4);
    wchar_t buf[32];
    if (v < 10.0)
        swprintf_s(buf, L"%.2f %s", v, units[u - 1]);
    else if (v < 100.0)
        swprintf_s(buf, L"%.1f %s", v, units[u - 1]);
    else
        swprintf_s(buf, L"%.0f %s", v, units[u - 1]);
    return buf;
}

// box.size（P0-5）：ScanBoxSize 递归统计 FileRoot（纯文件读，worker 线程）。
// 同步执行——契约的"后台线程+进度"（04 §4.3 设计）在 CLI 一问一答模型下
// 无呈现面（无流式进度 op），测试沙箱量级毫秒级完成；超大沙箱的未来扩展
// 见 docs/04 §14 遗留。
OpResult HBoxSize(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    model::BoxUsageStats usage;
    st = model::ScanBoxSize(bi.fileRoot, &usage, nullptr);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"box size scan failed (file error)");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"name", json::JsonValue(name));
    data.set(L"bytes", json::JsonValue((long long)usage.totalBytes));
    data.set(L"human", json::JsonValue(FormatHumanBytes(usage.totalBytes)));
    data.set(L"files", json::JsonValue((long long)usage.files));
    data.set(L"dirs", json::JsonValue((long long)usage.dirs));
    return OpResult::Succeed(std::move(data));
}

// recover.list（P0-12）：行集（index/sandbox_path/box_path/target_path/
// size/size_human），排序与 client 直连一致（RecoveryManager::List）
OpResult HRecoverList(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    json::JsonValue rows = json::JsonValue::Array();
    std::vector<model::RecoverEntry> es = model::RecoveryManager(bi).List();
    for (size_t i = 0; i < es.size(); ++i) {
        const model::RecoverEntry& e = es[i];
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"index", json::JsonValue((long long)i));
        r.set(L"sandbox_path", json::JsonValue(e.sandboxPath));
        r.set(L"box_path", json::JsonValue(e.boxPath));
        r.set(L"target_path", json::JsonValue(e.targetPath));
        r.set(L"size", json::JsonValue((long long)e.size));
        r.set(L"size_human", json::JsonValue(FormatHumanBytes(e.size)));
        rows.pushBack(std::move(r));
    }
    return OpResult::Succeed(std::move(rows));
}

// recover.copy（P0-12）：真实文件 IO 在 server 侧（架构决策 docs/04 §14）。
// params：name / paths[]（沙箱绝对路径，client 经 recover.list 解析）/ to?
// / overwrite? / move?（波 B 07-P1-3：CopyFileW 成功后 DeleteFileW 沙箱源）
// / on_file_recovery?（波 B 07-P1-3：恢复前执行箱键 OnFileRecovery 的检查器
// 命令，非零退出 = 拒绝该文件——跳过并列出；缺省 true，--no-check 关）。
// 写语义（client retry=false）
OpResult HRecoverCopy(const json::JsonValue& params,
                      const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::vector<std::wstring> paths;
    if (const json::JsonValue* a = params.find(L"paths");
        a && a->isArray()) {
        for (const json::JsonValue& p : a->items())
            if (p.isString())
                paths.push_back(p.asString());
    }
    if (paths.empty())
        return OpResult::Fail(SbieStatus::INVALID,
                              L"missing/empty param 'paths'");
    std::wstring to;
    GetStr(params, L"to", &to);   // 可选：空 = 恢复到原位
    const bool overwrite = GetBool(params, L"overwrite", false);
    const bool move = GetBool(params, L"move", false);
    const bool onFileRecovery = GetBool(params, L"on_file_recovery", true);

    model::BoxInfo bi;
    SbieStatus st = GetBoxInfoSafe(name, &bi);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"query failed for box: " + name);

    model::RecoverCopyOptions opts;
    opts.move = move;
    opts.runCheckers = onFileRecovery;
    model::RecoverCopyOutcome outcome;
    st = model::RecoveryManager(bi).CopyEx(paths, to, overwrite, opts,
                                           &outcome);
    if (st != SbieStatus::OK) {
        std::wstring msg = L"recover copy failed at: " + outcome.failedPath;
        if (outcome.win32Error == ERROR_FILE_EXISTS)
            msg += L" (target exists; use --overwrite to replace)";
        else if (outcome.win32Error == ERROR_SHARING_VIOLATION)
            msg += L" (source or target is in use)";
        else if (outcome.win32Error == ERROR_ACCESS_DENIED)
            msg += L" (access denied)";
        return OpResult::Fail(st, msg);
    }

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"copied", json::JsonValue((long long)outcome.copiedFiles));
    data.set(L"bytes", json::JsonValue((long long)outcome.copiedBytes));
    if (outcome.skippedFiles > 0) {
        data.set(L"skipped",
                 json::JsonValue((long long)outcome.skippedFiles));
        json::JsonValue skipped = json::JsonValue::Array();
        for (const std::wstring& p : outcome.skippedPaths)
            skipped.pushBack(json::JsonValue(p));
        data.set(L"skipped_paths", std::move(skipped));
    }
    data.set(L"message",
             json::JsonValue(std::to_wstring(outcome.copiedFiles)
                             + L" file(s) "
                             + (move ? L"moved" : L"recovered") + L" ("
                             + FormatHumanBytes(outcome.copiedBytes)
                             + L")"
                             + (outcome.skippedFiles > 0
                                    ? L", "
                                          + std::to_wstring(
                                                outcome.skippedFiles)
                                          + L" skipped by OnFileRecovery"
                                    : L"")));
    return OpResult::Succeed(std::move(data));
}

// recover.add（P0-12）：RecoverFolder 追加（经 SbieSvc 写路径）+ 写后回读
//（§13.1 范式；与 client 直连分支同款）
OpResult HRecoverAdd(const json::JsonValue& params,
                     const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring folder;
    if (!GetStr(params, L"folder", &folder) || folder.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'folder'");
    const std::wstring pw = ResolvePasswordParam(params);

    bool enabled = false, exists = false;
    if (drv::IsBoxEnabled(name, &enabled, &exists) != SbieStatus::OK
        || !exists)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");

    std::vector<std::wstring> before =
        model::ConfigStore().GetList(name, L"RecoverFolder", false, false);
    SbieStatus st = SvcCall(
        [&] { return model::RecoverAddFolder(name, folder, pw); });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"recover add");
    if (st == SbieStatus::INVALID)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"folder must be a %var%, DOS (X:\\...), UNC"
                              L" (\\\\server\\share) or NT (\\Device\\...)"
                              L" path");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"recover add failed"
                              + PasswordHintText(st, pw));

    // 写后回读：RecoverFolder 值数应增加（Add 模式追加在值表尾，
    // after.back() 即新值；膨胀形态 = 驱动展开后的路径）
    std::vector<std::wstring> after =
        model::ConfigStore().GetList(name, L"RecoverFolder", false, false);
    if (after.size() <= before.size())
        return OpResult::Fail(SbieStatus::GENERIC,
                              L"recover add verification failed:"
                              L" RecoverFolder count did not increase after"
                              L" write");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"value", json::JsonValue(after.back()));
    data.set(L"message",
             json::JsonValue(L"recover folder added: " + after.back()
                             + L" (" + folder + L")"));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// 波次 B op（07-P1-1/2/4，docs/04 §17）：沙箱复制 / 导出 / 导入。
//   * box.copy / box.import：节写入经 SvcProxy（CopyBox/ImportBox 内触
//     SbieSvc）+ 文件 IO（专职线程上直执文件操作无碍）；
//   * box.export：纯驱动读 + 文件 IO（worker 线程直执，无 SvcCall）。
// data.message 与 client 直连路径输出文案一致。
// ---------------------------------------------------------------------------

// box.copy：params src/dst/content(bool)/password?
OpResult HBoxCopy(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    std::wstring src;
    if (!GetStr(params, L"src", &src) || src.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'src'");
    std::wstring dst;
    if (!GetStr(params, L"dst", &dst) || dst.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'dst'");
    if (_wcsicmp(src.c_str(), dst.c_str()) == 0)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"source and destination are the same");
    if (model::BoxRepository::ValidateName(dst) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"invalid box name '" + dst + L"' (max 38 chars"
                              L" of A-Z a-z 0-9 _; reserved words excluded)");
    const bool content = GetBool(params, L"content", false);
    const std::wstring pw = ResolvePasswordParam(params);

    model::TransferStats stats;
    SbieStatus st = SvcCall([&] {
        return model::CopyBox(src, dst, content, pw, &stats);
    });
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + src + L"' not found, or '" + dst
                              + L"' already exists");
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box copy");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box copy failed"
                              + PasswordHintText(st, pw));

    json::JsonValue data = json::JsonValue::Object();
    if (content) {
        data.set(L"files", json::JsonValue((long long)stats.files));
        data.set(L"bytes", json::JsonValue((long long)stats.bytes));
    }
    data.set(L"message",
             json::JsonValue(L"box '" + src + L"' copied to '" + dst + L"'"
                             + (content
                                    ? L" (" + std::to_wstring(stats.files)
                                          + L" file(s), "
                                          + FormatHumanBytes(stats.bytes)
                                          + L")"
                                    : L"")));
    return OpResult::Succeed(std::move(data));
}

// box.export：params name/to/archive(bool)。纯文件 IO（worker 线程）。
OpResult HBoxExport(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    std::wstring to;
    if (!GetStr(params, L"to", &to) || to.empty())
        return OpResult::Fail(SbieStatus::USAGE, L"missing param 'to'");
    const bool archive = GetBool(params, L"archive", false);

    model::TransferStats stats;
    std::wstring detail;
    SbieStatus st = model::ExportBox(name, to, archive, &stats, &detail);
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box export failed"
                              + (detail.empty() ? L"" : L": " + detail));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"files", json::JsonValue((long long)stats.files));
    data.set(L"dirs", json::JsonValue((long long)stats.dirs));
    data.set(L"bytes", json::JsonValue((long long)stats.bytes));
    data.set(L"archive", json::JsonValue(archive));
    data.set(L"message",
             json::JsonValue(L"box '" + name + L"' exported to " + to
                             + L" (" + std::to_wstring(stats.files)
                             + L" file(s), "
                             + FormatHumanBytes(stats.bytes) + L")"));
    return OpResult::Succeed(std::move(data));
}

// box.import：params path/name/archive(bool)/password?
OpResult HBoxImport(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring path;
    if (!GetStr(params, L"path", &path) || path.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'path'");
    std::wstring name;
    if (!GetStr(params, L"name", &name) || name.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"missing param 'name'");
    const bool archive = GetBool(params, L"archive", false);
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"invalid box name '" + name + L"' (max 38"
                              L" chars of A-Z a-z 0-9 _; reserved words"
                              L" excluded)");
    const std::wstring pw = ResolvePasswordParam(params);

    model::TransferStats stats;
    std::wstring detail;
    SbieStatus st = SvcCall([&] {
        return model::ImportBox(path, name, archive, pw, &stats, &detail);
    });
    if (st == SbieStatus::NOT_FOUND)
        return OpResult::Fail(SbieStatus::NOT_FOUND,
                              L"box '" + name + L"' already exists, or"
                              L" package not found: " + path);
    if (st == SbieStatus::INVALID)
        return OpResult::Fail(SbieStatus::INVALID,
                              detail.empty() ? L"bad package: " + path
                                             : detail);
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"box import");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st,
                              L"box import failed"
                              + (detail.empty() ? L"" : L": " + detail)
                              + PasswordHintText(st, pw));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"files", json::JsonValue((long long)stats.files));
    data.set(L"dirs", json::JsonValue((long long)stats.dirs));
    data.set(L"bytes", json::JsonValue((long long)stats.bytes));
    data.set(L"message",
             json::JsonValue(L"box '" + name + L"' imported from " + path
                             + L" (" + std::to_wstring(stats.files)
                             + L" file(s), "
                             + FormatHumanBytes(stats.bytes) + L")"));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// P1 清尾波次 op（06 缺口表 P1-1/P1-3，docs/04 §15）。
// force.set/force.status：直驱动（API_DISABLE_FORCE_PROCESS 经 SbieApi_Ioctl），
// 无 SbieSvc 依赖——唯 ForceDisableSeconds 配置写经 SbieSvc（SvcProxy）。
// ---------------------------------------------------------------------------

// force 禁用时刻追踪（P1-1）：驱动 get 只回 BOOLEAN（Session_IsForceDisabled，
// drv session.c:472-503），剩余秒数由本 server 记录的禁用时刻 +
// ForceDisableSeconds 推算——与 SandMan 托盘倒计时同源语义（Plus 亦为禁用方
// 本地计时）。SandMan/直连 client 触发的禁用无时刻可考 → remaining 为 null。
std::mutex g_forceMtx;
ULONGLONG g_forceDisableTick = 0;      // 0 = 未记录（未知禁用时刻）
long long g_forceWindowSec = 10;

// GlobalSettings\ForceDisableSeconds 读取（缺省 10——drv Conf_Get_Number
// 同缺省；0 = 永不允许禁用）
long long ForceWindowSeconds()
{
    auto v = model::ConfigStore().Get(L"GlobalSettings",
                                      L"ForceDisableSeconds", 0, true, true);
    if (!v.has_value() || v->empty())
        return 10;
    return _wtol(v->c_str());
}

// force.set（P1-1）：params enable(bool)/seconds?(int)/password?。
// enable=true：seconds>0 时先写 ForceDisableSeconds（QSbieAPI DisableForceProcess
// 同序，SbieAPI.cpp:2589-2596），再驱动 set_flag=1；enable=false：驱动 set_flag=0。
SbieStatus ForceSet(bool enable, long long seconds, const std::wstring& pw,
                    long long* windowOut)
{
    if (enable) {
        if (seconds > 0) {
            SbieStatus st = SvcCall([&] {
                wchar_t buf[24];
                swprintf_s(buf, L"%lld", seconds);
                return model::ConfigStore().Set(
                    L"GlobalSettings", L"ForceDisableSeconds", buf, true, pw);
            });
            if (st == SbieStatus::ERR_SVC_TRANSPORT)
                return st;
            if (st != SbieStatus::OK)
                return st;
        }
    }
    ULONG flag = enable ? 1 : 0;
    SbieStatus st = drv::DisableForceProcess(&flag, nullptr);
    if (st != SbieStatus::OK)
        return st;
    const std::lock_guard<std::mutex> lk(g_forceMtx);
    g_forceDisableTick = enable ? GetTickCount64() : 0;
    g_forceWindowSec = ForceWindowSeconds();
    *windowOut = g_forceWindowSec;
    return SbieStatus::OK;
}

OpResult HForceSet(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    const json::JsonValue* en = params.find(L"enable");
    const bool enable = en && en->type() == json::JsonValue::Type::Bool
        ? en->asBool() : true;
    const bool hasSeconds = params.find(L"seconds")
        && params.find(L"seconds")->isInt();
    long long seconds = GetInt(params, L"seconds", -1);
    if (hasSeconds && (seconds < 0 || seconds > 86400))
        return OpResult::Fail(SbieStatus::INVALID,
                              L"'seconds' must be 0 < s <= 86400");
    const std::wstring pw = ResolvePasswordParam(params);

    long long window = 0;
    SbieStatus st = ForceSet(enable, hasSeconds ? seconds : 0, pw, &window);
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"force on (ForceDisableSeconds write)");
    if (st == SbieStatus::ACCESS_DENIED)
        return OpResult::Fail(
            SbieStatus::ACCESS_DENIED,
            L"force set failed"
            + (enable ? std::wstring(L" (ForceDisableAdminOnly=y requires"
                                     L" admin; or config locked)")
                      : PasswordHintText(st, pw)));
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"force set failed (driver ioctl)");

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"disabled", json::JsonValue(enable));
    if (enable)
        data.set(L"window_seconds", json::JsonValue(window));
    data.set(L"message",
             json::JsonValue(enable
                 ? L"force process disabled for "
                   + std::to_wstring(window) + L" second(s)"
                 : std::wstring(L"force process enabled")));
    return OpResult::Succeed(std::move(data));
}

// force.status（P1-1）：data {disabled, window_seconds, remaining(秒|null),
// message}。remaining=null = 禁用由非本 server 进程触发（时刻不可考）。
OpResult HForceStatus(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    ULONG flag = 0;
    SbieStatus st = drv::DisableForceProcess(nullptr, &flag);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"force status query failed (driver ioctl)");

    const bool disabled = flag != FALSE;
    long long window = 0;
    long long remaining = -1;   // -1 = 未知
    {
        const std::lock_guard<std::mutex> lk(g_forceMtx);
        if (g_forceDisableTick != 0) {
            window = ForceWindowSeconds();
            const ULONGLONG elapsed = (GetTickCount64() - g_forceDisableTick)
                                      / 1000;
            remaining = window - (long long)elapsed;
            if (remaining < 0)
                remaining = 0;
        }
    }

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"disabled", json::JsonValue(disabled));
    if (disabled || remaining >= 0) {
        data.set(L"window_seconds", json::JsonValue(
                     remaining >= 0 ? window : ForceWindowSeconds()));
        if (remaining >= 0)
            data.set(L"remaining_seconds", json::JsonValue(remaining));
        else
            data.set(L"remaining_seconds", json::JsonValue()); // null = 未知
    }
    data.set(L"message",
             json::JsonValue(disabled
                 ? (remaining >= 0
                    ? L"force process: disabled (" + std::to_wstring(remaining)
                      + L" second(s) remaining)"
                    : std::wstring(L"force process: disabled"
                                   L" (remaining time unknown; set by another"
                                   L" process)"))
                 : std::wstring(L"force process: normal (force enabled)")));
    return OpResult::Succeed(std::move(data));
}

// ---------------------------------------------------------------------------
// 波次 D2 op（docs/04 §18）：磁盘映像 / RAM 盘 / USB 沙箱运维域。
// img.*/ramdisk.status 经 SvcProxy 转发 SbieSvc MountManager（MSGID_IMBOX_*）；
// usb.* = 配置键读 + Win32 卷枚举（UsbSandbox Model）+ 写路径（ConfigStore）。
// SbieSvc 断连 → FailSvcDown（client 不降级直连重试，读命令例外）。
// ---------------------------------------------------------------------------

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

std::wstring ImDiskHint(SbieStatus st)
{
    if (st == SbieStatus::DRIVER_UNAVAILABLE)
        return L" (ImDisk driver not available)";
    return L"";
}

OpResult HImgList(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    std::vector<std::wstring> roots;
    SbieStatus st = SvcCall([&] {
        return model::EnumMountedRoots(&roots);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"img list");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"img list failed" + ImDiskHint(st));
    json::JsonValue rows = json::JsonValue::Array();
    for (const std::wstring& r : roots) {
        json::JsonValue row = json::JsonValue::Object();
        row.set(L"reg_root", json::JsonValue(r));
        rows.pushBack(std::move(row));
    }
    return OpResult::Succeed(std::move(rows));
}

// img.status：box 缺省 = 全箱行集；带 box = 单箱对象
OpResult HImgStatus(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring box;
    GetStr(params, L"box", &box);
    auto singleJson = [](const model::BoxImageInfo& i) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"box", json::JsonValue(i.box));
        d.set(L"use_file_image", json::JsonValue(i.useFileImage));
        d.set(L"use_ram_disk", json::JsonValue(i.useRamDisk));
        d.set(L"confidential",
              json::JsonValue(i.confidential ? L"confidential"
                              : i.lessConfidential ? L"less" : L"none"));
        d.set(L"enable_efs", json::JsonValue(i.enableEfs));
        d.set(L"force_protection_on_mount",
              json::JsonValue(i.forceProtectionOnMount));
        d.set(L"image_file", json::JsonValue(i.imageFile));
        d.set(L"image_exists", json::JsonValue(i.imageExists));
        d.set(L"image_bytes", json::JsonValue((long long)i.imageBytes));
        d.set(L"mounted", i.mount.known
                              ? json::JsonValue(i.mount.mounted)
                              : json::JsonValue());
        d.set(L"mount", MountJson(i.mount));
        return d;
    };
    if (!box.empty()) {
        model::BoxImageInfo info;
        SbieStatus st = SvcCall([&] {
            return model::QueryBoxImage(box, &info);
        });
        if (st == SbieStatus::ERR_SVC_TRANSPORT)
            return FailSvcDown(L"img status");
        if (st != SbieStatus::OK)
            return OpResult::Fail(st, L"box '" + box + L"' not found");
        return OpResult::Succeed(singleJson(info));
    }
    // 全箱（EnumBoxImages 内含 QUERY——SvcProxy 内执行）
    std::vector<model::BoxImageInfo> all;
    SbieStatus st = SvcCall([&] {
        all = model::EnumBoxImages();
        return SbieStatus::OK;
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"img status");
    json::JsonValue rows = json::JsonValue::Array();
    for (const model::BoxImageInfo& i : all)
        rows.pushBack(singleJson(i));
    return OpResult::Succeed(std::move(rows));
}

OpResult HImgCreate(const json::JsonValue& params,
                    const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"'box' is required");
    long long sizeKb = GetInt(params, L"size_kb", 0);
    if (sizeKb < 256ll * 1024)
        return OpResult::Fail(SbieStatus::INVALID,
                              L"'size_kb' must be >= 262144 (256 MB)");
    if (sizeKb > 64ull * 1024 * 1024 * 1024)
        return OpResult::Fail(SbieStatus::INVALID, L"'size_kb' too large");
    const std::wstring pw = ResolvePasswordParam(params);

    SbieStatus st = SvcCall([&] {
        return model::CreateBoxImage(box, (unsigned long long)sizeKb, pw);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"img create");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"img create failed for box '" + box + L"'"
                              + ImDiskHint(st));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(
                 L"image created for box '" + box + L"' ("
                 + std::to_wstring(sizeKb / 1024) + L" MB"
                 + (pw.empty() ? L"" : L", AES") + L")"));
    return OpResult::Succeed(std::move(data));
}

OpResult HImgMount(const json::JsonValue& params,
                   const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"'box' is required");
    const std::wstring pw = ResolvePasswordParam(params);
    std::optional<bool> protect, adminOnly;
    if (const json::JsonValue* v = params.find(L"protect"); v && v->type() == json::JsonValue::Type::Bool)
        protect = v->asBool();
    if (const json::JsonValue* v = params.find(L"admin_only"); v && v->type() == json::JsonValue::Type::Bool)
        adminOnly = v->asBool();
    const bool autoUnmount = GetBool(params, L"auto_unmount", false);

    SbieStatus st = SvcCall([&] {
        return model::MountBoxImage(box, pw, protect, adminOnly, autoUnmount);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"img mount");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"img mount failed for box '" + box + L"'"
                              + ImDiskHint(st));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message", json::JsonValue(
                 L"box '" + box + L"' image mounted"
                 + (autoUnmount ? L" (auto unmount on box close)" : L"")));
    return OpResult::Succeed(std::move(data));
}

OpResult HImgUnmount(const json::JsonValue& params,
                     const std::shared_ptr<Connection>&)
{
    std::wstring box;
    if (!GetStr(params, L"box", &box) || box.empty())
        return OpResult::Fail(SbieStatus::INVALID, L"'box' is required");

    SbieStatus st = SvcCall([&] {
        return model::UnmountBoxImage(box);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"img unmount");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"img unmount failed for box '" + box + L"'"
                              + (st == SbieStatus::NOT_FOUND
                                     ? L" (root not mounted)" : L""));

    json::JsonValue data = json::JsonValue::Object();
    data.set(L"message",
             json::JsonValue(L"box '" + box + L"' image unmounted"));
    return OpResult::Succeed(std::move(data));
}

OpResult HRamDiskStatus(const json::JsonValue&,
                        const std::shared_ptr<Connection>&)
{
    model::RamDiskInfo info;
    SbieStatus st = SvcCall([&] {
        return model::QueryRamDisk(&info);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"ramdisk status");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"ramdisk status failed");

    json::JsonValue d = json::JsonValue::Object();
    d.set(L"size_kb", json::JsonValue((long long)info.sizeKb));
    d.set(L"size_human", json::JsonValue(
              info.sizeKb ? FormatHumanBytes(info.sizeKb * 1024)
                          : std::wstring(L"(not configured)")));
    d.set(L"size_below_minimum", json::JsonValue(info.sizeBelowMinimum));
    d.set(L"letter", json::JsonValue(
              info.letter.empty() ? L"(auto)" : info.letter));
    json::JsonValue boxes = json::JsonValue::Array();
    for (const std::wstring& b : info.boxes)
        boxes.pushBack(json::JsonValue(b));
    d.set(L"boxes", std::move(boxes));
    d.set(L"mounted", info.mount.known
                          ? json::JsonValue(info.mount.mounted)
                          : json::JsonValue());
    d.set(L"mount", MountJson(info.mount));
    return OpResult::Succeed(std::move(d));
}

OpResult HUsbStatus(const json::JsonValue&, const std::shared_ptr<Connection>&)
{
    // 键面为驱动缓存读（worker 线程安全）；卷枚举纯 Win32
    model::UsbSandboxInfo info;
    SbieStatus st = model::QueryUsbSandbox(&info);
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"usb status failed");

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
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"serial", json::JsonValue(v.serial));
        r.set(L"label", json::JsonValue(v.label));
        json::JsonValue mps = json::JsonValue::Array();
        for (const std::wstring& mp : v.mountPoints)
            mps.pushBack(json::JsonValue(mp));
        r.set(L"mount_points", std::move(mps));
        r.set(L"bus_known", json::JsonValue(v.busKnown));
        r.set(L"on_usb_bus", json::JsonValue(v.onUsbBus));
        r.set(L"taken", json::JsonValue(info.VolumeTaken(v)));
        vols.pushBack(std::move(r));
    }
    d.set(L"volumes", std::move(vols));
    return OpResult::Succeed(std::move(d));
}

OpResult HUsbSync(const json::JsonValue& params,
                  const std::shared_ptr<Connection>&)
{
    const bool dryRun = GetBool(params, L"dry_run", false);
    const std::wstring pw = ResolvePasswordParam(params);

    model::UsbSyncResult res;
    SbieStatus st = SvcCall([&] {
        return model::SyncUsbSandbox(pw, dryRun, &res);
    });
    if (st == SbieStatus::ERR_SVC_TRANSPORT)
        return FailSvcDown(L"usb sync");
    if (st == SbieStatus::INVALID)
        return OpResult::Fail(
            st, L"usb drive sandboxing is disabled"
                L" (enable with: sbie-cli cfg set ForceUsbDrives y)");
    if (st != SbieStatus::OK)
        return OpResult::Fail(st, L"usb sync failed"
                                  + PasswordHintText(st, pw));

    model::UsbSandboxInfo info;
    (void)model::QueryUsbSandbox(&info);   // 箱名（读失败不致命）
    const std::wstring msg = dryRun
        ? L"dry run: " + std::to_wstring(res.added)
              + L" folder(s) would be added, " + std::to_wstring(res.removed)
              + L" removed for box '" + info.sandboxName + L"'"
        : L"usb sandbox '" + info.sandboxName + L"' synced ("
              + std::to_wstring(res.added) + L" added, "
              + std::to_wstring(res.removed) + L" removed"
              + (res.boxCreated ? L", box created" : L"") + L")";
    json::JsonValue d = json::JsonValue::Object();
    d.set(L"message", json::JsonValue(msg));
    d.set(L"box", json::JsonValue(info.sandboxName));
    d.set(L"box_created", json::JsonValue(res.boxCreated));
    d.set(L"dry_run", json::JsonValue(dryRun));
    d.set(L"added", json::JsonValue((long long)res.added));
    d.set(L"removed", json::JsonValue((long long)res.removed));
    json::JsonValue ff = json::JsonValue::Array();
    for (const std::wstring& v : res.written)
        ff.pushBack(json::JsonValue(v));
    d.set(L"force_folders", std::move(ff));
    return OpResult::Succeed(std::move(d));
}

// 未实现 op 的占位函数已在波次 E 退役（tpl 三连收口后注册面无 Stub 残留；
// 未知 op 由 Dispatch 直接回 ERR_NOT_IMPLEMENTED）。

} // namespace

void RegisterBuiltinOps()
{
    auto& reg = Registry();
    if (!reg.empty())
        return;

    // 全局读路径（04 §4.1）
    reg[ipc::kOpStatus]  = HStatus;
    reg[ipc::kOpVersion] = HVersion;
    // box（04 §4.3）——读路径
    reg[ipc::kOpBoxList] = HBoxList;
    reg[ipc::kOpBoxInfo] = HBoxInfo;
    reg[ipc::kOpBoxGet]  = HBoxGet;
    reg[ipc::kOpBoxListSetting] = HBoxListSetting;
    // proc（04 §4.4）——读路径
    reg[ipc::kOpProcList] = HProcList;
    reg[ipc::kOpProcInfo] = HProcInfo;
    // cfg（04 §4.5）——读路径 + reload（直连驱动语义，04 §5）
    reg[ipc::kOpCfgGet] = HCfgGet;
    reg[ipc::kOpCfgListSetting] = HCfgListSetting;
    reg[ipc::kOpCfgPath] = HCfgPath;
    reg[ipc::kOpCfgReload] = HCfgReload;
    // log（04 §4.7）——泵在本 server 内
    reg[ipc::kOpLogDump] = HLogDump;
    reg[ipc::kOpLogWatch] = HLogWatch;
    // trace（04 §20，波 D1）——TracePump 在本 server 内
    reg[ipc::kOpTraceDump] = HTraceDump;
    reg[ipc::kOpTraceWatch] = HTraceWatch;

    // ---- 写路径（server 写路径波次，04 §12；参数对拍表见该节）----
    reg[ipc::kOpBoxCreate]     = HBoxCreate;
    reg[ipc::kOpBoxDelete]     = HBoxDelete;
    reg[ipc::kOpBoxRename]     = HBoxRename;
    reg[ipc::kOpBoxSet]        = HBoxSet;
    reg[ipc::kOpBoxSnapList]   = HBoxSnapList;
    reg[ipc::kOpBoxSnapTake]   = HBoxSnapTake;
    reg[ipc::kOpBoxSnapRemove] = HBoxSnapRemove;
    reg[ipc::kOpBoxSnapSelect] = HBoxSnapSelect;
    reg[ipc::kOpBoxSnapSetInfo]= HBoxSnapSetInfo;
    reg[ipc::kOpProcStart]     = HProcStart;
    reg[ipc::kOpProcKill]      = HProcKill;
    reg[ipc::kOpCfgSet]        = HCfgSet;
    reg[ipc::kOpTplApply]      = HTplApply;
    reg[ipc::kOpTplRevoke]     = HTplRevoke;

    // ---- 补缺波次（06 缺口表 P0-1..P0-9 收口）----
    reg[ipc::kOpProcKillAll]   = HProcKillAll;
    reg[ipc::kOpProcSuspend]   = HProcSuspend;
    reg[ipc::kOpProcResume]    = HProcResume;
    reg[ipc::kOpBoxSetEnabled] = HBoxSetEnabled;
    reg[ipc::kOpBoxClean]      = HBoxClean;
    reg[ipc::kOpCfgUnset]      = HCfgUnset;
    reg[ipc::kOpCfgLock]       = HCfgLock;
    reg[ipc::kOpCfgUnlock]     = HCfgUnlock;

    // ---- box size / recover 波次（06 §P0-5/P0-12，docs/04 §14）----
    reg[ipc::kOpBoxSize]       = HBoxSize;
    reg[ipc::kOpRecoverList]   = HRecoverList;
    reg[ipc::kOpRecoverCopy]   = HRecoverCopy;
    reg[ipc::kOpRecoverAdd]    = HRecoverAdd;

    // ---- P1 清尾波次（06 §P1-1，docs/04 §15；直驱动无 SbieSvc）----
    reg[ipc::kOpForceSet]      = HForceSet;
    reg[ipc::kOpForceStatus]   = HForceStatus;

    // ---- 波次 B（07-P1-2/4，docs/04 §17）----
    reg[ipc::kOpBoxCopy]       = HBoxCopy;
    reg[ipc::kOpBoxExport]     = HBoxExport;
    reg[ipc::kOpBoxImport]     = HBoxImport;

    // ---- D2（磁盘映像/RAM 盘/USB 沙箱运维域，docs/04 §18）----
    reg[ipc::kOpImgList]       = HImgList;
    reg[ipc::kOpImgStatus]     = HImgStatus;
    reg[ipc::kOpImgCreate]     = HImgCreate;
    reg[ipc::kOpImgMount]      = HImgMount;
    reg[ipc::kOpImgUnmount]    = HImgUnmount;
    reg[ipc::kOpRamDiskStatus] = HRamDiskStatus;
    reg[ipc::kOpUsbStatus]     = HUsbStatus;
    reg[ipc::kOpUsbSync]       = HUsbSync;

    // ---- 波次 E（第三轮审计 08 微件收口，docs/04 §22）----
    // tpl 三连真 handler（06 §5 遗留；此前 Stub 降级直连）
    reg[ipc::kOpTplList]       = HTplList;
    reg[ipc::kOpTplInfo]       = HTplInfo;
    reg[ipc::kOpTplCheck]      = HTplCheck;
    // 08-P2-4：proc suspend-box/resume-box 的 server op（D3 自注待办）
    reg[ipc::kOpProcSuspendBox] = HProcSuspendBox;
    reg[ipc::kOpProcResumeBox]  = HProcResumeBox;
    // 08-P2-5：box snapshot default 的读写两形态（族内路径统一）
    reg[ipc::kOpBoxSnapDefault] = HBoxSnapDefault;

    // ---- server.push 专用 op：不接受请求方向 ----
    reg[ipc::kOpLogEvent]      = [](const json::JsonValue&,
                                    const std::shared_ptr<Connection>&) {
        return OpResult::Fail(SbieStatus::INVALID,
                              L"'log.event' is a server-push-only op"); };
    reg[ipc::kOpTraceEvent]    = [](const json::JsonValue&,
                                    const std::shared_ptr<Connection>&) {
        return OpResult::Fail(SbieStatus::INVALID,
                              L"'trace.event' is a server-push-only op"); };
    // 注：kOpServerShutdown 在 ServerMain 工作线程特判（会话校验），不入表。
}

OpResult Dispatch(const std::string& op, const json::JsonValue& params,
                  const std::shared_ptr<Connection>& conn)
{
    auto& reg = Registry();
    auto it = reg.find(op);
    if (it == reg.end())
        return OpResult::Fail(SbieStatus::ERR_NOT_IMPLEMENTED,
                              L"unknown op: " + util::Utf8ToWide(op));
    return it->second(params, conn);
}

} // namespace sbie::server
