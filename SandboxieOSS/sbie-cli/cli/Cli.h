// Sandboxie-OSS — sbie-cli/cli/Cli.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 argv 解析与路由（docs/10-v2-design.md §9；命令面经附录 2/4 扩展）。
// V1 命令面已整体删除（拍板 D4）。
// 通用选项：--json / --quiet(-q) / --wait <sec> / --verbose / --sbie-dll-path <dir>
// / --help(-h)。exec 另有 --detach（命令级）。

#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace sbie::cli {

struct GlobalOptions {
    bool json = false;          // --json
    bool quiet = false;         // --quiet / -q
    bool verbose = false;       // --verbose（展开溯源等诊断到 stderr）
    bool execWait = false;      // --wait（exec 等待子进程并透传退出码，R1）
    DWORD waitMs = 10000;       // --settle <sec>（状态机收敛超时，默认 10s）
    std::wstring sbieDllPath;   // --sbie-dll-path <dir>
    std::wstring password;      // --password <pw>（R2；空 = 查 SBIE_PASS 环境变量）
};

struct CommandContext {
    GlobalOptions opts;
    std::vector<std::wstring> args;   // 位置参数（含命令名）
};

using CommandHandler = int (*)(const CommandContext& ctx);

// wmain 入口（main.cpp 薄转发）。返回进程退出码。
int Run(const std::vector<std::wstring>& argv);

// 命令注册表：command → handler（V2 单层面，无二级子命令）
std::map<std::string, std::map<std::string, CommandHandler>>& Commands();
int Route(const std::wstring& group, const std::wstring& sub,
          const CommandContext& ctx);

// 用法文本（--help / USAGE 错误，stdout / stderr）
void PrintUsage(bool toStdout);

} // namespace sbie::cli
