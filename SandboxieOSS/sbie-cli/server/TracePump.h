// Sandboxie-OSS — sbie-cli/server/TracePump.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// API_MONITOR_GET2 泵（波 D1，契约：04-modules.md §20；00 §8 泵线程）。
// 常驻线程：MonitorControl 开启本会话监控 → 循环 MonitorGet2 批量拉取 →
// 解码（Model/Monitor）→ pid→box 解析 → 环形缓冲（trace.dump 数据源）→
// {"op":"trace.event"} 帧推送 trace.watch 订阅连接（推送框架复用 LogPump
// 模式：入 Connection::EnqueuePush，由该连接工作线程在订阅者轮询模式写出）。
//
// 架构决策（04 §20）：独立 TracePump 而非并入 LogPump——两者事件源、启停
// 前置（监控不要求 session leader）、推送订阅集合与节奏全不同，旁路并泵会
// 让 LogPump 持有双份环锁/双份订阅广播逻辑；共享的只有 Connection 推送队列
// 与 ServerState 订阅登记模式。
//
// 前置：SbieDll 已加载 + 驱动在场（MonitorControl/GET2 仅要求非沙箱，
// 02 §6——不依赖 session leader）。StartTracePump 由 RunServer 在驱动探测
// 后调用；若监控已被他者（SandMan）开启，本泵只读取不接管关闭权。

#pragma once

#include "../../SbieCore/Util/Json.h"

#include <cstddef>
#include <string>

namespace sbie::server {

// 启动泵线程（幂等）。前置：SbieDll 已加载、驱动在场、停机事件已建。失败
// 返回 false（trace.* op 应答 SERVER_UNAVAILABLE）。
bool StartTracePump();
// 停泵并 join（RunServer 退出路径调用）；由本泵开启的监控在此关回。
void StopTracePump();
// 泵是否在跑（Dispatcher 的 trace.* op 前置检查）
bool TracePumpRunning();
// 环形缓冲当前条数（诊断）
size_t TraceRingCount();

// 环形缓冲近期条目（server 侧过滤后）。filterBox 空 = 不过滤；filterType 0 =
// 不过滤（按 MONITOR_TYPE_MASK 匹配）；filterPid 0 = 不过滤。
// 元素 {type,type_code,status,pid,tid,box,time,timestamp,name,message}。
json::JsonValue TraceDumpJson(size_t lastN, const std::wstring& filterBox,
                              ULONG filterType, ULONG filterPid);

} // namespace sbie::server
