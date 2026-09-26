// Vendored from Sandboxie core (GPLv3): common/defines.h (BOXNAME_COUNT/CONF_* 片段)
//   + common/win32_ntddk.h (UNICODE_STRING64/UNICODE_STRING/MAX_PORTMSG_LENGTH/PORT_MESSAGE 片段)
//   + common/my_version.h (SANDBOXIE 宏) @ 5.73.5
// 版权与许可随原文件（GPLv3，见仓库 LICENSE.Classic）：
/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC
 * Copyright 2020-2024 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// 说明：本文件是按 05-build.md §1 从上述 GPLv3 core 头中抽取的"需要的片段"汇编，
// 宏取值逐字未改（见 01-license-map.md §4）。仅供 SandboxieOSS 用户态组件使用，
// 不得再从此文件反向修改驱动 ABI。

#ifndef SBIE_OSS_VENDOR_DEFINES_WIN32_H
#define SBIE_OSS_VENDOR_DEFINES_WIN32_H

#pragma warning(push, 3)
#include <windows.h>

//---------------------------------------------------------------------------
// common/my_version.h:51 — 产品名宏（API_DEVICE_NAME 组装需要）
//---------------------------------------------------------------------------

#ifndef SANDBOXIE
#define SANDBOXIE               L"Sandboxie"
#endif

//---------------------------------------------------------------------------
// common/defines.h:54-63 — 名称/配置常量（ABI 相关，勿改）
//---------------------------------------------------------------------------

#ifndef BOXNAME_COUNT
#define BOXNAME_COUNT               (38 + 2)
#endif

#ifndef CONF_LINE_LEN
#define CONF_LINE_LEN               2000
#endif

#define CONF_UPDATE_VALUE           1
#define CONF_APPEND_VALUE           2
//#define CONF_INSERT_VALUE         3
#define CONF_REMOVE_VALUE           4
#define CONF_REMOVE_SECTION         5
#define CONF_UPDATE_TEMPLATES       6

//---------------------------------------------------------------------------
// common/win32_ntddk.h:138-165 — 用户态 NT 字符串结构（32 位指针 / 64 位指针两版）
//---------------------------------------------------------------------------

typedef struct _SBIE_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} SBIE_UNICODE_STRING;

typedef struct _UNICODE_STRING64 {
    USHORT Length;
    USHORT MaximumLength;
    __declspec(align(8)) unsigned __int64 Buffer;
} UNICODE_STRING64;

typedef struct _ANSI_STRING64 {
    USHORT Length;
    USHORT MaximumLength;
    __declspec(align(8)) unsigned __int64 Buffer;
} ANSI_STRING64;

//---------------------------------------------------------------------------
// common/win32_ntddk.h:1681 / 1703-1728 — LPC 消息上限与 PORT_MESSAGE 布局
//   x64: sizeof(PORT_MESSAGE) == 0x28 (40)；x86: 0x18 (24)。
//   注：尾部 UCHAR Data[0] 省略——本项目一律以
//   (UCHAR*)buffer + sizeof(PORT_MESSAGE) 取数据区，不改动结构体语义。
//---------------------------------------------------------------------------

#define MAX_PORTMSG_LENGTH 328

// winnt.h 的 CLIENT_ID 仅在部分包含路径下可见；此处等价重定义（布局同）
typedef struct _SBIE_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} SBIE_CLIENT_ID;

typedef struct _PORT_MESSAGE {
    union {
        struct {
            USHORT DataLength;
            USHORT TotalLength;
        } s1;
        ULONG Length;
    } u1;
    union {
        struct {
            USHORT Type;
            USHORT DataInfoOffset;
        } s2;
        ULONG ZeroInit;
    } u2;
    union {
        SBIE_CLIENT_ID ClientId;
        double DoNotUseThisField;       // force quadword alignment
    };
    ULONG MessageId;
    union {
        ULONG_PTR ClientViewSize;       // for LPC_CONNECTION_REQUEST message
        ULONG CallbackId;               // for LPC_REQUEST message
    };
} PORT_MESSAGE, *PPORT_MESSAGE;

#pragma warning(pop)

#endif // SBIE_OSS_VENDOR_DEFINES_WIN32_H
