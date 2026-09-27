// Sandboxie-OSS — SbieCore/SvcClient/SvcClient.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// SbieSvc LPC 客户端（协议规格：03-svc-protocol.md；V2 使用面：docs/10
// §12.1"仅 RunSandboxed"+ 附录 3/4 扩展）。V2 实际保留的调用面：
//   IniGetVersion/IniGetPath（info）、IniSetSetting（ImportBox 行部署）、
//   SetPassword（--set-password）、KillOne/KillAll（kill/kill-box）、
//   RunSandboxed（exec）。
// 线程亲和（03 §1）：LPC 端口绑定首次 NtConnectPort 的线程；本客户端以
// std::mutex 串行化全部请求（CLI/monitor 均单线程调用），满足约束。

#pragma once

#include "../Util/Status.h"

#include <windows.h>
#include <string>

namespace sbie::svc {

// ---- 值类型 ----

struct RunResult {
    HANDLE hProcess = nullptr;   // 由调用方 CloseHandle
    ULONG pid = 0;
};

// 单实例。全部 LPC 请求经 mutex 串行（03 §1 线程亲和）。
class SvcClient {
public:
    static SvcClient& Instance();

    bool Connected();   // 惰性连接 \RPC Control\SbieSvcPort

    // req 指向含 MSG_HEADER 的结构；返回 malloc 的完整回复（调用方 free）；
    // 传输错误置 status=ERR_SVC_TRANSPORT 并内部重连一次。
    SbieStatus Call(const void* req, size_t reqLen, void** outRpl, size_t* outRplLen);

    // ---- 便捷层（内部组装 vendored 结构体）----

    SbieStatus IniGetPath(std::wstring* path, bool* isHome);
    SbieStatus IniGetVersion(std::wstring* version, ULONG* abi);

    enum class SetMode { Update, Append, Insert, Delete };  // 对应 0x1811-0x1814
    SbieStatus IniSetSetting(const std::wstring& section, const std::wstring& setting,
                             const std::wstring& value, SetMode mode, bool refresh,
                             const std::wstring& password);
    SbieStatus SetPassword(const std::wstring& oldPw, const std::wstring& newPw);

    // ProcessServer（03 §4）
    SbieStatus KillOne(ULONG pid);
    SbieStatus KillAll(const std::wstring& box, ULONG sessionId = (ULONG)-1);

    SbieStatus RunSandboxed(const std::wstring& box, const std::wstring& cmd,
                            const std::wstring& dir, ULONG creationFlags, RunResult* out);
    // RunSandboxed 失败时服务端回的 win32 错误码（成功清零；诊断用）
    static ULONG LastRunSandboxedWin32();

private:
    SvcClient() = default;
    struct Impl;
    Impl* ImplPtr();
};

} // namespace sbie::svc
