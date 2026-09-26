// Sandboxie-OSS — sbie-cli/server/ServerState.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// server 进程级共享状态（server 内部实现，非对外契约）：
//   * 停机事件/标志（优雅停机与空闲退出共用）
//   * 活动连接登记（计数 + status op 的 server 行 + 空闲计时）
//   * log.watch 订阅连接集合（LogPump 推送目标）
// 单实例（RunServer Initialize；进程内唯一）。

#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sbie::server {

// 单个已连接 client 的会话上下文（工作线程拥有存续期）。
// 写路径归工作线程独占：同步句柄不支持跨线程并发读写（log.event 推送经
// pushQueue 转交，见 ServerMain WorkerMain 的订阅者轮询模式）。
struct Connection {
    HANDLE pipe = nullptr;
    ULONG clientPid = 0;
    ULONG clientSession = 0;
    bool logSubscriber = false;   // log.watch 成功后置位

    explicit Connection(HANDLE h) : pipe(h) {}

    // log.event 推送入队（LogPump 调；慢消费者丢最旧）
    void EnqueuePush(std::string payload);
    // 取走全部待推送（工作线程调）
    size_t DrainPushes(std::vector<std::string>* out);

private:
    std::mutex pushMtx;
    std::vector<std::string> pushQueue;
};

class ServerState {
public:
    static ServerState& Get();

    // RunServer 开头调用一次；失败（事件创建失败）→ 放弃启动
    bool Initialize(ULONG session, ULONG idleTimeoutSec);

    // 请求停机（server.shutdown / 空闲超时）。幂等。
    void Stop();
    bool Stopping() const { return stopping_.load(std::memory_order_acquire); }
    HANDLE StopEvent() const { return stopEvent_; }

    ULONG Session() const { return session_; }
    DWORD Pid() const { return pid_; }
    ULONG IdleTimeoutSec() const { return idleTimeoutSec_; }
    ULONGLONG UptimeSec() const;
    // 空闲剩余毫秒；~0ULL = 无限（有活动客户端或 idleTimeoutSec==0）
    ULONGLONG IdleRemainingMs() const;

    // 连接登记/注销（工作线程首尾调用）；Detach 在最后一个连接移除时武装
    // 空闲计时（00 §6）
    void Attach(const std::shared_ptr<Connection>& c);
    void Detach(const std::shared_ptr<Connection>& c);
    long ActiveClients() const;

    // log 订阅（LogPump 推送目标快照）
    void AddLogSubscriber(const std::shared_ptr<Connection>& c);
    std::vector<std::shared_ptr<Connection>> LogSubscribers();

    void SetLogPumpActive(bool on) { logPumpActive_.store(on); }
    bool LogPumpActive() const { return logPumpActive_.load(); }

private:
    ServerState() = default;

    mutable std::mutex mtx_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> stopping_{ false };
    std::atomic<bool> logPumpActive_{ false };
    ULONG session_ = 0;
    DWORD pid_ = 0;
    ULONG idleTimeoutSec_ = 0;
    ULONGLONG startTick_ = 0;
    ULONGLONG idleDeadlineTick_ = 0;   // 最后一个客户端断开后 armed
    std::vector<std::shared_ptr<Connection>> conns_;
    std::vector<std::shared_ptr<Connection>> subs_;
};

} // namespace sbie::server
