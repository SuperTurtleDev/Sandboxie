// Sandboxie-OSS — sbie-cli/server/Guardian.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 空箱守护监视器（波次 A：07-P0-2，docs/04 §16）。
//
// 常驻 server 内监视"box 进程数归零"事件（1s 轮询 EnumBoxProcesses——02 文档
// 无驱动侧空箱通知 API，轮询为选型结论；仅当进程数由非零转零时触发，一次
// 生命周期事件）：
//   * OnBoxTerminate 多值命令逐条执行（先于清理，对齐 SandMan OnBoxClosed
//     的执行序，规格 docs/07 §3.1）；
//   * AutoDelete=y   → 清空沙箱内容（含 OnBoxDelete 触发器）；
//   * AutoRemove=y   → 清空内容 + 删除 ini 节；
//   * box 名以 Temp_/Local_Temp_ 开头 → 等同 AutoRemove（一次性沙箱语义，
//     07 记录的规则）。
//
// 开关：--start-server --no-guardians（显式关，最高优先）或全局配置键
// GlobalSettings\GuardiansEnabled（n=关；07 未记录 SandMan 的等价控制键——
// SandMan 作为常驻管理器恒执行守护，无键面，故为本项目自有键，缺省开）。

#pragma once

#include "../../SbieCore/Util/Status.h"

#include <string>

namespace sbie::server {

// 清空模式（Dispatcher 的 box.clean/box.delete 与守护行为共用执行器）
enum class PurgeMode {
    CleanContents,   // 清空 FileRoot 内容、保留根目录（clean 语义）
    RemoveRoot,      // 连根目录递归删除（delete --files 语义）
};

// box 内容清理共享执行器（任意线程可调；纯文件 IO + 驱动缓存读，节删除不在
// 此内——调用方经 SvcProxy 自行处理密码/事务语义）：
//   NeverDelete=y → ACCESS_DENIED；否则（noTriggers=false 时）先逐条执行
//   OnBoxDelete 触发器，再按 mode 清理（Model Boxes.cpp RunBoxTriggers 的
//   执行语义：逐条、≤15s、失败继续）。
SbieStatus ExecuteBoxPurge(const std::wstring& name, PurgeMode mode,
                           bool noTriggers);

// 监视器生命周期（RunServer INIT 调 Start、EXIT 调 Stop；均幂等）。
// forceOff = --no-guardians。返回 false：forceOff / 配置键关 / 驱动缺席 /
// 线程创建失败（调用方 DiagServer 区分文案）。
bool StartGuardians(bool forceOff);
void StopGuardians();
bool GuardiansActive();
long long GuardiansFired();   // 已处理的空箱事件计数（status op 呈现）

} // namespace sbie::server
