// Sandboxie-OSS — sbie-cli/cli/Commands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 命令面（docs/10-v2-design.md §9 + 附录 2/4；V1 命令已整体删除——
// 拍板 D4）：
//   exec | register | unregister | sync-config | ps | kill-box | kill | log | info
// 加内部入口：--monitor（main.cpp 截获）、--migrate-templates（M5 迁移工具）。

#pragma once

#include "Cli.h"

namespace sbie::cli {

// V2 用户命令（V2Commands.cpp）
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

// 内部工具：--migrate-templates <Templates.ini> <outDir>（TemplateMigrate.cpp）
int CmdMigrateTemplates(const std::wstring& src, const std::wstring& outDir);

void RegisterCommands();

} // namespace sbie::cli
