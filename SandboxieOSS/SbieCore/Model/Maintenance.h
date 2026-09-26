// Sandboxie-OSS — SbieCore/Model/Maintenance.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 组件维护（P1-2，06 缺口表；docs/04 §4.8）：SbieDrv 驱动 / SbieSvc 服务
// 的状态查询与启停。
//   * 服务（SbieSvc）：进程内 Service 控制器（OpenSCManager/StartService/
//     ControlService/QueryServiceStatusEx）——服务名核实于 core 源码
//     common/my_version.h:66 `#define SBIESVC L"SbieSvc"`（注册名，非显示名）；
//   * 驱动（SbieDrv）：经 KmdUtil.exe（GPL core install\kmdutil，运行时调用
//     不受限——命令形态核实于 KmdUtil 解析器 kmdutil.c:194-225 与安装器
//     SandboxieVS.nsi:1619 的 `start SbieSvc` / :1593-1596 的
//     `stop SbieSvc`+`stop SbieDrv` 序列）；进程经 SbieDll_RunFromHome 从
//     Sandboxie 安装目录拉起（KmdUtil.exe 与 SbieDll.dll 同目录分发）。
//   * 启停为**特权操作**（SCM 与 KmdUtil 均需管理员）——非提升进程得
//     ACCESS_DENIED(6)。
// 只在显式命令（sbie maint start/stop）时调用；本模块不做任何自动启停
//（任务边界：server 启动路径禁止触发组件启停）。

#pragma once

#include "../Util/Status.h"

#include <string>

namespace sbie::model {

enum class Component {
    Driver,    // SbieDrv（内核驱动服务）
    Service,   // SbieSvc（Win32 Own Process 服务）
};

struct ComponentStatus {
    bool installed = false;  // 服务注册表中存在
    bool running = false;    // SCM 报告 RUNNING
    std::wstring state;      // "running"/"stopped"/"start_pending"/
                             // "stop_pending"/"paused"/"not_installed"/"unknown"
};

// 查询（无副作用；无特权要求——SC_MANAGER_ENUMERATE_SERVICE）
SbieStatus QueryComponent(Component c, ComponentStatus* out);

// 启动（幂等：已运行视为成功）。Service=StartService；Driver=KmdUtil start。
SbieStatus StartComponent(Component c);

// 停止（幂等：已停止视为成功）。Service=ControlService(STOP)+等待落定；
// Driver=KmdUtil stop（KmdUtil 对 SbieDrv 有专门的卸载序列，kmdutil.c:643）。
SbieStatus StopComponent(Component c);

} // namespace sbie::model
