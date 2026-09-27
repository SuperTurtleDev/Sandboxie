// Sandboxie-OSS — sbie-cli/cli/Cli.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// argv 解析与路由框架（04-modules.md §3-§4）。降级直连判定表：§5——M1 无
// server，全部命令在 EnsureConnected 失败后走直连/不可用桩。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"
#include "ServerConnect.h"

#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Util/Utf8.h"
#include "../server/ServerMain.h"

#include <windows.h>

namespace sbie::cli {

void PrintUsage(bool toStdout)
{
    const wchar_t* usage =
        L"sbie-cli - Sandboxie-OSS command line\n"
        L"\n"
        L"usage: sbie-cli [--json] [--quiet|-q] [--no-server] [--password <pw>]\n"
        L"                [--no-refresh] [--show-transport] [--sbie-dll-path <dir>]\n"
        L"                <group> <command> [...]\n"
        L"\n"
        L"command groups (see docs/04-modules.md section 4):\n"
        L"  status | version                       driver/service/server state\n"
        L"  server start [--no-guardians]|stop|status\n"
        L"                                        sbie-cli server lifecycle\n"
        L"  box list|info|create|delete|rename|enable|disable|set|get|\n"
        L"      list-setting|clean|size|copy|export|import|types|\n"
        L"      recover (list|copy|add)|snapshot ...\n"
        L"      (create --type <t>: hardening|hardened-plus|standard|\n"
        L"       standard-plus|app|app-plus - see 'box types';\n"
        L"       copy <src> <dst> [--content]; export <name> --to <dir|.sbx>;\n"
        L"       import <path> --name <n>; recover copy adds --move (remove\n"
        L"       from sandbox after copy) and runs OnFileRecovery checkers\n"
        L"       unless --no-check;\n"
        L"       clean / delete --files also run the OnBoxDelete trigger;\n"
        L"       pass --no-triggers to skip; guardian keys: OnBoxTerminate,\n"
        L"       AutoDelete, AutoRemove, Temp_* boxes)\n"
        L"  proc list|info|start|kill|kill-all|suspend|resume\n"
        L"      (kill-all <box> | kill-all --all for every box)\n"
        L"  cfg get|set|unset|list-setting|reload|path|lock|unlock\n"
        L"  template list|info|apply|revoke|check\n"
        L"  log watch|dump\n"
        L"  force on [<seconds>]|off|status       disable forced sandboxing\n"
        L"  maint status|start|stop               driver/service lifecycle\n"
        L"                                        [--driver|--service|--all]\n"
        L"\n"
        L"options:\n"
        L"  --json            emit JSON on stdout (04-modules.md section 7.2)\n"
        L"  --quiet, -q       data only, no headers\n"
        L"  --no-server       never auto-start the sbie-cli server\n"
        L"  --password <pw>   config password (overrides SBIE_PASS)\n"
        L"  --no-refresh      skip driver hot-reload on set operations\n"
        L"  --show-transport  diagnostics: report ipc/direct routing per command\n"
        L"  --sbie-dll-path <dir>  explicit SbieDll.dll directory\n"
        L"  --help, -h        this text\n";
    if (toStdout)
        util::PrintUtf8(util::WideToUtf8(usage));
    else
        util::PrintErrUtf8(util::WideToUtf8(usage));
}

int Route(const std::wstring& group, const std::wstring& sub,
          const CommandContext& ctx)
{
    auto& reg = Commands();
    auto g = reg.find(util::WideToUtf8(group));
    if (g == reg.end())
        return -1; // 未注册组
    auto h = g->second.find(util::WideToUtf8(sub));
    if (h == g->second.end())
        return -2; // 未注册子命令
    return h->second(ctx);
}

int Run(const std::vector<std::wstring>& argv)
{
    // server 内部入口（00 §5：client 派生 --start-server；非用户语法，
    // 在选项解析前截获）
    if (!argv.empty() && argv[0] == L"--start-server") {
        server::ServerOptions so;
        return server::RunServer(so);
    }

    GlobalOptions opts;
    std::vector<std::wstring> positional;

    for (size_t i = 0; i < argv.size(); ++i) {
        const std::wstring& a = argv[i];
        if (a == L"--json") {
            opts.json = true;
        } else if (a == L"--quiet" || a == L"-q") {
            opts.quiet = true;
        } else if (a == L"--no-server") {
            opts.noServer = true;
        } else if (a == L"--no-refresh") {
            opts.noRefresh = true;
        } else if (a == L"--show-transport") {
            // 诊断（04 §12）：stderr 报告每命令实际走的传输（ipc/direct）。
            // 仅组名前位置生效（IpcRoute 读取；log watch 的专用订阅连接在
            // log_cmd.cpp 内另行报告）
            opts.showTransport = true;
        } else if (a == L"--password" || a == L"--sbie-dll-path") {
            if (i + 1 >= argv.size()) {
                EmitError(opts, SbieStatus::USAGE, L"missing value for " + a);
                return 2;
            }
            if (a == L"--password")
                opts.password = argv[++i];
            else
                opts.sbieDllPath = argv[++i];
        } else if (a == L"--help" || a == L"-h") {
            PrintUsage(true);
            return 0;
        } else if (a.size() >= 2 && a[0] == L'-' && positional.empty()) {
            // 组名之前：此处只允许出现全局选项（--json/--password 等）
            EmitError(opts, SbieStatus::USAGE, L"unknown option: " + a);
            return 2;
        } else {
            // 组名之后：命令专属标志（--all/--box/--append 等）原样透传，
            // 由各命令 handler 自行解析（04 §4 各命令参数）
            positional.push_back(a);
        }
    }

    if (positional.empty()) {
        PrintUsage(false);
        return 2;
    }

    RegisterCommands();

    // 沙箱内自检（02 §6：sbie-cli client 不得在沙箱内运行）
    if (drv::LoadSbieDll(opts.sbieDllPath) && drv::InSandbox()) {
        return EmitError(opts, SbieStatus::ACCESS_DENIED,
                         L"sbie-cli must not run inside a sandbox");
    }

    const std::wstring group = positional[0];
    std::wstring sub;
    if (positional.size() >= 2)
        sub = positional[1];

    // server 探测/拉起（00 §5 状态机；M1 server 未实现，可降级命令在各自
    // handler 内直连并提示，不可降级命令由桩退出 4）
    if (!opts.noServer)
        srvconn::EnsureConnected(opts);

    CommandContext ctx{ opts, positional };
    int rc = Route(group, sub, ctx);
    if (rc == -1 || rc == -2) {
        EmitError(opts, SbieStatus::USAGE,
                  rc == -1 ? L"unknown command group: " + group
                           : L"unknown subcommand: " + group + L" " + sub);
        return 2;
    }
    return rc;
}

} // namespace sbie::cli
