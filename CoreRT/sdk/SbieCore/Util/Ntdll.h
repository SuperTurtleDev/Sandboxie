// Sandboxie-OSS — SbieCore/Util/Ntdll.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// ntdll 符号的动态解析（05-build.md §2：零附加导入库——不链接 ntdll.lib，
// 全部经 GetProcAddress(GetModuleHandleW(L"ntdll.dll")) 取得）。
// SbieCore 内禁止绕过本文件自行声明/解析这些符号。

#pragma once

#include <windows.h>
#include "defines_win32.h"   // SBIE_UNICODE_STRING（全局 _SBIE_UNICODE_STRING）

#ifndef NTAPI
#define NTAPI __stdcall
#endif

namespace sbie::nt {

// ---- 所需的 NT 结构（布局与 winternl.h/ntdef.h 一致；仅本项目用到者）----

typedef struct _SBIE_IO_STATUS_BLOCK {
    union { LONG Status; PVOID Pointer; };
    ULONG_PTR Information;
} SBIE_IO_STATUS_BLOCK;

typedef struct _SBIE_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    SBIE_UNICODE_STRING* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} SBIE_OBJECT_ATTRIBUTES;

inline void InitObjectAttributes(SBIE_OBJECT_ATTRIBUTES* o,
                                 SBIE_UNICODE_STRING* name,
                                 ULONG attrs, HANDLE root, PVOID sd)
{
    o->Length = sizeof(*o);
    o->RootDirectory = root;
    o->ObjectName = name;
    o->Attributes = attrs;
    o->SecurityDescriptor = sd;
    o->SecurityQualityOfService = nullptr;
}

// ---- 函数指针类型（与 ntdll 导出原型逐参一致）----

typedef LONG(NTAPI* P_NtOpenFile)(HANDLE* FileHandle, ACCESS_MASK DesiredAccess,
                                  SBIE_OBJECT_ATTRIBUTES* ObjectAttributes,
                                  SBIE_IO_STATUS_BLOCK* IoStatusBlock,
                                  ULONG ShareAccess, ULONG OpenOptions);
typedef LONG(NTAPI* P_NtDeviceIoControlFile)(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    SBIE_IO_STATUS_BLOCK* IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength);
typedef LONG(NTAPI* P_NtConnectPort)(
    HANDLE* PortHandle, SBIE_UNICODE_STRING* PortName,
    SECURITY_QUALITY_OF_SERVICE* SecurityQos, PVOID ClientView,
    PVOID ServerView, ULONG* MaxMessageSize, PVOID ConnectionInformation,
    ULONG* ConnectionInformationLength);
typedef LONG(NTAPI* P_NtRequestWaitReplyPort)(HANDLE PortHandle,
                                              PVOID RequestMessage,
                                              PVOID ReplyMessage);
typedef LONG(NTAPI* P_NtClose)(HANDLE Handle);
typedef ULONG(NTAPI* P_RtlNtStatusToDosError)(LONG Status);

struct NtdllApi {
    P_NtOpenFile              NtOpenFile;
    P_NtDeviceIoControlFile   NtDeviceIoControlFile;
    P_NtConnectPort           NtConnectPort;
    P_NtRequestWaitReplyPort  NtRequestWaitReplyPort;
    P_NtClose                 NtClose;
    P_RtlNtStatusToDosError   RtlNtStatusToDosError;
};

// 进程内一次解析；失败项为 nullptr（调用方判空）。ntdll.dll 永在进程中，
// GetModuleHandle 即可，无需 LoadLibrary。
NtdllApi* Get();

} // namespace sbie::nt
