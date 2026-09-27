// Sandboxie-OSS — sbie-cli/cli/Commands/doctor_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie doctor（07-P2-4 精简版，波次 D3；docs/04 §19）——纯只读体检报告：
//   SbieDll 加载/ABI、驱动与服务状态（SCM + 设备应答）、安装布局
//   （Templates.ini / Sandboxie.ini / 组件二进制）、常见配置问题
//   （配置锁、FileRootPath 无效/冲突、DefaultBox 缺失、守护键状态、
//   sbie-cli server 连接）。
//
// 07-P2-4 原建议（JS 诊断树）与"无第三方依赖"冲突 → 按任务书做静态体检
// （诊断规则内建，非脚本驱动）。全部探测只读（注册表/文件/SCM/驱动查询，
// 写权限探测 = 打开即关、不落一字节）。
//
// 无 sbie-cli server op（诊断结论应基于本机直接事实，非经 server 转述）。
// 退出码：0 = 无 FAIL（可有 WARN）；1 = 至少一项 FAIL。

#include "BoxProcCommands.h"
#include "../Output.h"
#include "../ServerConnect.h"

#include "Model/Boxes.h"
#include "Model/ConfigStore.h"
#include "Model/Maintenance.h"

#include "Util/Version.h"

#include <windows.h>

#include <cwchar>

namespace sbie::cli {

namespace {

enum class Sev { Ok, Warn, Fail, Info };

const wchar_t* SevText(Sev s)
{
    switch (s) {
    case Sev::Ok:   return L"ok";
    case Sev::Warn: return L"warn";
    case Sev::Fail: return L"FAIL";
    case Sev::Info: return L"info";
    }
    return L"?";
}

struct Doctor {
    util::TablePrinter t;
    json::JsonValue rows = json::JsonValue::Array();
    int fails = 0, warns = 0;

    void Add(Sev sev, const std::wstring& check, const std::wstring& detail)
    {
        if (sev == Sev::Fail)
            ++fails;
        else if (sev == Sev::Warn)
            ++warns;
        t.AddRow({ SevText(sev), check, detail });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"status", json::JsonValue(SevText(sev)));
        r.set(L"check", json::JsonValue(check));
        r.set(L"detail", json::JsonValue(detail));
        rows.pushBack(std::move(r));
    }
};

bool FileExistsW(const std::wstring& p)
{
    const DWORD at = GetFileAttributesW(p.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring DirOfFile(const std::wstring& p)
{
    const size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? L"" : p.substr(0, cut);
}

// 服务注册表 ImagePath（无引号形态）
std::wstring ServiceImagePath(const wchar_t* service)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      (std::wstring(L"SYSTEM\\CurrentControlSet"
                                    L"\\Services\\")
                       + service).c_str(),
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = L"";
    DWORD cb = sizeof(buf) - sizeof(WCHAR);
    const LSTATUS rc = RegQueryValueExW(k, L"ImagePath", nullptr, nullptr,
                                        (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS)
        return L"";
    buf[cb / sizeof(WCHAR)] = L'\0';
    std::wstring v = buf;
    if (!v.empty() && v.front() == L'"') {
        const size_t e = v.find(L'"', 1);
        v = e == std::wstring::npos ? v.substr(1) : v.substr(1, e - 1);
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie doctor
// ---------------------------------------------------------------------------

int CmdDoctor(const CommandContext& ctx)
{
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (local diagnostics)");

    Doctor d;
    d.t.AddColumn(L"STATUS");
    d.t.AddColumn(L"CHECK");
    d.t.AddColumn(L"DETAIL");

    // ---- 1. SbieDll 加载与 ABI -------------------------------------------
    const bool dllLoaded = drv::Loaded()
                           || drv::LoadSbieDll(ctx.opts.sbieDllPath);
    if (dllLoaded) {
        const drv::VersionInfo vi = drv::GetVersion();
        wchar_t abi[16];
        swprintf_s(abi, L"0x%lX", vi.abi);
        d.Add(Sev::Ok, L"sbiedll",
              drv::LoadedFrom() + L" (abi " + abi + L")");
        if (drv::AbiMatches()) {
            d.Add(Sev::Ok, L"sbiedll-abi",
                  L"matches expected baseline 5.73.5 (0x57230)");
        } else {
            d.Add(Sev::Warn, L"sbiedll-abi",
                  L"loaded abi " + std::wstring(abi)
                      + L" != expected 0x57230 (baseline 5.73.5)");
        }
    } else {
        d.Add(Sev::Fail, L"sbiedll",
              L"cannot load SbieDll.dll: " + drv::LastLoadError());
    }

    // ---- 2. 驱动 / 服务（SCM + 设备应答交叉核证） -------------------------
    model::ComponentStatus cs;
    SbieStatus st = model::QueryComponent(model::Component::Driver, &cs);
    if (st != SbieStatus::OK) {
        d.Add(Sev::Warn, L"driver-service", L"SCM query failed");
    } else if (!cs.installed) {
        d.Add(Sev::Fail, L"driver-service", L"SbieDrv not installed");
    } else if (cs.running) {
        const bool alive = dllLoaded && drv::DriverAlive();
        d.Add(alive ? Sev::Ok : Sev::Warn, L"driver-service",
              L"SbieDrv running"
                  + std::wstring(alive ? L" (device responding)"
                                       : L" (device not responding)"));
    } else {
        d.Add(Sev::Fail, L"driver-service",
              L"SbieDrv not running (" + cs.state + L")");
    }

    st = model::QueryComponent(model::Component::Service, &cs);
    if (st != SbieStatus::OK) {
        d.Add(Sev::Warn, L"sbiesvc", L"SCM query failed");
    } else if (!cs.installed) {
        d.Add(Sev::Fail, L"sbiesvc", L"SbieSvc not installed");
    } else if (!cs.running) {
        d.Add(Sev::Fail, L"sbiesvc",
              L"SbieSvc not running (" + cs.state + L")");
    } else {
        svc::SvcClient& svc = svc::SvcClient::Instance();
        std::wstring v;
        ULONG abi = 0;
        if (svc.Connected() && Ok(svc.IniGetVersion(&v, &abi))) {
            wchar_t ab[16];
            swprintf_s(ab, L"0x%lX", abi);
            d.Add(abi == sbie::kExpectedAbi ? Sev::Ok : Sev::Warn, L"sbiesvc",
                  L"running, connected / " + v + L" (abi " + ab + L")");
        } else {
            d.Add(Sev::Warn, L"sbiesvc",
                  L"running but LPC connection failed");
        }
    }

    // ---- 3. 安装布局 ------------------------------------------------------
    const std::wstring svcBin = ServiceImagePath(L"SbieSvc");
    if (svcBin.empty()) {
        d.Add(Sev::Warn, L"svc-binary",
              L"SbieSvc ImagePath not readable (service not registered?)");
    } else if (FileExistsW(svcBin)) {
        d.Add(Sev::Ok, L"svc-binary", svcBin);
    } else {
        d.Add(Sev::Fail, L"svc-binary",
              L"missing: " + svcBin);
    }
    const std::wstring drvBin = ServiceImagePath(L"SbieDrv");
    if (!drvBin.empty())
        d.Add(FileExistsW(drvBin) ? Sev::Ok : Sev::Fail, L"drv-binary",
              FileExistsW(drvBin) ? drvBin : L"missing: " + drvBin);

    std::wstring installDir;
    if (dllLoaded && !drv::LoadedFrom().empty())
        installDir = DirOfFile(drv::LoadedFrom());
    if (!installDir.empty()) {
        const std::wstring tplIni = installDir + L"\\Templates.ini";
        d.Add(FileExistsW(tplIni) ? Sev::Ok : Sev::Warn, L"templates-ini",
              FileExistsW(tplIni) ? tplIni : L"missing: " + tplIni);
    }

    // ---- 4. Sandboxie.ini -------------------------------------------------
    std::wstring iniPath;
    bool iniIsHome = false;
    {
        svc::SvcClient& svc = svc::SvcClient::Instance();
        if (svc.Connected()
            && Ok(svc.IniGetPath(&iniPath, &iniIsHome)) && !iniPath.empty()
            && !FileExistsW(iniPath))
            iniPath.clear();
    }
    if (iniPath.empty()) {
        if (FileExistsW(L"C:\\Windows\\Sandboxie.ini")) {
            iniPath = L"C:\\Windows\\Sandboxie.ini";
        } else if (!installDir.empty()
                   && FileExistsW(installDir + L"\\Sandboxie.ini")) {
            iniPath = installDir + L"\\Sandboxie.ini";
            iniIsHome = true;
        }
    }
    if (iniPath.empty()) {
        d.Add(Sev::Fail, L"sandboxie-ini", L"not found (SbieSvc down and no "
                                           L"fallback location exists)");
    } else {
        d.Add(Sev::Ok, L"sandboxie-ini",
              iniPath + (iniIsHome ? L" (portable/home layout)" : L""));
    }

    // ---- 5. 常见配置问题（驱动缓存读） -----------------------------------
    if (dllLoaded) {
        model::ConfigStore cfg;

        // 5.1 配置锁
        if (cfg.Locked())
            d.Add(Sev::Warn, L"config-lock",
                  L"EditPassword set - writes need --password / SBIE_PASS");

        // 5.2 FileRootPath 盘符有效（展开 %var%、剥 \??\ 前缀后取 "X:\" 形态）
        const auto frp = cfg.Get(L"GlobalSettings", L"FileRootPath", 0, true,
                                 true);
        if (frp.has_value() && !frp->empty()) {
            wchar_t buf[MAX_PATH * 2] = L"";
            std::wstring expanded =
                ExpandEnvironmentStringsW(
                    frp->c_str(), buf,
                    (DWORD)(sizeof(buf) / sizeof(buf[0])))
                    ? std::wstring(buf)
                    : *frp;
            if (expanded.rfind(L"\\??\\", 0) == 0)
                expanded = expanded.substr(4);
            if (expanded.size() >= 3 && expanded[1] == L':'
                && (expanded[2] == L'\\' || expanded[2] == L'/')) {
                const UINT dt = GetDriveTypeW(expanded.substr(0, 3).c_str());
                d.Add(dt != DRIVE_UNKNOWN && dt != DRIVE_NO_ROOT_DIR
                          ? Sev::Ok
                          : Sev::Warn,
                      L"filerootpath",
                      *frp + (dt != DRIVE_UNKNOWN && dt != DRIVE_NO_ROOT_DIR
                                  ? L""
                                  : L" (drive not available)"));
            }
        }

        // 5.3 DefaultBox 存在
        bool enabled = false, exists = false;
        if (Ok(drv::IsBoxEnabled(L"DefaultBox", &enabled, &exists)))
            d.Add(exists ? Sev::Ok : Sev::Warn, L"defaultbox",
                  exists ? L"present" : L"absent (Start.exe default target)");

        // 5.4 箱清单 / FileRootPath 冲突 / 守护键
        std::vector<std::wstring> boxes;
        if (Ok(drv::EnumBoxes(&boxes, false))) {
            size_t disabled = 0;
            std::vector<std::pair<std::wstring, std::wstring>> roots;
            std::wstring guardianBoxes, neverDeleteBoxes;
            for (const auto& b : boxes) {
                bool en = false, ex = false;
                if (!Ok(drv::IsBoxEnabled(b, &en, &ex)) || !ex)
                    continue;
                if (!en)
                    ++disabled;
                std::wstring root;
                if (Ok(drv::QueryBoxPath(b, &root, nullptr, nullptr))) {
                    for (const auto& r : roots) {
                        if (_wcsicmp(r.second.c_str(), root.c_str()) == 0) {
                            d.Add(Sev::Warn, L"box-fileroot",
                                  b + L" and " + r.first
                                      + L" share FileRootPath " + root);
                        }
                    }
                    roots.emplace_back(b, root);
                }
                const auto gd = cfg.Get(b, L"AutoDelete", 0, true, true);
                const auto gr = cfg.Get(b, L"AutoRemove", 0, true, true);
                if ((gd.has_value() && *gd == L"y")
                    || (gr.has_value() && *gr == L"y")) {
                    if (!guardianBoxes.empty())
                        guardianBoxes += L",";
                    guardianBoxes += b;
                }
                const auto nd = cfg.Get(b, L"NeverDelete", 0, true, true);
                if (nd.has_value() && *nd == L"y") {
                    if (!neverDeleteBoxes.empty())
                        neverDeleteBoxes += L",";
                    neverDeleteBoxes += b;
                }
            }
            d.Add(Sev::Ok, L"boxes",
                  std::to_wstring(boxes.size()) + L" total, "
                      + std::to_wstring(disabled) + L" disabled");
            if (!guardianBoxes.empty())
                d.Add(Sev::Info, L"guardian-keys",
                      L"auto delete/remove active: " + guardianBoxes);
            const auto ge = cfg.Get(L"GlobalSettings", L"GuardiansEnabled",
                                    0, true, true);
            if (ge.has_value() && *ge == L"n")
                d.Add(Sev::Info, L"guardian-keys",
                      L"GuardiansEnabled=n (server guardians disabled)");
            if (!neverDeleteBoxes.empty())
                d.Add(Sev::Info, L"neverdelete", neverDeleteBoxes);
        }

        // 5.5 驱动版本 vs 基线
        if (drv::DriverAlive()) {
            const drv::VersionInfo vi = drv::GetVersion();
            d.Add(Sev::Ok, L"driver-version", vi.version);
        }
    }

    // ---- 6. sbie-cli server ------------------------------------------------
    d.Add(srvconn::HasServer() ? Sev::Ok : Sev::Info, L"cli-server",
          srvconn::HasServer() ? L"connected"
                               : L"not running (commands degrade to direct)");

    // ---- 输出 --------------------------------------------------------------
    if (ctx.opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"checks", std::move(d.rows));
        json::JsonValue summary = json::JsonValue::Object();
        summary.set(L"fail", json::JsonValue((long long)d.fails));
        summary.set(L"warn", json::JsonValue((long long)d.warns));
        data.set(L"summary", std::move(summary));
        EmitJsonOk(ctx.opts, data);
        return d.fails ? 1 : 0;
    }
    EmitRows(ctx.opts, d.t, json::JsonValue::Array(), L"no checks");
    if (d.fails)
        Diag(L"doctor: " + std::to_wstring(d.fails) + L" FAIL, "
                + std::to_wstring(d.warns) + L" warn");
    return d.fails ? 1 : 0;
}

void RegisterDoctorCommand()
{
    Commands()["doctor"][""] = CmdDoctor;
}

} // namespace sbie::cli
