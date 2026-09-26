// Sandboxie-OSS — sbie-cli/server/LogPump.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// API_GET_MESSAGE 泵（契约：04-modules.md §2.6；00 §8 泵线程）。
// 线程；日志事件写入环形缓冲并向订阅连接推送 {"op":"log.event"} 帧
//（仅 `log watch` 在线订阅时）。
//
// 前置条件（00 §9.2）：本 server 已成功 API_SESSION_LEADER（set 路径要求
// 调用进程非沙箱，02 §3.5）——StartLogPump 由 RunServer 在 leader 确立后
// 调用，别处不得调。
//
// server 写路径波次（04 §12）追加：StartInteractivePump——interactive
// queue（03 §6 *MANPROXY_<session>）聚合泵，把队列请求以 log.event 帧
//（data.interactive=true）推送给 log.watch 订阅者，默认无人值守策略自动
// 应答（retval=0）。其 LPC 全部经 SvcProxy 专职线程（QueueClient 内部走
// SvcClient::Call）。

#pragma once

#include "../../SbieCore/Util/Json.h"

#include <cstddef>

namespace sbie::server {

// 启动泵线程（幂等）。前置：leader 已设、SbieDll 已加载。失败/前置不满足
// 返回 false（log.* op 应答 SERVER_UNAVAILABLE）。
bool StartLogPump();
// 停泵并 join（RunServer 退出路径调用；泵线程自身以 ServerState 停机事件
// 为退出条件，此函数只回收）。
void StopLogPump();
// 泵是否在跑（Dispatcher 的 log.* op 前置检查）
bool LogPumpRunning();
// 环形缓冲当前条数（诊断）
size_t LogRingCount();
// 最近 n 条的 JSON 数组（元素 {msg_num,msgid,pid,time,text}；text 经
// SbieMsg.dll 格式化，与 client 直连文案一致）
json::JsonValue LogDumpJson(size_t lastN);

// interactive queue 聚合泵（幂等启动；leader 确立后与日志泵同点调用）
bool StartInteractivePump();
void StopInteractivePump();
bool InteractivePumpActive();

} // namespace sbie::server
