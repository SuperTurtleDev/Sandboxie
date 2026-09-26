// Sandboxie-OSS — sbie-cli/cli/Commands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// M1 直连命令：version / status / box list / proc list（04 §4.1/§4.3/§4.4，
// 全部属 §5 可降级直连表）。其余命令注册为未实现桩（server/Svc 波次）。

#pragma once

#include "Cli.h"

namespace sbie::cli {

int CmdVersion(const CommandContext& ctx);
int CmdStatus(const CommandContext& ctx);
int CmdBoxList(const CommandContext& ctx);
int CmdProcList(const CommandContext& ctx);

// 未实现桩（报错退出；exit 码按命令语义选 1/4）
int CmdNotImplemented(const CommandContext& ctx);
int CmdServerUnavailable(const CommandContext& ctx);

void RegisterCommands();

} // namespace sbie::cli
