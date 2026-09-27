// Sandboxie-OSS — sbie-cli/cli/Commands/Commands_D3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 波次 D3 注册集线（docs/04 §19）——多 agent 边界约定：D3 的全部命令注册
// 统一放本文件 RegisterD3Commands()（Commands.cpp 末尾一行调用），后注册
// 覆盖同名原实现（box create/info/list、box snapshot、proc info 的增强版
// 在 box_d3.cpp / proc_d3.cpp——原文件不动）。新命令：template gen-browser
// （template_gen.cpp）、doctor（doctor_cmd.cpp）、proc suspend-box/resume-box
// 与 cfg whoami 与 proc info 增强/proc exempt（proc_d3.cpp）、maint
// install/uninstall（maint_d3.cpp）、box explore（box_d3.cpp）。

#include "../Commands.h"

namespace sbie::cli {

// box_d3.cpp
int CmdBoxCreateD3(const CommandContext& ctx);
int CmdBoxInfoD3(const CommandContext& ctx);
int CmdBoxListD3(const CommandContext& ctx);
int CmdBoxSnapshotD3(const CommandContext& ctx);
int CmdBoxExplore(const CommandContext& ctx);
// template_gen.cpp
int CmdTemplateGenBrowser(const CommandContext& ctx);
// doctor_cmd.cpp
void RegisterDoctorCommand();
// proc_d3.cpp
int CmdProcSuspendBox(const CommandContext& ctx);
int CmdProcResumeBox(const CommandContext& ctx);
int CmdCfgWhoami(const CommandContext& ctx);
int CmdProcInfoD3(const CommandContext& ctx);
int CmdProcExempt(const CommandContext& ctx);
int CmdProcStartD3(const CommandContext& ctx);
// maint_d3.cpp
int CmdMaintInstall(const CommandContext& ctx);
int CmdMaintUninstall(const CommandContext& ctx);
// cfg_d3.cpp（06 P2-9 用户面）
int CmdBoxDump(const CommandContext& ctx);
int CmdCfgDump(const CommandContext& ctx);

void RegisterD3Commands()
{
    auto& reg = Commands();
    // 覆盖增强（后注册胜）
    reg["box"]["create"] = CmdBoxCreateD3;       // +高级旗标（无旗标=原路径）
    reg["box"]["info"] = CmdBoxInfoD3;           // +type/never_delete/… 派生
    reg["box"]["list"] = CmdBoxListD3;           // +--type 过滤（无旗标=原路径）
    reg["box"]["snapshot"] = CmdBoxSnapshotD3;   // +default 动词（其余=原路径）
    reg["proc"]["info"] = CmdProcInfoD3;         // +image_type/flags_decoded/
                                                //  elevated/wow64（06 P2-5）
    reg["proc"]["start"] = CmdProcStartD3;       // +Start.exe 伪命令路由
                                                //  （07-P2-3，非伪=原路径）
    // 新命令
    reg["box"]["explore"] = CmdBoxExplore;                     // 06 P2-13
    reg["template"]["gen-browser"] = CmdTemplateGenBrowser;    // 07-P2-5
    reg["proc"]["suspend-box"] = CmdProcSuspendBox;            // 06 P2-6
    reg["proc"]["resume-box"] = CmdProcResumeBox;              // 06 P2-6
    reg["proc"]["exempt"] = CmdProcExempt;                     // 06 P2-14
    reg["cfg"]["whoami"] = CmdCfgWhoami;                       // 06 P2-12
    reg["maint"]["install"] = CmdMaintInstall;                 // 06 P2-7
    reg["maint"]["uninstall"] = CmdMaintUninstall;             // 06 P2-7
    reg["box"]["dump"] = CmdBoxDump;                           // 06 P2-9
    reg["cfg"]["dump"] = CmdCfgDump;                           // 06 P2-9
    RegisterDoctorCommand();                                   // 07-P2-4 精简版
}

} // namespace sbie::cli
