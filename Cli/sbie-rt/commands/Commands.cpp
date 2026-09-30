// Sandboxie-OSS — sbie-cli/cli/Commands.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 命令注册表（拍板 D4：V1 的 ~60 命令全部删除，本表是唯一面）。

#include "Commands.h"

namespace sbie::cli {

std::map<std::string, std::map<std::string, CommandHandler>>& Commands()
{
    static std::map<std::string, std::map<std::string, CommandHandler>> reg;
    return reg;
}

void RegisterCommands()
{
    auto& reg = Commands();
    reg["exec"][""] = CmdExec;
    reg["register"][""] = CmdRegister;
    reg["unregister"][""] = CmdUnregister;
    reg["sync-config"][""] = CmdSyncConfig;
    reg["ps"][""] = CmdPs;
    reg["kill-box"][""] = CmdKillBox;
    reg["kill"][""] = CmdKill;
    reg["log"][""] = CmdLog;
    reg["info"][""] = CmdInfo;
    reg["create-box"][""] = CmdCreateBox;
    reg["create-encbox"][""] = CmdCreateEncBox;
}

} // namespace sbie::cli
