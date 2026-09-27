// Sandboxie-OSS — sbie-cli/server/TracePump.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 泵循环（00 §8 框架，LogPump 同款）：MonitorGet2 批量拉取 → 解码 → 环形
// 缓冲 → 订阅连接推送。等待退避 10ms→250ms；STATUS_MORE_ENTRIES 立即续拉
//（驱动端单次装满即返回，QSbieAPI GetMonitor 返回值同语义，SbieAPI.cpp:3118）。
// 解码/类型文本/时间格式化全部复用 Model/Monitor（与 client 直连路径同构，
// 消除两侧文案漂移）。

#include "TracePump.h"
#include "ServerState.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/Monitor.h"
#include "../../SbieCore/Util/Utf8.h"

// vendor：MONITOR_TYPE_MASK
#include "api_defs.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace sbie::server {

namespace {

// trace.dump 环形缓冲容量（监控流量远高于日志消息；SandMan TraceView 保留
// 百万级，CLI dump 场景 8196 已足覆盖一次重度会话）
constexpr size_t kTraceRingMax = 8196;

// GET2 拉取缓冲（1 MiB，QSbieAPI/Model 同容量）
constexpr ULONG kFetchBufferBytes = 256 * 4096;

// 防病态积压的单轮续拉上限
constexpr int kFetchRoundsMax = 64;

struct TraceEntryRec {
    model::MonitorEntry e;
    std::wstring box;    // pid 解析结果（未知 "-"）
};

std::mutex& RingMtx()
{
    static std::mutex m;
    return m;
}
std::deque<TraceEntryRec>& Ring()
{
    static std::deque<TraceEntryRec> r;
    return r;
}

std::atomic<bool>& Running()
{
    static std::atomic<bool> r{ false };
    return r;
}
std::thread& PumpThread()
{
    static std::thread t;
    return t;
}
// 由本泵开启的监控（Stop 时关回；他者已开的不动）
bool& EnabledByUs()
{
    static bool b = false;
    return b;
}

json::JsonValue EntryJson(const TraceEntryRec& r)
{
    json::JsonValue o = json::JsonValue::Object();
    o.set(L"type", json::JsonValue(model::MonitorTypeName(r.e.type)));
    o.set(L"type_code", json::JsonValue((long long)(r.e.type & MONITOR_TYPE_MASK)));
    o.set(L"status", json::JsonValue(model::MonitorStatusText(r.e.type)));
    o.set(L"pid", json::JsonValue((long long)r.e.pid));
    o.set(L"tid", json::JsonValue((long long)r.e.tid));
    o.set(L"box", json::JsonValue(r.box));
    o.set(L"time", json::JsonValue(model::MonitorTimeText(r.e.timestamp)));
    o.set(L"timestamp", json::JsonValue((long long)r.e.timestamp));
    o.set(L"name",
          json::JsonValue(r.e.strings.size() > 0 ? r.e.strings[0]
                                                 : std::wstring()));
    o.set(L"message",
          json::JsonValue(r.e.strings.size() > 1 ? r.e.strings[1]
                                                 : std::wstring()));
    return o;
}

// 推送帧序列化 + 广播（log.event 先例：入 pushQueue，由该连接工作线程在
// 订阅者轮询模式写出；慢消费者丢最旧由 Connection::EnqueuePush 兜底）
std::string BuildPushPayload(const TraceEntryRec& r)
{
    json::JsonValue push = json::JsonValue::Object();
    push.set(L"op", json::JsonValue(L"trace.event"));
    push.set(L"data", EntryJson(r));
    return json::SerializeUtf8(push);
}

void Broadcast(const std::string& payload)
{
    for (auto& c : ServerState::Get().TraceSubscribers())
        c->EnqueuePush(payload);
}

void PumpLoop()
{
    std::vector<unsigned char> buf(kFetchBufferBytes);
    model::MonitorPidResolver resolver;   // pid→box 缓存（泵线程独占）
    HANDLE stop = ServerState::Get().StopEvent();
    DWORD idleMs = 10;

    for (;;) {
        if (ServerState::Get().Stopping())
            break;

        bool drained = false;
        bool degraded = false;
        for (int round = 0; round < kFetchRoundsMax; ++round) {
            ULONG len = kFetchBufferBytes;
            bool more = false;
            drv::MonitorFetch f = drv::MonitorGet2(buf.data(), &len, &more);
            if (f == drv::MonitorFetch::Empty) {
                drained = true;   // 环空
                break;
            }
            if (f != drv::MonitorFetch::Ok) {
                // NotEnabled（监控被并发关闭）/Error：降退避轮询，期间由
                // Start 语义外的并发干扰自愈（驱动不自行关环，常态不可达）
                degraded = true;
                break;
            }
            std::vector<model::MonitorEntry> entries;
            model::DecodeMonitorBuffer(buf.data(), len, &entries);
            for (auto& e : entries) {
                TraceEntryRec rec;
                rec.e = std::move(e);
                resolver.Lookup(rec.e.pid, &rec.box);
                // 序列化在环锁内完成（rec 已入环；读取仅本线程写、并发读
                // 需锁保护——TraceDumpJson 同锁）
                std::string payload;
                {
                    std::lock_guard<std::mutex> lk(RingMtx());
                    Ring().push_back(std::move(rec));
                    while (Ring().size() > kTraceRingMax)
                        Ring().pop_front();
                    payload = BuildPushPayload(Ring().back());
                }
                Broadcast(payload);
            }
            if (!more) {
                drained = true;
                break;
            }
            // STATUS_MORE_ENTRIES：立即续拉（排干积压）
        }

        if (drained && !degraded)
            idleMs = idleMs >= 250 ? 250 : idleMs * 2;   // LogPump 同款退避
        else
            idleMs = 1000;   // 异常态：慢轮询防热转
        if (WaitForSingleObject(stop, idleMs) == WAIT_OBJECT_0)
            break;
    }
}

} // namespace

bool StartTracePump()
{
    if (Running().exchange(true))
        return true;
    if (!drv::Loaded() || !drv::ApiP()->SbieApi_Ioctl
        || !ServerState::Get().StopEvent()) {
        Running().store(false);
        return false;
    }
    // 开启本会话监控（他者已开则不接管关闭权）
    ULONG old = 0;
    if (drv::MonitorControl(nullptr, &old) != SbieStatus::OK) {
        Running().store(false);
        return false;
    }
    EnabledByUs() = (old == 0);
    if (old == 0) {
        ULONG on = 1;
        if (drv::MonitorControl(&on, nullptr) != SbieStatus::OK) {
            EnabledByUs() = false;
            Running().store(false);
            return false;
        }
    }
    try {
        PumpThread() = std::thread(PumpLoop);
    } catch (...) {
        if (EnabledByUs()) {
            ULONG off = 0;
            (void)drv::MonitorControl(&off, nullptr);
            EnabledByUs() = false;
        }
        Running().store(false);
        return false;
    }
    ServerState::Get().SetTracePumpActive(true);
    return true;
}

void StopTracePump()
{
    if (PumpThread().joinable())
        PumpThread().join();
    if (EnabledByUs()) {
        ULONG off = 0;
        (void)drv::MonitorControl(&off, nullptr);
        EnabledByUs() = false;
    }
    Running().store(false);
    ServerState::Get().SetTracePumpActive(false);
}

bool TracePumpRunning()
{
    return ServerState::Get().TracePumpActive();
}

size_t TraceRingCount()
{
    std::lock_guard<std::mutex> lk(RingMtx());
    return Ring().size();
}

json::JsonValue TraceDumpJson(size_t lastN, const std::wstring& filterBox,
                              ULONG filterType, ULONG filterPid)
{
    json::JsonValue rows = json::JsonValue::Array();
    std::lock_guard<std::mutex> lk(RingMtx());
    // 从尾部向前收集 lastN 条（过滤后），再正序输出
    std::vector<const TraceEntryRec*> picked;
    picked.reserve(lastN);
    for (auto it = Ring().rbegin();
         it != Ring().rend() && picked.size() < lastN; ++it) {
        if (!filterBox.empty()
            && (it->box != filterBox))
            continue;
        if (filterType && (it->e.type & MONITOR_TYPE_MASK) != filterType)
            continue;
        if (filterPid && it->e.pid != filterPid)
            continue;
        picked.push_back(&*it);
    }
    for (auto it = picked.rbegin(); it != picked.rend(); ++it)
        rows.pushBack(EntryJson(**it));
    return rows;
}

} // namespace sbie::server
