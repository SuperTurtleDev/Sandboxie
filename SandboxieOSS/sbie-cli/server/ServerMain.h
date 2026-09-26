// Sandboxie-OSS — sbie-cli/server/ServerMain.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// server 入口契约（04-modules.md §2.6）：--start-server 进 ServerMain。
// 生命周期/状态机规格见 00-architecture.md §4-§8。
// M2（server 波次）已实现：
//   * 双锁单实例（00 §4：会话互斥体 + FILE_FLAG_FIRST_PIPE_INSTANCE 抢占）
//   * LoadSbieDll → 驱动在场检测 → API_SESSION_LEADER(set) → LogPump
//   * Dispatcher.cpp：op → SbieCore → JSON（读路径 op 全量 + 未实现占位）
//   * SvcProxy.cpp：SbieSvc LPC 专职线程（03 §1 线程亲和）
//   * 空闲退出 idleTimeoutSec（默认 300；0=不退出），有活动客户端不退
//   * server.shutdown 优雅停机（同会话校验，排空 ≤10s）

#pragma once

#include "../../SbieCore/Util/Status.h"

#include <windows.h>

namespace sbie::server {

struct ServerOptions {
    ULONG idleTimeoutSec = 300;   // 0 = 不退出（00 §6）
};

// 对外契约：int RunServer(const ServerOptions&)
int RunServer(const ServerOptions& options);

} // namespace sbie::server
