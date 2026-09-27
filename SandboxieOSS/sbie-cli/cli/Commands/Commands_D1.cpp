// Sandboxie-OSS — sbie-cli/cli/Commands/Commands_D1.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 波次 D1 注册集线（docs/04 §20）——多 agent 边界约定：D1 的全部命令注册
// 统一放本文件 RegisterD1Commands()（Commands.cpp 在 "// D3" 标记之前一行
// 调用）。本波命令：trace watch|dump（trace/资源监控面，07 深度差距
// 06-P2-1 收口；实现于 trace_cmd.cpp——server TracePump 订阅推送优先，
// --no-server 降级直连自拉）。

#include "../Commands.h"

namespace sbie::cli {

// trace_cmd.cpp（04 §20）
int CmdTraceWatch(const CommandContext& ctx);  // trace watch [--box --type --pid --json]
int CmdTraceDump(const CommandContext& ctx);   // trace dump [--last n --box --type --pid]

void RegisterD1Commands()
{
    auto& reg = Commands();
    // trace（04 §20，波 D1）——TracePump 订阅/环形缓冲
    reg["trace"]["watch"] = CmdTraceWatch;
    reg["trace"]["dump"] = CmdTraceDump;
}

} // namespace sbie::cli
