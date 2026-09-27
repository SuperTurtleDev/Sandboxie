// Sandboxie-OSS — sbie-cli/main.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// wmain 薄入口：UTF-8 控制台初始化 + argv 转发到 cli::Run。
// 内部入口（非用户命令面）：
//   --version            版本一行（CLI + 驱动/服务尽力探测；dist 校验用）
//   --monitor [--poll-ms N] [--grace-ms N] [--zero-streak N] [--empty-ticks N]
//       公共监视器（monitor/MonitorMain；exec 顺手拉起，亦可手动调参调试）
//   --migrate-templates <Templates.ini> <outDir>
//       V1 模板树迁移工具（M5；生成 SBIE_TEMPLATE_DIR 目录结构）

#include "cli/Cli.h"
#include "cli/Commands.h"
#include "monitor/MonitorMain.h"
#include "../SbieCore/SvcClient/SvcClient.h"
#include "../SbieCore/DriverApi/DriverApi.h"
#include "../SbieCore/Util/Status.h"
#include "../SbieCore/Util/Utf8.h"
#include "../SbieCore/Util/Version.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// sbie-cli <ver> / driver <ver> (abi 0x…, alive) —— 对齐 V1 `version` 命令输出形
int PrintVersion()
{
    std::wstring line = L"sbie-cli " + std::wstring(sbie::kCliVersionW);
    if (sbie::drv::LoadSbieDll()) {
        sbie::drv::VersionInfo v = sbie::drv::GetVersion();
        wchar_t drvAbi[16];
        swprintf_s(drvAbi, L"%X", v.abi);
        line += L"\ndriver " + v.version + L" (abi 0x" + drvAbi + L", "
                + (sbie::drv::DriverAlive() ? L"alive" : L"absent") + L")";
    }
    sbie::util::PrintLineUtf8(sbie::util::WideToUtf8(line));
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    sbie::util::InitUtf8Console();
    // argv[0] 是程序路径，跳过（命令行参数从 argv[1] 起）
    std::vector<std::wstring> args(argv + (argc > 0 ? 1 : 0), argv + argc);

    if (!args.empty() && args[0] == L"--version")
        return PrintVersion();

    if (!args.empty() && args[0] == L"--monitor") {
        sbie::monitor::MonitorOptions mo;
        for (size_t i = 1; i < args.size(); ++i) {
            const long v = (i + 1 < args.size())
                               ? wcstol(args[i + 1].c_str(), nullptr, 10) : -1;
            if (args[i] == L"--poll-ms" && v > 0) {
                mo.pollMs = (DWORD)v;
                ++i;
            } else if (args[i] == L"--grace-ms" && v >= 0) {
                mo.graceMs = (DWORD)v;
                ++i;
            } else if (args[i] == L"--zero-streak" && v > 0) {
                mo.zeroStreak = (int)v;
                ++i;
            } else if (args[i] == L"--empty-ticks" && v > 0) {
                mo.emptyTicks = (int)v;
                ++i;
            }
        }
        return sbie::monitor::RunMonitor(mo);
    }

    if (!args.empty() && args[0] == L"--set-password") {
        // 内部/管理工具：经 SbieSvc SBIE_INI SET_PASSWORD 设置或清除配置
        // 密码（EditPassword）。用法：--set-password <旧密码|-> <新密码|->
        //（"-" = 空密码）。R2/R3 测试与运维用。
        if (args.size() != 3) {
            sbie::util::PrintErrLineUtf8(
                "usage: sbie-cli --set-password <old|-> <new|->");
            return 2;
        }
        std::wstring oldPw = args[1] == L"-" ? std::wstring() : args[1];
        std::wstring newPw = args[2] == L"-" ? std::wstring() : args[2];
        sbie::SbieStatus st = sbie::svc::SvcClient::Instance().SetPassword(
            oldPw, newPw);
        if (st != sbie::SbieStatus::OK) {
            sbie::util::PrintErrLineUtf8("set-password failed: "
                                         + sbie::util::WideToUtf8(
                                               sbie::StatusName(st)));
            return 1;
        }
        sbie::util::PrintLineUtf8("config password updated");
        return 0;
    }

    if (!args.empty() && args[0] == L"--ini-del") {
        // 内部/管理工具：经 SbieSvc 删除一个配置值（带密码验证）。R2/R3
        // 测试与运维用。用法：--ini-del <section> <setting> <value> <pw|->
        if (args.size() != 5) {
            sbie::util::PrintErrLineUtf8(
                "usage: sbie-cli --ini-del <section> <setting> <value> <pw|->");
            return 2;
        }
        std::wstring pw = args[4] == L"-" ? std::wstring() : args[4];
        sbie::SbieStatus st = sbie::svc::SvcClient::Instance().IniSetSetting(
            args[1], args[2], args[3], sbie::svc::SvcClient::SetMode::Delete,
            true, pw);
        if (st != sbie::SbieStatus::OK) {
            sbie::util::PrintErrLineUtf8(
                "ini-del failed: "
                + sbie::util::WideToUtf8(sbie::StatusName(st)));
            return 1;
        }
        sbie::util::PrintLineUtf8("value deleted");
        return 0;
    }

    if (!args.empty() && args[0] == L"--migrate-templates") {
        if (args.size() != 3) {
            sbie::util::PrintErrLineUtf8(
                "usage: sbie-cli --migrate-templates <Templates.ini> <outDir>");
            return 2;
        }
        return sbie::cli::CmdMigrateTemplates(args[1], args[2]);
    }

    return sbie::cli::Run(args);
}
