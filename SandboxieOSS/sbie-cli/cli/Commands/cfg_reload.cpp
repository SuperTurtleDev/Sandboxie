// Sandboxie-OSS — sbie-cli/cli/Commands/cfg_reload.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie cfg reload [--reconfigure]（04-modules.md §4.5 维护命令）。
//
// 语义（02-driver-api.md §3.4 实读核实）：SbieApi_ReloadConf(session_id=-1 当前,
// flags)——驱动重读 Sandboxie.ini + Templates.ini 进缓存（conf/boxes 全量生效）；
// flags 仅 SBIE_CONF_FLAG_RECONFIGURE=0x1。非沙箱限定（conf.c:1701-1702），
// 不经 SbieSvc（04 §5：可降级直连；与 SbieSvc 写路径自带的 refresh 是同一
// 驱动入口，勿与 set --no-refresh 混淆——04 §8.2）。

#include "CfgTemplateCommands.h"
#include "IpcRoute.h"
#include "../Output.h"
#include "../ServerConnect.h"
#include "../../ipcc/SbieIpc.h"

#include "../../../SbieCore/DriverApi/DriverApi.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace sbie::cli {

int CmdCfgReload(const CommandContext& ctx)
{
    GlobalOptions o;
    std::vector<std::wstring> rest = cfgtmpl::AbsorbTrailingGlobals(ctx, &o);

    bool reconfigure = false;
    for (const auto& a : rest) {
        if (a == L"--reconfigure") {
            reconfigure = true;
        } else {
            return EmitError(o, SbieStatus::USAGE, L"unknown option: " + a);
        }
    }

    // IPC 优先（cfg.reload；server 与直连同走 SbieApi_ReloadConf），降级直连
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"reconfigure", reconfigure);
        ipcroute::Result r = ipcroute::Invoke(
            o, ipc::kOpCfgReload, params, true,
            [reconfigure](const GlobalOptions& op,
                          const json::JsonValue& /*data*/) {
                EmitMessage(op, L"configuration reloaded"
                    + (reconfigure ? std::wstring(L" (reconfigured)")
                                   : std::wstring()));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    srvconn::NoteDegraded();

    if (!drv::LoadSbieDll(o.sbieDllPath))
        return EmitError(o, SbieStatus::ERR_SBIEDLL, drv::LastLoadError());
    if (!drv::DriverAlive())
        return EmitError(o, SbieStatus::DRIVER_UNAVAILABLE, L"driver not running");

    SbieStatus st = drv::ReloadConf(0, reconfigure);
    if (!Ok(st)) {
        wchar_t nt[16] = L"";
        swprintf_s(nt, L"0x%08X", (unsigned)drv::LastNtStatus());
        std::wstring what = reconfigure ? L"reconfigure, conf+boxes"
                                        : L"conf+boxes";
        return EmitError(o, st,
                         L"SbieApi_ReloadConf failed (" + what + L")", nt);
    }
    EmitMessage(o, L"configuration reloaded"
                        + (reconfigure ? std::wstring(L" (reconfigured)")
                                       : std::wstring()));
    return 0;
}

} // namespace sbie::cli
