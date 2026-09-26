// Sandboxie-OSS — sbie-cli/cli/ServerConnect.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// client 侧 server 探测/自动拉起/降级框架（00-architecture §5）。
// M2（server 波次）：kServerImplemented=true，(A)-(E) 状态机已实现——
//   (A) CreateFileW(管道) 成功 → 走管道
//   (D) ERROR_PIPE_BUSY → WaitNamedPipeW 后重试
//   (B) ERROR_FILE_NOT_FOUND → 派生 "<自身 exe> --start-server"
//       （DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB，cwd=exe 目录）
//       → 50ms 轮询，上限 3000ms
//   (C) 拉起失败/超时 → 降级（可降级命令直连 + NoteDegraded 提示；
//       不可降级命令由命令层报 SERVER_UNAVAILABLE=4）
//   (E) --no-server：永不拉起
//
// 接线 agent 用法（把命令实现从直连切到 IPC）：
//   * Cli.cpp 在路由前已调 EnsureConnected(opts)——连接缓存在本模块；
//   * 命令 handler 内：srvconn::Call("box.list", params) 得 IpcOutcome，
//     data 为 JSON（行集 op = 对象数组，字段名与直连 --json 相同），
//     直接交给 Output 的表格/JSON 双轨输出；
//   * Call 传输失败且 retry=false（非幂等命令）→ code=8 RETRY_SUGGESTED
//    （00 §7：server 中途崩溃，不自动重试）；retry=true 自动拉起+重试一次。

#pragma once

#include "Cli.h"
#include "../ipcc/SbieIpc.h"
#include "../../SbieCore/Util/Json.h"

namespace sbie::cli::srvconn {

inline constexpr bool kServerImplemented = true;

// 探测/拉起管道连接（幂等；成功后连接缓存在本模块）。
// opts.noServer → 恒 false 且不派生（00 §5 (E)）。
bool EnsureConnected(const GlobalOptions& opts);

// 当前是否持有可用 IPC 连接（EnsureConnected 成功后且未断裂）
bool HasServer();

// 管道探测（不拉起；sbie server status 用）：server 在跑返回 true 并回填
// server pid。
bool ProbeRunning(ULONG* serverPid = nullptr);

// 无 server 时的降级直连提示（去重：每进程一次；已连 server 则不提示）
void NoteDegraded();

// 一次 IPC 往返结果
struct IpcOutcome {
    bool transport = false;  // false = 管道断裂且无法恢复（server 死）
    bool ok = false;         // reply JSON 的 "ok"
    int code = 0;            // error.code（transport=false 时 = 4/8）
    std::wstring message;
    json::JsonValue data;    // ok=true 时的 "data"（行集=数组，其余=对象）
};

// 经缓存连接的请求往返；传输失败时（可选）自动拉起 server 重试一次。
// retry=false：非幂等命令（box create/cfg set/snapshot take 等，00 §7）。
IpcOutcome Call(const char* op, const json::JsonValue& params,
                bool retry = true);

} // namespace sbie::cli::srvconn
