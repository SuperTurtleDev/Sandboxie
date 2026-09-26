// Sandboxie-OSS — sbie-cli/main.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// wmain 薄入口：UTF-8 控制台初始化 + argv 转发到 cli::Run（04 §7.1）。
// server 波次接线：--start-server 在此截获进 server::RunServer（cli 路由前），
// 额外解析 --idle-timeout <sec>（默认 300；0=不退出；测试用短超时）。

#include "cli/Cli.h"
#include "server/ServerMain.h"
#include "../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    sbie::util::InitUtf8Console();
    // argv[0] 是程序路径，跳过（命令行参数从 argv[1] 起）
    std::vector<std::wstring> args(argv + (argc > 0 ? 1 : 0), argv + argc);

    // --start-server 接线（server 波次；唯一允许的 main.cpp 改动点）
    if (!args.empty() && args[0] == L"--start-server") {
        sbie::server::ServerOptions so;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == L"--idle-timeout" && i + 1 < args.size()) {
                const long v = wcstol(args[++i].c_str(), nullptr, 10);
                so.idleTimeoutSec = v > 0 ? (ULONG)v : 0; // <=0 = 不退出
            }
        }
        return sbie::server::RunServer(so);
    }

    return sbie::cli::Run(args);
}
