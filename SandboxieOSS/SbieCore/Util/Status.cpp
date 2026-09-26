// Sandboxie-OSS — SbieCore/Util/Status.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// NTSTATUS 常数取自 Windows SDK 10.0.26100.0 shared/ntstatus.h（实机核对，
// 解除 02-driver-api.md §7.3 的 TODO-VERIFY：STATUS_SERVER_DISABLED == 0xC0000080）。

#include "Status.h"

#include <map>
#include <cstdio>
#include <cwchar>

namespace sbie {

SbieStatus FromNtStatus(LONG ntstatus)
{
    // 告警区间（正 NTSTATUS 但非 0）与 0 一律成功语义
    if (ntstatus >= 0)
        return SbieStatus::OK;

    switch ((unsigned long)ntstatus) {
    case 0xC0000022L: // STATUS_ACCESS_DENIED
        return SbieStatus::ACCESS_DENIED;
    case 0xC0000002L: // STATUS_NOT_IMPLEMENTED（驱动端"调用方在沙箱内"守卫）
        return SbieStatus::ACCESS_DENIED;
    case 0xC0000080L: // STATUS_SERVER_DISABLED（sbieapi.c:131-133 的"驱动未运行"映射）
        return SbieStatus::DRIVER_UNAVAILABLE;
    case 0xC0000034L: // STATUS_OBJECT_NAME_NOT_FOUND
        return SbieStatus::NOT_FOUND;
    case 0xC000008BL: // STATUS_RESOURCE_NAME_NOT_FOUND（驱动端"值不存在"，
        //               // Conf_Api_Query 无值返回，drv/conf.c:1885-1887；SDK
        //               // ntstatus.h:3354 实读核对 = 0xC000008B）
        return SbieStatus::NOT_FOUND;
    case 0xC0000033L: // STATUS_OBJECT_NAME_INVALID
        return SbieStatus::NOT_FOUND;
    case 0xC0000023L: // STATUS_BUFFER_TOO_SMALL（枚举跳过逻辑重试，不该外泄；折叠）
        return SbieStatus::GENERIC;
    case 0xC000006EL: // STATUS_ACCOUNT_RESTRICTION（box 存在但 Enabled=n）
        return SbieStatus::GENERIC; // 语义由调用方（IsBoxEnabled 薄封装）显式解读
    case 0xC0000038L: // STATUS_DEVICE_ALREADY_ATTACHED（会话已有 leader）
        return SbieStatus::GENERIC;
    case 0xC000006AL: // STATUS_WRONG_PASSWORD
        return SbieStatus::ACCESS_DENIED;
    case 0xC0000155L: // STATUS_LOGON_NOT_GRANTED（SbieSvc：非管理员改他人节）
        return SbieStatus::ACCESS_DENIED;
    case 0xC00000BBL: // STATUS_NOT_SUPPORTED（SbieSvc：调用方在沙箱内）
        return SbieStatus::ACCESS_DENIED;
    case 0xC0000001L: // STATUS_UNSUCCESSFUL
    default:
        return SbieStatus::GENERIC;
    }
}

int ToExitCode(SbieStatus s)
{
    long v = static_cast<long>(s);
    if (v >= 0 && v <= 9)
        return (int)v;
    // 扩展码折叠
    switch (s) {
    case SbieStatus::ERR_SBIEDLL:
        return (int)SbieStatus::DRIVER_UNAVAILABLE; // 3：SbieDll 不可得 ≈ 驱动不可用
    case SbieStatus::ERR_SVC_TRANSPORT:
        return (int)SbieStatus::SERVER_UNAVAILABLE; // 4：SbieSvc 传输不可用
    case SbieStatus::ERR_JSON:
    case SbieStatus::ERR_NOT_IMPLEMENTED:
    default:
        return (int)SbieStatus::GENERIC; // 1
    }
}

const wchar_t* StatusName(SbieStatus s)
{
    switch (s) {
    case SbieStatus::OK:                 return L"OK";
    case SbieStatus::GENERIC:            return L"GENERIC";
    case SbieStatus::USAGE:              return L"USAGE";
    case SbieStatus::DRIVER_UNAVAILABLE: return L"DRIVER_UNAVAILABLE";
    case SbieStatus::SERVER_UNAVAILABLE: return L"SERVER_UNAVAILABLE";
    case SbieStatus::NOT_FOUND:          return L"NOT_FOUND";
    case SbieStatus::ACCESS_DENIED:      return L"ACCESS_DENIED";
    case SbieStatus::INVALID:            return L"INVALID";
    case SbieStatus::RETRY_SUGGESTED:    return L"RETRY_SUGGESTED";
    case SbieStatus::BOX_BUSY:           return L"BOX_BUSY";
    case SbieStatus::ERR_JSON:           return L"ERR_JSON";
    case SbieStatus::ERR_SBIEDLL:        return L"ERR_SBIEDLL";
    case SbieStatus::ERR_SVC_TRANSPORT:  return L"ERR_SVC_TRANSPORT";
    case SbieStatus::ERR_NOT_IMPLEMENTED:return L"ERR_NOT_IMPLEMENTED";
    default:                             return L"UNKNOWN";
    }
}

std::wstring NtStatusText(LONG ntstatus)
{
    // 先经 RtlNtStatusToDosError（ntdll，动态解析避免链接期依赖）→ win32 文案
    typedef ULONG(NTAPI* P_RtlNtStatusToDosError)(LONG);
    static P_RtlNtStatusToDosError pRtl = []() -> P_RtlNtStatusToDosError {
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        return h ? (P_RtlNtStatusToDosError)(void(*)(void))GetProcAddress(
                       h, "RtlNtStatusToDosError")
                 : nullptr;
    }();

    if (pRtl) {
        wchar_t buf[512];
        DWORD n = 0;
        ULONG dos = pRtl(ntstatus);
        if (dos != ERROR_MR_MID_NOT_FOUND) {
            n = FormatMessageW(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, dos,
                MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                buf, (DWORD)std::size(buf), nullptr);
            if (n) {
                while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n'
                             || buf[n - 1] == L' '))
                    --n;
                return L"(win32 " + std::to_wstring(dos) + L") "
                       + std::wstring(buf, n);
            }
        }
        // ntdll 自带消息表（SBIE 文案不在其中，但 NTSTATUS 名称在部分系统可用）
        n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           GetModuleHandleW(L"ntdll.dll"), (DWORD)ntstatus, 0,
                           buf, (DWORD)std::size(buf), nullptr);
        if (n) {
            while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n'
                         || buf[n - 1] == L' '))
                --n;
            return std::wstring(buf, n);
        }
    }
    wchar_t hex[16];
    swprintf_s(hex, L"0x%08X", (unsigned)ntstatus);
    return hex;
}

} // namespace sbie
