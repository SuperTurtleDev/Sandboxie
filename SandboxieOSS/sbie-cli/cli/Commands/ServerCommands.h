// Sandboxie-OSS — sbie-cli/cli/Commands/ServerCommands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie server start|stop|status（04-modules.md §4.2；接线波次）。
// 实现于 server_cmd.cpp；注册函数由 Commands.cpp 的 RegisterCommands()
// 末尾调用（覆盖 M1 桩）。

#pragma once

#include "../Cli.h"

namespace sbie::cli {

int CmdServerStart(const CommandContext& ctx);   // server start [--idle-timeout N]
int CmdServerStop(const CommandContext& ctx);    // server stop（server.shutdown op）
int CmdServerStatus(const CommandContext& ctx);  // server status（ProbeRunning）

void RegisterServerCommands();

} // namespace sbie::cli
