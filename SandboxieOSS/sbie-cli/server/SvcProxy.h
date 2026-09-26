// Sandboxie-OSS — sbie-cli/server/SvcProxy.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// SbieSvc LPC 专职线程（server 内部实现，非对外契约）。
//
// 背景（03-svc-protocol.md §1 / SvcClient.h 头注）：LPC 端口句柄绑定首次
// NtConnectPort 的线程。client（M1）短命令在主线程串行调用天然满足；server
// 是多线程（每 client 一个工作线程 + 泵线程），因此把全部 SvcClient 调用
// 收敛到本专职线程的队列上串行执行。
//
// 用法：Dispatcher 内一切触到 svc::SvcClient 的 handler 一律写成
//   SvcCall([&]{ return svc::SvcClient::Instance().Xxx(...); });
// 捕获局部变量收集结果。

#pragma once

#include "../../SbieCore/Util/Status.h"

#include <functional>

namespace sbie::server {

// RunServer 生命周期内调用；幂等
void StartSvcProxy();
void StopSvcProxy();   // 排空队列后结束线程（在 worker 收尾之后调用）

// 把 task 交给专职线程执行并等待结果。代理未启动/已停/等待超时 →
// ERR_SVC_TRANSPORT（调用方按 SbieSvc 不可用处理）。
SbieStatus SvcCall(const std::function<SbieStatus()>& task);

} // namespace sbie::server
