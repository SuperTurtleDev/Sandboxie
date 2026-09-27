// Sandboxie-OSS — sbie-cli/cli/Commands/maint_d3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie maint install|uninstall [--driver|--service|--all]（06 P2-7，波次 D3；
// docs/04 §19）——组件装卸（机器级注册，无 IPC op；需管理员）。
//
//   * uninstall：读并回显当前注册参数（ImagePath/DisplayName/Start/Group/
//     Altitude——失败时的手工还原线索）→ 停止组件（模型 StopComponent，
//     幂等）→ KmdUtil delete <name>；
//   * install：KmdUtil install <name> <path> type=… start=… display=… group=…
//     [altitude=…] [msgfile=…]（NSIS 安装器同命令形态，SandboxieVS.nsi:1563-
//     1568；参数优先取"注册表先前值"，缺省用安装目录标准布局）；
//     不自动启动——`maint start` 显式执行。
//   * 装卸均经 GPL core 的 KmdUtil.exe 运行时调用（P1-2 同款；错误路径为
//     其 GUI MessageBox → 15s 超时兜底）。
//
// 实测范围声明：SbieSvc 往返实测；SbieDrv 装卸实现就绪但未实测（驱动
// 级注册删除在共享实测机风险不可控——见 docs/04 §19 遗留）。

#include "BoxProcCommands.h"
#include "../Output.h"

#include "Model/Maintenance.h"

#include <windows.h>

#include <cwchar>

namespace sbie::cli {

namespace {

enum class Target { Driver, Service, All };

bool ParseTarget(const std::vector<std::wstring>& args, Target* out,
                 std::wstring* err)
{
    bool driver = false, service = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == L"--driver")
            driver = true;
        else if (args[i] == L"--service")
            service = true;
        else if (args[i] == L"--all" || args[i] == L"--json"
                 || args[i] == L"--quiet" || args[i] == L"-q")
            ;
        else {
            *err = args[i];
            return false;
        }
    }
    *out = (driver && service) || (!driver && !service)
               ? Target::All
               : driver ? Target::Driver : Target::Service;
    return true;
}

struct RegInfo {
    std::wstring imagePath, displayName, group, altitude, msgfile;
    DWORD start = 0;   // SERVICE_AUTO_START(2)/DEMAND(3)/...
    bool present = false;
};

std::wstring RegQueryString(HKEY key, const wchar_t* value)
{
    wchar_t buf[1024] = L"";
    DWORD cb = sizeof(buf) - sizeof(WCHAR);
    const LSTATUS rc = RegQueryValueExW(key, value, nullptr, nullptr,
                                        (LPBYTE)buf, &cb);
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

RegInfo ReadRegistration(model::Component c)
{
    RegInfo ri;
    const wchar_t* name = c == model::Component::Driver ? L"SbieDrv"
                                                        : L"SbieSvc";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      (std::wstring(L"SYSTEM\\CurrentControlSet"
                                    L"\\Services\\") + name).c_str(),
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return ri;
    ri.present = true;
    ri.imagePath = RegQueryString(k, L"ImagePath");
    ri.displayName = RegQueryString(k, L"DisplayName");
    ri.group = RegQueryString(k, L"Group");
    DWORD v = 0, cb = sizeof(v);
    if (RegQueryValueExW(k, L"Start", nullptr, nullptr, (LPBYTE)&v, &cb)
        == ERROR_SUCCESS)
        ri.start = v;
    RegCloseKey(k);
    if (c == model::Component::Driver) {
        // 过滤驱动的 Altitude 在 Instances\Default 子键
        HKEY ki = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          (std::wstring(L"SYSTEM\\CurrentControlSet"
                                        L"\\Services\\SbieDrv\\Instances"
                                        L"\\Default")).c_str(),
                          0, KEY_QUERY_VALUE, &ki) == ERROR_SUCCESS) {
            ri.altitude = RegQueryString(ki, L"Altitude");
            RegCloseKey(ki);
        }
    }
    // msgfile：安装目录存在 SbieMsg.dll 时附带（NSIS 同款）
    std::wstring dir;
    if (!drv::LoadedFrom().empty()) {
        const size_t cut = drv::LoadedFrom().find_last_of(L'\\');
        if (cut != std::wstring::npos)
            dir = drv::LoadedFrom().substr(0, cut);
    }
    if (!dir.empty()) {
        const std::wstring msg = dir + L"\\SbieMsg.dll";
        if (GetFileAttributesW(msg.c_str()) != INVALID_FILE_ATTRIBUTES)
            ri.msgfile = msg;
    }
    return ri;
}

// KmdUtil.exe <args>（P1-2 RunKmdUtil 同款：RunFromHome 拉起 + 15s 兜底）
SbieStatus RunKmdUtil(const std::wstring& args)
{
    if ((!drv::Loaded() && !drv::LoadSbieDll())
        || !drv::ApiP()->SbieDll_RunFromHome)
        return SbieStatus::ERR_SBIEDLL;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!drv::ApiP()->SbieDll_RunFromHome(L"KmdUtil.exe", args.c_str(),
                                          &si, &pi))
        return SbieStatus::GENERIC;
    CloseHandle(pi.hThread);
    SbieStatus st = SbieStatus::OK;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        if (ec != 0)
            st = SbieStatus::GENERIC;
    } else {
        TerminateProcess(pi.hProcess, (UINT)-1);
        st = SbieStatus::GENERIC;
    }
    CloseHandle(pi.hProcess);
    return st;
}

const wchar_t* ComponentLabel(model::Component c)
{
    return c == model::Component::Driver ? L"driver" : L"service";
}

// ImagePath 里的 \??\ 前缀剥除（KmdUtil install 取裸路径）
std::wstring StripNtPrefix(const std::wstring& p)
{
    if (p.rfind(L"\\??\\", 0) == 0)
        return p.substr(4);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie maint uninstall / install
// ---------------------------------------------------------------------------

int CmdMaintUninstall(const CommandContext& ctx)
{
    Target target = Target::All;
    std::wstring err;
    if (!ParseTarget(ctx.args, &target, &err))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown option: " + err
                         + L"; usage: sbie-cli maint uninstall"
                           L" [--driver|--service|--all]");

    std::vector<model::Component> comps;
    if (target != Target::Driver)
        comps.push_back(model::Component::Service);
    if (target != Target::Service)
        comps.push_back(model::Component::Driver);

    util::TablePrinter t;
    t.AddColumn(L"COMPONENT");
    t.AddColumn(L"RESULT");
    t.AddColumn(L"DETAIL");
    json::JsonValue rows = json::JsonValue::Array();
    int rc = 0;

    for (model::Component c : comps) {
        const wchar_t* name = c == model::Component::Driver ? L"SbieDrv"
                                                            : L"SbieSvc";
        const RegInfo ri = ReadRegistration(c);
        std::wstring result, detail;
        if (!ri.present) {
            result = L"not installed";
        } else {
            // 停止（幂等）→ KmdUtil delete
            const SbieStatus stopSt = model::StopComponent(c);
            const SbieStatus delSt = RunKmdUtil(
                std::wstring(L"delete ") + name);
            if (delSt == SbieStatus::OK)
                result = L"uninstalled";
            else if (delSt == SbieStatus::ACCESS_DENIED
                     || stopSt == SbieStatus::ACCESS_DENIED) {
                result = L"access denied (run as administrator)";
                rc = ToExitCode(SbieStatus::ACCESS_DENIED);
            } else {
                result = L"failed";
                rc = rc ? rc : ToExitCode(SbieStatus::GENERIC);
            }
            // 还原线索（先前注册参数）
            detail = L"was: " + ri.imagePath
                     + (ri.displayName.empty()
                            ? L""
                            : L" display=\"" + ri.displayName + L"\"")
                     + (ri.group.empty() ? L"" : L" group=" + ri.group)
                     + (ri.altitude.empty() ? L""
                                            : L" altitude=" + ri.altitude);
        }
        t.AddRow({ ComponentLabel(c), result, detail });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"component", json::JsonValue(ComponentLabel(c)));
        r.set(L"result", json::JsonValue(result));
        r.set(L"was_image_path", json::JsonValue(ri.imagePath));
        r.set(L"was_display", json::JsonValue(ri.displayName));
        r.set(L"was_group", json::JsonValue(ri.group));
        r.set(L"was_altitude", json::JsonValue(ri.altitude));
        r.set(L"was_start", json::JsonValue((long long)ri.start));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"");
    return rc;
}

int CmdMaintInstall(const CommandContext& ctx)
{
    // --image <path>：显式二进制路径（缺省 = 当前注册值，再缺省安装目录布局）；
    // 先剥离 --image 对（ParseTarget 只认目标旗标）
    std::wstring imageOverride;
    std::vector<std::wstring> args;
    for (size_t i = 0; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--image" && i + 1 < ctx.args.size()) {
            imageOverride = ctx.args[++i];
            continue;
        }
        args.push_back(ctx.args[i]);
    }
    Target target = Target::All;
    std::wstring err;
    if (!ParseTarget(args, &target, &err))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown option: " + err
                         + L"; usage: sbie-cli maint install"
                           L" [--driver|--service|--all] [--image <path>]");

    std::vector<model::Component> comps;
    if (target != Target::Driver)
        comps.push_back(model::Component::Service);
    if (target != Target::Service)
        comps.push_back(model::Component::Driver);

    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    util::TablePrinter t;
    t.AddColumn(L"COMPONENT");
    t.AddColumn(L"RESULT");
    t.AddColumn(L"DETAIL");
    json::JsonValue rows = json::JsonValue::Array();
    int rc = 0;

    for (model::Component c : comps) {
        const wchar_t* name = c == model::Component::Driver ? L"SbieDrv"
                                                            : L"SbieSvc";
        // 先前注册（若在——保留参数语义）+ --image 显式覆盖 + 缺省布局兜底
        RegInfo ri = ReadRegistration(c);
        const bool isDrv = c == model::Component::Driver;
        std::wstring bin = !imageOverride.empty() ? imageOverride
                                                  : StripNtPrefix(ri.imagePath);
        std::wstring detail = !imageOverride.empty() ? L"--image: " : L"";
        if (bin.empty()) {
            // 缺省：SbieDll 加载目录下的标准二进制名
            std::wstring dir;
            const std::wstring from = drv::LoadedFrom();
            const size_t cut = from.find_last_of(L'\\');
            if (cut != std::wstring::npos)
                dir = from.substr(0, cut);
            bin = dir + (isDrv ? L"\\SbieDrv.sys" : L"\\SbieSvc.exe");
            detail += L"defaults: ";
        }
        if (GetFileAttributesW(bin.c_str()) == INVALID_FILE_ATTRIBUTES) {
            t.AddRow({ ComponentLabel(c), L"failed",
                       L"binary not found: " + bin });
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"component", json::JsonValue(ComponentLabel(c)));
            r.set(L"result", json::JsonValue(L"failed"));
            r.set(L"detail", json::JsonValue(L"binary not found: " + bin));
            rows.pushBack(std::move(r));
            rc = rc ? rc : ToExitCode(SbieStatus::NOT_FOUND);
            continue;
        }

        // KmdUtil install 参数（NSIS 形态；start 保留先前值，缺省
        // svc=auto / drv=demand）
        const DWORD start = ri.start;
        std::wstring kmdArgs = std::wstring(L"install ") + name + L" \"" + bin
            + L"\"";
        if (isDrv)
            kmdArgs += L" type=kernel";
        else
            kmdArgs += L" type=own";
        kmdArgs += (start == 2 ? L" start=auto"
                 : start == 4 ? L" start=disabled"
                 : !isDrv     ? L" start=auto"
                              : L" start=demand");
        if (!ri.displayName.empty())
            kmdArgs += L" display=\"" + ri.displayName + L"\"";
        else if (!isDrv)
            kmdArgs += L" display=\"Sandboxie Service\"";
        if (!isDrv && !ri.group.empty())
            kmdArgs += L" group=" + ri.group;
        else if (!isDrv)
            kmdArgs += L" group=UIGroup";
        if (!ri.altitude.empty())
            kmdArgs += L" altitude=" + ri.altitude;
        else if (isDrv)
            kmdArgs += L" altitude=389000";   // NSIS FILTER_ALTITUDE 基线
        if (!ri.msgfile.empty())
            kmdArgs += L" msgfile=\"" + ri.msgfile + L"\"";

        const SbieStatus st = RunKmdUtil(kmdArgs);
        std::wstring result;
        if (st == SbieStatus::OK) {
            result = L"installed";
        } else if (st == SbieStatus::ACCESS_DENIED) {
            result = L"access denied (run as administrator)";
            rc = ToExitCode(st);
        } else {
            result = L"failed";
            rc = rc ? rc : ToExitCode(st);
        }
        t.AddRow({ ComponentLabel(c), result, detail + bin });
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"component", json::JsonValue(ComponentLabel(c)));
        r.set(L"result", json::JsonValue(result));
        r.set(L"image_path", json::JsonValue(bin));
        rows.pushBack(std::move(r));
    }
    EmitRows(ctx.opts, t, rows, L"");
    return rc;
}

} // namespace sbie::cli
