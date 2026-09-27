// Sandboxie-OSS — sbie-cli/cli/Cli.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// argv 解析（位置参数 + `-`/`--` 选项）、命令路由框架（04-modules.md §2.6/§3）。
// 通用选项：--json / --quiet(-q) / --password <pw> / --no-server / --no-refresh /
// --help(-h)；全局选项：--sbie-dll-path <dir>（传给 drv::LoadSbieDll）。
// 密码优先级：选项 > 环境变量 SBIE_PASS（在 Commands 使用点解析）。

#pragma once

#include <map>
#include <string>
#include <vector>

namespace sbie::cli {

struct GlobalOptions {
    bool json = false;          // --json
    bool quiet = false;         // --quiet / -q
    bool noServer = false;      // --no-server
    bool noRefresh = false;     // --no-refresh（box/cfg set 类）
    bool help = false;          // --help / -h
    bool showTransport = false; // --show-transport（诊断：stderr 报告每命令
                                // 实际走的传输——ipc / direct；server 写路径
                                // 波次验收用，04 §12）
    std::wstring password;      // --password <pw>
    std::wstring sbieDllPath;   // --sbie-dll-path <dir>（全局）
};

struct CommandContext {
    GlobalOptions opts;
    std::vector<std::wstring> args;   // 子命令树剩余位置参数（含组名/子名）
};

using CommandHandler = int (*)(const CommandContext& ctx);

// wmain 入口（main.cpp 薄转发）。返回进程退出码（04 §6 语义）。
int Run(const std::vector<std::wstring>& argv);

// 子命令注册表：group → sub → handler（框架；新命令只加注册项）
std::map<std::string, std::map<std::string, CommandHandler>>& Commands();
// 无子命令的顶层组（status/version）以 L"" 为 sub 键
int Route(const std::wstring& group, const std::wstring& sub,
          const CommandContext& ctx);

// 用法文本（--help / USAGE 错误，stdout / stderr）
void PrintUsage(bool toStdout);
// 组级帮助（08-P2-6）：<group> --help 打印该组子命令清单（注册表枚举）。
// group 必须已注册（未注册组由调用方退回 PrintUsage）。
void PrintGroupUsage(const std::wstring& group, bool toStdout);

} // namespace sbie::cli
