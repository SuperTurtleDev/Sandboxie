// Sandboxie-OSS — sbie-cli/cli/Commands/CfgTemplateCommands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// cfg/template/log 命令集（04-modules.md §4.5-§4.7；第二波：cfg/template/log/
// maintenance agent）。实现分布于 cfg_read.cpp / cfg_reload.cpp /
// template_cmd.cpp / log_cmd.cpp。
//
// 注册方式（多 agent 边界约定）：
//   * 本头导出 RegisterCfgTemplateCommands()——整合 agent 在
//     RegisterCommands()（Commands.cpp，本波次禁改）末尾调用一次即可接线；
//   * 各实现文件同时以静态初始化器把 handler 写入 cli::Commands() 注册表
//     （04 §2.6"新命令只加注册项"）——接线（删除 Commands.cpp 内对应桩行）
//     完成后即生效；
//   * 接线完成前的自测通道：环境变量 SBIE_CLI_DIRECT=1 时 cfg_read.cpp 的
//     分发器在静态初始化期直接路由本命令集（绕过 M1 桩），详见其文件头。

#pragma once

#include "../Cli.h"

#include <string>
#include <vector>

namespace sbie::cli {

// ---- handlers ------------------------------------------------------------

// cfg_read.cpp（04 §4.5 读路径；可降级直连）
int CmdCfgGet(const CommandContext& ctx);     // cfg get <setting> [--section s] [--index i] [--raw]
int CmdCfgListSetting(const CommandContext& ctx); // cfg list-setting [section]（P0-9）
int CmdCfgPath(const CommandContext& ctx);    // cfg path（经 SbieSvc IniGetPath）

// cfg_reload.cpp（04 §4.5 维护；SbieApi_ReloadConf 直连驱动，可降级）
int CmdCfgReload(const CommandContext& ctx);  // cfg reload [--reconfigure]

// template_cmd.cpp（04 §4.6 全部五个子命令）
int CmdTemplateList(const CommandContext& ctx);   // template list [--class c]
int CmdTemplateInfo(const CommandContext& ctx);   // template info <name>
int CmdTemplateApply(const CommandContext& ctx);  // template apply <box> <name>
int CmdTemplateRevoke(const CommandContext& ctx); // template revoke <box> <name>
int CmdTemplateCheck(const CommandContext& ctx);  // template check <box>

// log_cmd.cpp（04 §4.7；双源聚合：驱动日志泵 + interactive queue）
int CmdLogWatch(const CommandContext& ctx);   // log watch [--pid --msg --raw --interactive]
int CmdLogDump(const CommandContext& ctx);    // log dump|messages [--last n]

// ---- 接线入口（整合 agent 调用；幂等） ----------------------------------

void RegisterCfgTemplateCommands();

// ---- 共享小工具 ----------------------------------------------------------

namespace cfgtmpl {

// 通用选项（04 §3）允许出现在组名之后（框架仅在组名前解析全局选项）——
// 本命令集统一吸收尾置全局旗标进 *opts，返回剥离后的位置/专属选项序列。
inline std::vector<std::wstring> AbsorbTrailingGlobals(const CommandContext& ctx,
                                                       GlobalOptions* opts)
{
    *opts = ctx.opts;
    std::vector<std::wstring> rest;
    const std::vector<std::wstring>& a = ctx.args;
    for (size_t i = 2; i < a.size(); ++i) {
        if (a[i] == L"--json")
            opts->json = true;
        else if (a[i] == L"--quiet" || a[i] == L"-q")
            opts->quiet = true;
        else if (a[i] == L"--no-refresh")
            opts->noRefresh = true;
        else if (a[i] == L"--password" && i + 1 < a.size())
            opts->password = a[++i];
        else
            rest.push_back(a[i]);
    }
    return rest;
}

} // namespace cfgtmpl

} // namespace sbie::cli
