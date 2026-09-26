// Sandboxie-OSS — sbie-cli/server/SvcProxy.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "SvcProxy.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace sbie::server {

namespace {

constexpr DWORD kCallTimeoutMs = 30000; // 单请求兜底（RunSandboxed 等慢操作）

struct Item {
    std::function<SbieStatus()> task;
    HANDLE done = nullptr;   // 自动复位事件；由本结构体独有（析构关闭）
    SbieStatus result = SbieStatus::ERR_SVC_TRANSPORT;
    std::atomic<bool> abandoned{ false }; // 调用方超时放弃（仅诊断语义）

    ~Item()
    {
        if (done)
            CloseHandle(done);
    }
};

// 所有权：队列/专职线程与调用方各持 shared_ptr——超时放弃后 Item 仍存活到
// 专职线程 SetEvent 完毕，杜绝悬垂句柄（SetEvent 落在已释放句柄上的竞态）。

struct Queue {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Item>> pending;
    bool quit = false;
};

Queue& Q()
{
    static Queue q;
    return q;
}

std::thread& Worker()
{
    static std::thread t;
    return t;
}

std::atomic<bool>& Running()
{
    static std::atomic<bool> r{ false };
    return r;
}

void ProxyLoop()
{
    // 本线程是 SbieSvc LPC 端口的第一（且唯一）使用者：SvcClient::Instance()
    // 的首次 NtConnectPort 发生在这里，线程亲和即落定（03 §1）。
    Queue& q = Q();
    for (;;) {
        std::shared_ptr<Item> it;
        {
            std::unique_lock<std::mutex> lk(q.mtx);
            q.cv.wait(lk, [&q] { return q.quit || !q.pending.empty(); });
            if (q.pending.empty())
                break; // quit 且排空
            it = q.pending.front();
            q.pending.pop_front();
        }
        if (it->task)
            it->result = it->task();
        SetEvent(it->done); // Item 此后交给调用方（或其放弃后自然析构）
    }
}

} // namespace

void StartSvcProxy()
{
    if (Running().exchange(true))
        return;
    {
        Queue& q = Q();
        std::lock_guard<std::mutex> lk(q.mtx);
        q.quit = false;
    }
    Worker() = std::thread(ProxyLoop);
}

void StopSvcProxy()
{
    if (!Running().exchange(false))
        return;
    {
        Queue& q = Q();
        std::lock_guard<std::mutex> lk(q.mtx);
        q.quit = true;
    }
    Q().cv.notify_all();
    if (Worker().joinable())
        Worker().join();
}

SbieStatus SvcCall(const std::function<SbieStatus()>& task)
{
    if (!Running().load() || !task)
        return SbieStatus::ERR_SVC_TRANSPORT;

    auto it = std::make_shared<Item>();
    it->task = task;
    it->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!it->done)
        return SbieStatus::GENERIC;

    {
        Queue& q = Q();
        std::lock_guard<std::mutex> lk(q.mtx);
        if (q.quit)
            return SbieStatus::ERR_SVC_TRANSPORT;
        q.pending.push_back(it);
    }
    Q().cv.notify_one();

    if (WaitForSingleObject(it->done, kCallTimeoutMs) != WAIT_OBJECT_0) {
        // 超时放弃：专职线程稍后照常 SetEvent（Item 经 shared_ptr 存活），
        // 本侧按 SbieSvc 不可用返回
        it->abandoned.store(true);
        return SbieStatus::ERR_SVC_TRANSPORT;
    }
    return it->result;
}

} // namespace sbie::server
