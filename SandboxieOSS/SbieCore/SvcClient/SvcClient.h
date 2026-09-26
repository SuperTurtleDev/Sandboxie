// Sandboxie-OSS — SbieCore/SvcClient/SvcClient.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// SbieSvc LPC 客户端（契约：04-modules.md §2.2；协议规格：03-svc-protocol.md）。
// 本头文件的契约签名冻结；便捷层（IniGet* 等）在 SvcClient.cpp 实现。
//
// 实现说明（给 server 波次 agent）：
//   - M1 已实现 NtConnectPort 连接 + MSG_HEADER 分块收发（03 §2 逐行为对齐），
//     以及 IniGetVersion/IniGetUser/IniGetPath 三个只读便捷函数；
//   - 03 §1 线程亲和：LPC 端口绑定首次 NtConnectPort 的线程。M1 只有
//     client 直连（短命令，主线程串行 + mutex），满足"串行"约束；
//     server 波次若引入多线程请把全部 Call 移交专职线程。
//   - 写路径（IniSetSetting/SetPassword/ProcessServer 组）的组装函数
//     已按 vendored wire 头就位，行为规格见 03 §3/§4。

#pragma once

#include "../Util/Status.h"

#include <windows.h>
#include <string>

namespace sbie::svc {

// ---- 值类型（04 §2.2 契约：位于命名空间层，非类嵌套）----

struct ProcInfo {
    ULONG parentId = 0, flags = 0;
    bool suspended = false;
    std::wstring image, cmdline, workdir;
};

struct RunResult {
    HANDLE hProcess = nullptr;   // 由调用方 CloseHandle
    ULONG pid = 0;
};

// 单实例。专用线程串行执行所有 LPC 请求（03 §1 线程亲和）。
class SvcClient {
public:
    static SvcClient& Instance();

    bool Connected();   // 惰性连接 \RPC Control\SbieSvcPort

    // req 指向含 MSG_HEADER 的结构；返回 malloc 的完整回复（调用方 free）；
    // 传输错误置 status=ERR_SVC_TRANSPORT 并内部重连一次。
    SbieStatus Call(const void* req, size_t reqLen, void** outRpl, size_t* outRplLen);

    // ---- 便捷层（内部组装 vendored 结构体）----

    SbieStatus IniGetUser(bool* admin, std::wstring* section, std::wstring* name);
    SbieStatus IniGetPath(std::wstring* path, bool* isHome);
    SbieStatus IniGetVersion(std::wstring* version, ULONG* abi);

    enum class SetMode { Update, Append, Insert, Delete };  // 对应 0x1811-0x1814
    SbieStatus IniSetSetting(const std::wstring& section, const std::wstring& setting,
                             const std::wstring& value, SetMode mode, bool refresh,
                             const std::wstring& password);
    SbieStatus IniGetSetting(const std::wstring& section, const std::wstring& setting,
                             std::wstring* value);
    SbieStatus SetPassword(const std::wstring& oldPw, const std::wstring& newPw);
    SbieStatus TestPassword(const std::wstring& pw);

    // ProcessServer（03 §4）
    SbieStatus KillOne(ULONG pid);
    SbieStatus KillAll(const std::wstring& box, ULONG sessionId = (ULONG)-1);
    SbieStatus SuspendResume(ULONG pid, bool suspend);
    SbieStatus SuspendResumeAll(const std::wstring& box, bool suspend);

    SbieStatus GetProcInfo(ULONG pid, unsigned infoClasses /*1|2|4*/, ProcInfo* out);
    SbieStatus RunSandboxed(const std::wstring& box, const std::wstring& cmd,
                            const std::wstring& dir, ULONG creationFlags, RunResult* out);

private:
    SvcClient() = default;
    struct Impl;
    Impl* ImplPtr();
};

} // namespace sbie::svc
