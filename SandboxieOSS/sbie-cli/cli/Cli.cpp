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
        L"       AutoDelete, AutoRemove, Temp_* boxes;\n"
        L"       create also accepts --location <dir> (custom FileRootPath),\n"
        L"       --temp (AutoDelete+AutoRemove one-shot box), --v2-delete,\n"
        L"       --auto-recover, --block-net, --drop-admin;\n"
        L"       info adds type/never_delete/auto_delete/empty/initialized;\n"
        L"       list takes --type <t>; snapshot default <box> [<id>|--clear]\n"
        L"       reads/sets the default-snapshot marker; dump <name> prints\n"
        L"       the raw ini section; explore <name> opens the file root in\n"
        L"       the host explorer)\n"
        L"  proc list|info|start|kill|kill-all|suspend|resume|\n"
        L"      suspend-box|resume-box|exempt\n"
        L"      (kill-all <box> | kill-all --all for every box; --all skips\n"
        L"       boxes with ExcludeFromTerminateAll=y unless --no-exceptions;\n"
        L"       suspend-box/resume-box <box> or --all;\n"
        L"       exempt <pid> <on|off|get> [--what internet|spooler]:\n"
        L"       per-process exemption control (API_PROCESS_EXEMPTION_CONTROL);\n"
        L"       start forwards Start.exe pseudo commands: run_dialog,\n"
        L"       default_browser, mail_agent, auto_run, and plain system\n"
        L"       targets like explorer.exe / regedit.exe / cmd.exe)\n"
        L"  cfg get|set|unset|list-setting|reload|path|lock|unlock|whoami|dump\n"
        L"      (whoami: current user / ini section / admin;\n"
        L"       dump [<section>]: section list, or raw ini section)\n"
        L"  template list|info|apply|revoke|check|gen-browser\n"
        L"      (gen-browser [--browser <name>] [--access <a,b,...>]\n"
        L"       [--no-force] [--box <b> --install | --remove]: scan installed\n"
        L"       browsers (Chrome/Edge/Firefox/Chromium/Brave/Vivaldi/Opera)\n"
        L"       and generate [Template_Local_*] sections with ForceProcess\n"
        L"       + direct-access OpenFilePath rules; dry-run by default;\n"
        L"       access categories: bookmarks|history|cookies|passwords|\n"
        L"       preferences|profile, default bookmarks,cookies,passwords,\n"
        L"       preferences)\n"
        L"  log watch|dump\n"
        L"  trace watch|dump [--box <b>] [--type <t>] [--pid <p>]\n"
        L"                                        sandbox resource trace\n"
        L"                                        (watch streams, Ctrl+C stops;\n"
        L"                                        dump takes --last <n>=100;\n"
        L"                                        types: apicall syscall pipe\n"
        L"                                        ipc rpc winclass drive comclass\n"
        L"                                        rtclass ignore image file key\n"
        L"                                        socket dns scm hook debug)\n"
        L"  force on [<seconds>]|off|status       disable forced sandboxing\n"
        L"  maint status|start|stop|install|uninstall\n"
        L"                                        driver/service lifecycle\n"
        L"                                        [--driver|--service|--all]\n"
        L"                                        (install/uninstall = service\n"
        L"                                        registration, admin only)\n"
        L"  doctor                                read-only health check\n"
        L"                                        (driver/service/abi/install\n"
        L"                                        layout/common config issues;\n"
        L"                                        exit 1 when a check FAILs)\n"
        L"  img list|status [<box>]|create <box> --size-mb <N>|mount <box>|\n"
        L"      unmount <box>                     box disk image (ImBox) ops;\n"
        L"      create/mount take [--password <pw>] (AES); mount takes\n"
        L"      [--protect|--no-protect] [--admin-only|--no-admin-only]\n"
        L"      [--auto-unmount] (mount executor lives in SbieSvc;\n"
        L"      UseFileImage=y auto-mounts on box start)\n"
        L"  ramdisk status                        global ramdisk keys + mount\n"
        L"  usb status|sync [--dry-run]           usb drive sandbox takeover\n"
        L"      (ForceUsbDrives/UsbSandbox/DisabledForceVolume keys;\n"
        L"      sync writes ForceFolder list, 07-P2-1)\n"
        L"\n"
        L"options:\n"
        L"  --json            emit JSON on stdout (04-modules.md section 7.2)\n"
        L"  --quiet, -q       data only, no headers\n"
        L"  --no-server       never auto-start the sbie-cli server\n"
        L"  --password <pw>   config password (overrides SBIE_PASS)\n"
        L"  --no-refresh      skip driver hot-reload on set operations\n"
        L"  --show-transport  diagnostics: report ipc/direct routing per command\n"
        L"  --sbie-dll-path <dir>  explicit SbieDll.dll directory\n"
        L"  --help, -h        this text; '<group> --help' lists that group's\n"
        L"                    commands\n";
    if (toStdout)
        util::PrintUtf8(util::WideToUtf8(usage));
    else
        util::PrintErrUtf8(util::WideToUtf8(usage));
}

// 组级帮助（08-P2-6）：<group> --help 打印该组子命令清单（枚举自注册表，
// std::map 字典序）。此前组名后 --help 与顶层完全同文。
// 描述行人工维护；未登记的组退回顶层用法（调用方判定）。
void PrintGroupUsage(const std::wstring& group, bool toStdout)
{
    struct GroupNote {
        const wchar_t* group;
        const wchar_t* note;   // 组内第三级路由/别名等组级提示
    };
    static const GroupNote kNotes[] = {
        { L"status",  L"bare command (no subcommand)" },
        { L"version", L"bare command (no subcommand)" },
        { L"server",  L"" },
        { L"box",     L"snapshot and recover have a third level:\n"
                      L"  snapshot list|take|remove|select|set-info|default\n"
                      L"  recover list|copy|add" },
        { L"proc",    L"" },
        { L"cfg",     L"" },
        { L"template",L"list hides Tmpl.Hide=y templates (--all shows);\n"
                      L"  gen-browser takes --browser/--access/--no-force and\n"
                      L"  --box <b> --install|--remove" },
        { L"log",     L"messages is an alias of dump" },
        { L"trace",   L"" },
        { L"force",   L"on takes optional <seconds>" },
        { L"maint",   L"install/uninstall are admin-only service registration" },
        { L"img",     L"create takes --size-mb; mount takes --protect/--admin-"
                      L"only/--auto-unmount" },
        { L"ramdisk", L"bare command (no subcommand)" },
        { L"usb",     L"sync takes --dry-run" },
        { L"doctor",  L"bare command (no subcommand)" },
    };
    const wchar_t* note = L"";
    for (const GroupNote& n : kNotes)
        if (group == n.group)
            note = n.note;

    const std::string g = util::WideToUtf8(group);
    auto& reg = Commands();
    auto gIt = reg.find(g);
    if (gIt == reg.end())
        return;   // 调用方保证已注册

    std::wstring out = L"sbie-cli " + group + L" - command group help\n\n"
        L"usage: sbie-cli [--json] [--quiet|-q] [--no-server] "
        L"[--show-transport] ... <global options> " + group
        + L" <command> [...]\n\n";
    out += L"commands:\n";
    for (const auto& sub : gIt->second) {
        if (sub.first.empty()) {
            out += L"  (this group takes no subcommand)\n";
            continue;
        }
        out += L"  " + util::Utf8ToWide(sub.first) + L"\n";
    }
    if (*note) {
        out += L"\n";
        out += std::wstring(L"notes:\n") + note + L"\n";
    }
    out += L"\nrun 'sbie-cli --help' for the full command tree.\n";
    if (toStdout)
        util::PrintUtf8(util::WideToUtf8(out));
    else
        util::PrintErrUtf8(util::WideToUtf8(out));
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
    bool helpSeen = false;

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
            helpSeen = true;   // 位置敏感性在注册表就绪后统一分派（下方）
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

    RegisterCommands();

    // --help 分派（08-P2-6）：组名前 = 顶层用法；组名后 = 该组子命令清单
    //（枚举自注册表）；组名未注册 = 顶层用法（向后兼容旧行为）
    if (helpSeen) {
        if (!positional.empty()) {
            auto& reg = Commands();
            const auto g = reg.find(util::WideToUtf8(positional[0]));
            if (g != reg.end()) {
                PrintGroupUsage(positional[0], true);
                return 0;
            }
        }
        PrintUsage(true);
        return 0;
    }

    if (positional.empty()) {
        PrintUsage(false);
        return 2;
    }

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
