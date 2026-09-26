// Sandboxie-OSS — SbieCore/Model/Maintenance.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（01-license-map §2 允许范围）：
//   - QSbieAPI SbieUtils.cpp GetServiceStatus（LGPL-2.1，仅参考）：SCM 三步
//     查询与 GetServiceStatus 返回值约定（-1 错 / 0 未安装 / 其余=状态码）；
//   - GPL core 的 KmdUtil.exe 仅作运行时外部进程调用（install\kmdutil
//     kmdutil.c 的命令行形态：`KmdUtil.exe start|stop <name>`），未复制其代码。

#include "Maintenance.h"
#include "../DriverApi/DriverApi.h"

#include <windows.h>

#include <cwchar>

namespace sbie::model {

namespace {

// 服务注册名（core common/my_version.h:63/66；SCM 识别用）
constexpr const wchar_t* kDriverName  = L"SbieDrv";
constexpr const wchar_t* kServiceName = L"SbieSvc";

const wchar_t* ComponentName(Component c)
{
    return c == Component::Driver ? kDriverName : kServiceName;
}

// QSbieAPI GetServiceStatus 同款约定：-1=SCM/查询失败，0=未安装，
// 其余 = SERVICE_* 当前态（dwCurrentState）
int ServiceState(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                    SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm)
        return -1;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : -1;
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD need = 0;
    BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                   (LPBYTE)&ssp, sizeof(ssp), &need);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? (int)ssp.dwCurrentState : -1;
}

const wchar_t* StateName(int state)
{
    switch (state) {
    case SERVICE_RUNNING:       return L"running";
    case SERVICE_STOPPED:       return L"stopped";
    case SERVICE_START_PENDING: return L"start_pending";
    case SERVICE_STOP_PENDING:  return L"stop_pending";
    case SERVICE_PAUSED:        return L"paused";
    case 0:                     return L"not_installed";
    default:                    return L"unknown";
    }
}

// 轮询等待服务状态落定（stop 异步：ControlService 返回后多为 STOP_PENDING）
bool WaitServiceState(const wchar_t* name, DWORD want, ULONGLONG ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                        SC_MANAGER_ENUMERATE_SERVICE);
        if (!scm)
            return false;
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
        if (!svc) {
            CloseServiceHandle(scm);
            return false;
        }
        SERVICE_STATUS_PROCESS ssp{};
        DWORD need = 0;
        BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                       (LPBYTE)&ssp, sizeof(ssp), &need);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (!ok)
            return false;
        if (ssp.dwCurrentState == want)
            return true;
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(200);
    }
}

// KmdUtil.exe <verb> <name>（SbieDll_RunFromHome 从安装目录拉起；等待退出，
// 退出码 0 = 成功）。KmdUtil 的错误路径是 MessageBox（GUI）——超时上限
// 15s 兜底防无人值守挂死。
SbieStatus RunKmdUtil(const wchar_t* verb, const wchar_t* name)
{
    if (!drv::Loaded() && !drv::LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (!drv::ApiP()->SbieDll_RunFromHome)
        return SbieStatus::ERR_SBIEDLL;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring args = std::wstring(verb) + L" " + name;
    if (!drv::ApiP()->SbieDll_RunFromHome(L"KmdUtil.exe", args.c_str(),
                                          &si, &pi)) {
        return SbieStatus::GENERIC;
    }
    CloseHandle(pi.hThread);
    SbieStatus st = SbieStatus::OK;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        if (ec != 0)
            st = SbieStatus::GENERIC;   // KmdUtil 报错（含其 GUI 弹窗路径）
    } else {
        TerminateProcess(pi.hProcess, (UINT)-1);   // 超时兜底（防 MessageBox 挂死）
        st = SbieStatus::GENERIC;
    }
    CloseHandle(pi.hProcess);
    return st;
}

} // namespace

SbieStatus QueryComponent(Component c, ComponentStatus* out)
{
    const int state = ServiceState(ComponentName(c));
    out->installed = state != 0 && state != -1;
    out->running = state == SERVICE_RUNNING;
    out->state = StateName(state);
    return state == -1 ? SbieStatus::GENERIC : SbieStatus::OK;
}

SbieStatus StartComponent(Component c)
{
    if (c == Component::Driver) {
        const int state = ServiceState(kDriverName);
        if (state == SERVICE_RUNNING)
            return SbieStatus::OK;              // 幂等
        if (state == 0)
            return SbieStatus::NOT_FOUND;       // 未安装（装卸归 P2-7）
        return RunKmdUtil(L"start", kDriverName);
    }

    const int state = ServiceState(kServiceName);
    if (state == SERVICE_RUNNING)
        return SbieStatus::OK;
    if (state == 0)
        return SbieStatus::NOT_FOUND;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm)
        return SbieStatus::ACCESS_DENIED;       // 非提升：rc 6
    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return SbieStatus::ACCESS_DENIED;
    }
    BOOL ok = StartServiceW(svc, 0, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok && err != ERROR_SERVICE_ALREADY_RUNNING) {
        // 典型：ERROR_SERVICE_DISABLED（启动类型禁用）
        return err == ERROR_ACCESS_DENIED ? SbieStatus::ACCESS_DENIED
                                          : SbieStatus::GENERIC;
    }
    // 启动异步：等待 RUNNING（SbieSvc 秒级；超时不视为失败——
    // 状态以 maint status 复核）
    (void)WaitServiceState(kServiceName, SERVICE_RUNNING, 15000);
    return SbieStatus::OK;
}

SbieStatus StopComponent(Component c)
{
    if (c == Component::Driver)
        return RunKmdUtil(L"stop", kDriverName);   // 含驱动专属卸载序列

    const int state = ServiceState(kServiceName);
    if (state == 0)
        return SbieStatus::NOT_FOUND;
    if (state == SERVICE_STOPPED)
        return SbieStatus::OK;                      // 幂等

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm)
        return SbieStatus::ACCESS_DENIED;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return SbieStatus::ACCESS_DENIED;
    }
    SERVICE_STATUS ss{};
    BOOL ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    const DWORD err = ok ? 0 : GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok && err != ERROR_SERVICE_NOT_ACTIVE)
        return err == ERROR_ACCESS_DENIED ? SbieStatus::ACCESS_DENIED
                                          : SbieStatus::GENERIC;
    // 停止异步：等待 STOPPED（SbieSvc 需排空 LPC 客户端；超时回 GENERIC——
    // 维持"报告即事实"，由 maint status 复核）
    return WaitServiceState(kServiceName, SERVICE_STOPPED, 15000)
               ? SbieStatus::OK
               : SbieStatus::GENERIC;
}

} // namespace sbie::model
