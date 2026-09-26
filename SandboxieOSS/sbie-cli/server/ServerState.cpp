// Sandboxie-OSS — sbie-cli/server/ServerState.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "ServerState.h"

#include <algorithm>
#include <iterator>

namespace sbie::server {

namespace {
constexpr size_t kPushQueueMax = 256; // 慢订阅者上限（丢最旧）
}

void Connection::EnqueuePush(std::string payload)
{
    std::lock_guard<std::mutex> lk(pushMtx);
    pushQueue.push_back(std::move(payload));
    if (pushQueue.size() > kPushQueueMax)
        pushQueue.erase(pushQueue.begin());
}

size_t Connection::DrainPushes(std::vector<std::string>* out)
{
    std::lock_guard<std::mutex> lk(pushMtx);
    out->insert(out->end(),
                std::make_move_iterator(pushQueue.begin()),
                std::make_move_iterator(pushQueue.end()));
    size_t n = pushQueue.size();
    pushQueue.clear();
    return n;
}

ServerState& ServerState::Get()
{
    static ServerState s;
    return s;
}

bool ServerState::Initialize(ULONG session, ULONG idleTimeoutSec)
{
    session_ = session;
    pid_ = GetCurrentProcessId();
    idleTimeoutSec_ = idleTimeoutSec;
    startTick_ = GetTickCount64();
    idleDeadlineTick_ = startTick_ + (ULONGLONG)idleTimeoutSec_ * 1000;
    if (!stopEvent_)
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); // 手动复位
    return stopEvent_ != nullptr;
}

void ServerState::Stop()
{
    std::lock_guard<std::mutex> lk(mtx_);
    stopping_.store(true, std::memory_order_release);
    if (stopEvent_)
        SetEvent(stopEvent_);
}

ULONGLONG ServerState::UptimeSec() const
{
    return (GetTickCount64() - startTick_) / 1000;
}

ULONGLONG ServerState::IdleRemainingMs() const
{
    // 无限语义：0 = 不退出（00 §6）或有活动客户端
    if (idleTimeoutSec_ == 0)
        return ~0ULL;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!conns_.empty())
        return ~0ULL;
    ULONGLONG now = GetTickCount64();
    if (now >= idleDeadlineTick_)
        return 0;
    return idleDeadlineTick_ - now;
}

void ServerState::Attach(const std::shared_ptr<Connection>& c)
{
    std::lock_guard<std::mutex> lk(mtx_);
    conns_.push_back(c);
}

void ServerState::Detach(const std::shared_ptr<Connection>& c)
{
    std::lock_guard<std::mutex> lk(mtx_);
    conns_.erase(std::remove(conns_.begin(), conns_.end(), c), conns_.end());
    subs_.erase(std::remove(subs_.begin(), subs_.end(), c), subs_.end());
    if (conns_.empty())
        idleDeadlineTick_ = GetTickCount64() + (ULONGLONG)idleTimeoutSec_ * 1000;
}

long ServerState::ActiveClients() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return (long)conns_.size();
}

void ServerState::AddLogSubscriber(const std::shared_ptr<Connection>& c)
{
    c->logSubscriber = true;
    std::lock_guard<std::mutex> lk(mtx_);
    if (std::find(subs_.begin(), subs_.end(), c) == subs_.end())
        subs_.push_back(c);
}

std::vector<std::shared_ptr<Connection>> ServerState::LogSubscribers()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return subs_;
}

} // namespace sbie::server
