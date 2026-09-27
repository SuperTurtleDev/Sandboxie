// Sandboxie-OSS — sbie-cli/cli/Cli.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 路由框架：五命令 + 内部工具（--migrate-templates）。
// 无 server / 无 IPC / 无降级路径（拍板 D2/D4）。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"

#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

namespace sbie::cli {

void PrintUsage(bool toStdout)
{
    const wchar_t* usage =
        L"sbie-cli - Sandboxie-OSS V2 command line\n"
        L"\n"
        L"usage: sbie-cli [--json] [--quiet|-q] [--wait <sec>] [--verbose]\n"
        L"                [--sbie-dll-path <dir>] <command> [...]\n"
        L"\n"
        L"commands:\n"
        L"  exec PATH\\TO\\box|*alias [cmdline] [--detach]\n"
        L"                                run a command inside the box (state\n"
        L"                                machine: lock -> register -> start ->\n"
        L"                                monitor task; waits for the child and\n"
        L"                                propagates its exit code unless --detach;\n"
        L"                                default cmdline = cmd.exe)\n"
        L"  register PATH\\TO\\box [alias] register the box persistently (no lock,\n"
        L"                                no monitor task; alias enables *alias)\n"
        L"  unregister PATH\\TO\\box|*alias\n"
        L"                                remove registration (refuses while box\n"
        L"                                has processes; deletes cache ini +\n"
        L"                                reloads driver config)\n"
        L"  sync-config PATH\\TO\\box|*alias\n"
        L"                                re-expand sandbox.ini (template changes)\n"
        L"                                rewrite cache + reload\n"
        L"  ps [PATH\\TO\\box|*alias]     no arg: all registered v2 boxes with\n"
        L"                                lock/task/process state; with target:\n"
        L"                                process list of that box\n"
        L"  kill-box PATH\\TO\\box|*alias   kill all processes in the box\n"
        L"                                (monitor will auto-unregister the box\n"
        L"                                once it observes it empty)\n"
        L"  kill <PID>                    kill one sandboxed process\n"
        L"  log [-w] [--last N] [--type TT] [--box B] [--pid P] [--json]\n"
        L"                                driver log, dmesg style (-w follows,\n"
        L"                                Ctrl+C stops; takes session leadership\n"
        L"                                - stop SandMan/monitor first if held)\n"
        L"\n"
        L"box layout (source of truth):  PATH\\TO\\box\\sandbox.ini + drive\\ + user\\\n"
        L"runtime cache (regenerated):   %LOCALAPPDATA%\\SandboxieOSS\\boxes\\<box>.ini\n"
        L"templates:                     SBIE_TEMPLATE_DIR (category\\name.ini,\n"
        L"                                recursive Template= expansion, user-mode)\n"
        L"\n"
        L"options:\n"
        L"  --json              emit JSON on stdout\n"
        L"  --quiet, -q         data only, no headers\n"
        L"  --wait <sec>        state-machine settle timeout (default 10)\n"
        L"  --verbose           expansion trace on stderr\n"
        L"  --sbie-dll-path <dir>  explicit SbieDll.dll directory\n"
        L"  --help, -h          this text\n"
        L"\n"
        L"exit codes: 0 ok; 1 generic; 2 usage; 3 driver unavailable; 5 not found;\n"
        L"            6 access denied; 7 invalid/name collision; 9 box busy;\n"
        L"            10 state timeout; 11 template error; 12 cache/registration\n"
        L"            failure; 13 spawn failure.\n";
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
        return -1;
    auto h = g->second.find(util::WideToUtf8(sub));
    if (h == g->second.end())
        return -2;
    return h->second(ctx);
}

int Run(const std::vector<std::wstring>& argv)
{
    GlobalOptions opts;
    std::vector<std::wstring> positional;
    bool helpSeen = false;

    for (size_t i = 0; i < argv.size(); ++i) {
        const std::wstring& a = argv[i];
        if (a == L"--json") {
            opts.json = true;
        } else if (a == L"--quiet" || a == L"-q") {
            opts.quiet = true;
        } else if (a == L"--verbose") {
            opts.verbose = true;
        } else if (a == L"--wait") {
            // exec 的等待旗标（布尔，无值）：spawn 后等待子进程并透传退出码
            // （R1）。带值的全局形态移至 --settle <sec>（状态机收敛超时），
            // 消除同名互斥。
            opts.execWait = true;
        } else if (a == L"--settle") {
            if (i + 1 >= argv.size()) {
                EmitError(opts, SbieStatus::USAGE, L"missing value for --settle");
                return 2;
            }
            long v = wcstol(argv[++i].c_str(), nullptr, 10);
            opts.waitMs = v > 0 ? (DWORD)(v * 1000) : 1000;
        } else if (a == L"--password") {
            if (i + 1 >= argv.size()) {
                EmitError(opts, SbieStatus::USAGE, L"missing value for --password");
                return 2;
            }
            opts.password = argv[++i];   // R2：优先于 SBIE_PASS（EffectivePassword）
        } else if (a == L"--sbie-dll-path") {
            if (i + 1 >= argv.size()) {
                EmitError(opts, SbieStatus::USAGE, L"missing value for --sbie-dll-path");
                return 2;
            }
            opts.sbieDllPath = argv[++i];
        } else if (a == L"--help" || a == L"-h") {
            helpSeen = true;
        } else if (a.size() >= 2 && a[0] == L'-' && positional.empty()) {
            EmitError(opts, SbieStatus::USAGE, L"unknown option: " + a);
            return 2;
        } else {
            positional.push_back(a);
        }
    }

    RegisterCommands();

    if (helpSeen) {
        PrintUsage(true);
        return 0;
    }
    if (positional.empty()) {
        PrintUsage(false);
        return 2;
    }

    // 沙箱内自检（02 §6：CLI 不得在沙箱内运行）
    // 沙箱内自检（02 §6）：仅 exec 允许盒内运行（CmdExec 的"自身盒"快速
    // 路径——盒内 `exec ./ cmd.exe` 向同盒追加进程）；其余命令拒绝。
    if (drv::LoadSbieDll(opts.sbieDllPath) && drv::InSandbox()
        && positional[0] != L"exec") {
        return EmitError(opts, SbieStatus::ACCESS_DENIED,
                         L"sbie-cli must not run inside a sandbox (except: exec)");
    }

    const std::wstring cmd = positional[0];
    std::wstring sub;
    if (positional.size() >= 2)
        sub = positional[1];
    // V2 命令无二级子命令：positional[1] 起即命令参数。Route 用空 sub 查表。
    CommandContext ctx{opts, positional};
    static const wchar_t* kV2Commands[] = {
        L"exec", L"register", L"unregister", L"sync-config", L"ps",
        L"kill-box", L"kill", L"log", L"info",
    };
    for (const wchar_t* c : kV2Commands) {
        if (cmd == c) {
            int rc = Route(cmd, L"", ctx);
            if (rc == -1 || rc == -2) {
                EmitError(opts, SbieStatus::USAGE, L"unknown command: " + cmd);
                return 2;
            }
            return rc;
        }
    }
    EmitError(opts, SbieStatus::USAGE, L"unknown command: " + cmd);
    return 2;
}

} // namespace sbie::cli
