// Sandboxie-OSS — Cli/sbie-rt/commands/Commands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 命令面（docs/10-v2-design.md §9 + 附录 2/4；V1 命令已整体删除——
// 拍板 D4）：
//   exec | register | unregister | sync-config | ps | kill-box | kill | log | info
// 加 create-box | create-encbox。V2/V3 合并后由 sbie-rt.cpp 统一入口
// 分发（动态盒命令 drv/create/exec<handle>/destroy/query 在入口层拦截）。

#pragma once

#include "Cli.h"

namespace sbie::cli {

// V2 用户命令（V2Commands.cpp / BoxCommands.cpp / InfoCommand.cpp / LogCommand.cpp）
int CmdExec(const CommandContext& ctx);
int CmdRegister(const CommandContext& ctx);
int CmdUnregister(const CommandContext& ctx);
int CmdSyncConfig(const CommandContext& ctx);
int CmdPs(const CommandContext& ctx);
int CmdKillBox(const CommandContext& ctx);
int CmdKill(const CommandContext& ctx);
int CmdLog(const CommandContext& ctx);
int CmdInfo(const CommandContext& ctx);
int CmdCreateBox(const CommandContext& ctx);
int CmdCreateEncBox(const CommandContext& ctx);

void RegisterCommands();

} // namespace sbie::cli
