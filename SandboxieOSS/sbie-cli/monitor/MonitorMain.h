// Sandboxie-OSS — sbie-cli/monitor/MonitorMain.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 公共 monitor（docs/10-v2-design.md §8）：
//   * 每登录会话单实例（命名互斥体 Local\SbieOSS_Monitor_S<sid>）；
//     由 exec/register 顺手拉起；任务表空且持续 empty-ticks 后退出。
//   * 纯轮询（拍板 D1）：不做驱动日志泵/leader 仲裁——Api_GetMessage 要求
//     会话 leader（api.c:725-733）且驱动无进程退出日志（只有启动 MSG_1399，
//     process.c:1399），轮询是唯一可靠信号源。
//   * 任务 = monitors\<box>.task 文件；每 tick：重扫目录对账 → 逐盒数进程 →
//     归零边沿（连续 K 次 + 从未起过进程的孤儿任务过宽限期）→ teardown =
//     删 running.lock → 删缓存 → SbieApi_ReloadConf → 等注册消失 → 删任务文件。
//   * 它不是 server：无管道/无命令枢纽/无 LPC；唯一输入任务目录。

#pragma once

#include <windows.h>

#include <string>

namespace sbie::monitor {

struct MonitorOptions {
    DWORD pollMs = 250;        // 轮询间隔
    DWORD graceMs = 5000;      // 从未见过进程的任务的宽限期（spawn 失败孤儿）
    int zeroStreak = 2;        // 非零→零的确认次数（边沿去抖）
    int emptyTicks = 4;        // 任务表空的退出阈值（tick 数）
    DWORD serviceGraceMs = 15000;  // B1：仅剩自举服务（上游 linger 竞态滞留）
                                   // 的宽限，超时 KillAll 清场后 teardown
};

// monitor 主循环（阻塞直至退出条件）。返回进程退出码（恒 0）。
int RunMonitor(const MonitorOptions& opts);

// CLI 侧：确保本会话 monitor 在跑（不存在则拉起 DETACHED 实例；幂等）。
// exePath = 当前 sbie-cli.exe 全路径（CreateProcess 用）。
bool EnsureMonitorRunning(const std::wstring& exePath);

} // namespace sbie::monitor
