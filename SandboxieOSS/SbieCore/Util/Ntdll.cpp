// Sandboxie-OSS — SbieCore/Util/Ntdll.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "Ntdll.h"

namespace sbie::nt {

NtdllApi* Get()
{
    static NtdllApi api = []() -> NtdllApi {
        NtdllApi a{};
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        if (!h)
            return a;
        a.NtOpenFile = (P_NtOpenFile)(void (*)(void))
            GetProcAddress(h, "NtOpenFile");
        a.NtDeviceIoControlFile = (P_NtDeviceIoControlFile)(void (*)(void))
            GetProcAddress(h, "NtDeviceIoControlFile");
        a.NtConnectPort = (P_NtConnectPort)(void (*)(void))
            GetProcAddress(h, "NtConnectPort");
        a.NtRequestWaitReplyPort = (P_NtRequestWaitReplyPort)(void (*)(void))
            GetProcAddress(h, "NtRequestWaitReplyPort");
        a.NtClose = (P_NtClose)(void (*)(void))GetProcAddress(h, "NtClose");
        a.RtlNtStatusToDosError = (P_RtlNtStatusToDosError)(void (*)(void))
            GetProcAddress(h, "RtlNtStatusToDosError");
        return a;
    }();
    return &api;
}

} // namespace sbie::nt
