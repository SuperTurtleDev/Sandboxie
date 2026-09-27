// Sandboxie-OSS — sbie-cli/server/LogPump.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 泵循环（00 §8）：SbieApi_GetMessage → 环形缓冲 → 订阅连接推送。
// 等待退避 10ms→250ms（对齐 QSbieAPI run() 的 Idle 递增，SbieAPI.cpp:711-758）。
//
// server 写路径波次（04 §12）：
//   * 文案：SbieDll_FormatMessage 数组变体（SbieMsg.dll 消息表，02 §3.7；
//     08-P1-2 绑定）——与 client 直连 FormatText 同构（表键 = 完整 msgCode；
//     "%0" 类消息/无表项回退插入串直拼；表文案自带的 "SBIE%04u " 前缀剥去），
//     消除 §11 遗留 4 的"server 文案=插入串拼接"差异；
//   * interactive queue 事件源（03 §6）：专职泵线程聚合
//     *MANPROXY_<session> 队列请求为 log.event 推送（interactive=true
//     字段，向后兼容加字段），按默认无人值守策略自动应答（retval=0）。
//     QueueClient 的 LPC 全部经 SvcClient::Call——Start/Drain/Reconnect
//     一律包进 SvcCall 专职线程（线程模型坑：直接在本泵线程调会违反
//     03 §1 线程亲和）。

#include "LogPump.h"
#include "ServerState.h"
#include "SvcProxy.h"
#include "../ipcc/SbieIpc.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/QueueClient/QueueClient.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sbie::server {

namespace {

constexpr size_t kRingMax = 512;          // log.dump 环形缓冲容量
constexpr ULONG kStatusAccessDenied = 0xC0000022UL;

struct LogEntry {
    ULONG msgNum = 0;
    ULONG msgId = 0;
    ULONG pid = 0;
    std::wstring time;   // 本地 "hh:mm:ss"
    std::wstring text;   // SbieMsg.dll 文案（回退：插入串直拼）
};

std::mutex& RingMtx()
{
    static std::mutex m;
    return m;
}
std::deque<LogEntry>& Ring()
{
    static std::deque<LogEntry> r;
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

json::JsonValue EntryJson(const LogEntry& e)
{
    json::JsonValue o = json::JsonValue::Object();
    o.set(L"msg_num", json::JsonValue((long long)e.msgNum));
    o.set(L"msgid", json::JsonValue((long long)e.msgId));
    o.set(L"pid", json::JsonValue((long long)e.pid));
    o.set(L"time", json::JsonValue(e.time));
    o.set(L"text", json::JsonValue(e.text));
    return o;
}

std::wstring LocalTimeText()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[24];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 一条日志记录的缓冲区布局：msgid + pid 已由出参承接，缓冲区本身是若干
// \0 结尾 WCHAR 串；首段空占位后逐段收集，遇空段即止（与 client
// FetchLogEntry 同构——插入串两侑一致才能共用 FormatMessage 语义）
std::vector<std::wstring> SplitInserts(const WCHAR* buf, size_t cch)
{
    std::vector<std::wstring> ins;
    const WCHAR* p = buf;
    const WCHAR* end = buf + cch;
    while (p < end) {
        const WCHAR* seg = p;
        while (p < end && *p)
            ++p;
        if (seg == p)
            break;   // 空段 = 结束
        ins.emplace_back(seg, (size_t)(p - seg));
        ++p; // 跳过 NUL
    }
    return ins;
}

std::wstring JoinInserts(const std::vector<std::wstring>& ins)
{
    std::wstring text;
    for (const auto& i : ins) {
        if (!text.empty())
            text += L' ';
        text += i;
    }
    return text;
}

// SbieMsg.dll 文案（与 client 直连 FormatText 同构，log_cmd.cpp 对照）：
// 表键 = 完整 msgCode（含严重度/设施位）；"%0" 消息（如 1399）与无表项
// 回退插入串直拼；表文案自带 "SBIE%04u " 前缀剥去避免与行首重复。
// 数组变体 SbieDll_FormatMessage（08-P1-2）：%N ↔ ins[N]，前 5 个插入串
// 置于 ins[1..5]——修复 ≥3 插入串在渲染文本中丢失（消息表 %3×数十、%4×4）。
std::wstring FormatEntryText(ULONG msgId, const std::vector<std::wstring>& ins)
{
    drv::Api* api = drv::ApiP();
    if (!api || !api->SbieDll_FormatMessage)
        return JoinInserts(ins);
    const WCHAR* args[6] = {};   // support.c 内部同容量；空槽必须 nullptr
    for (size_t i = 0; i < ins.size() && i < 5; ++i)
        args[i + 1] = ins[i].c_str();
    WCHAR* p = api->SbieDll_FormatMessage(msgId, args);
    std::wstring s;
    if (p) {
        s = p;
        LocalFree(p);   // FormatMessage ALLOCATE_BUFFER / LocalAlloc 同族
    }
    if (s.empty() || s.rfind(L"err=", 0) == 0) {
        s = JoinInserts(ins);
    } else {
        wchar_t want[10];
        swprintf_s(want, L"SBIE%04u ", msgId & 0xFFFF);
        if (s.rfind(want, 0) == 0)
            s.erase(0, wcslen(want));
    }
    return s;
}

// 推送到全部订阅连接：入 pushQueue，由该连接的工作线程在轮询模式写出
//（同步句柄不支持跨线程并发读写——server 波次实测坑，见 00 §验收记录）
void PushToSubscribers(const LogEntry& e)
{
    json::JsonValue push = json::JsonValue::Object();
    push.set(L"op", json::JsonValue(L"log.event"));
    push.set(L"data", EntryJson(e));
    const std::string payload = json::SerializeUtf8(push);

    for (auto& c : ServerState::Get().LogSubscribers())
        c->EnqueuePush(payload);
}

void PumpLoop()
{
    drv::Api* api = drv::ApiP();
    if (!api || !api->SbieApi_GetMessage)
        return;

    const ULONG session = ServerState::Get().Session();
    HANDLE stop = ServerState::Get().StopEvent();
    ULONG msgNum = 0;         // 游标（02 §3.5：从 0 开始每会话独立）
    DWORD idleMs = 10;        // 退避
    WCHAR buf[2048];          // 4KB（02 §3.5 建议）

    for (;;) {
        if (ServerState::Get().Stopping())
            break;

        ULONG msgId = 0, pid = 0;
        ULONG rc = api->SbieApi_GetMessage(&msgNum, session, &msgId, &pid,
                                           buf, sizeof(buf));
        if (rc == 0) {
            LogEntry e;
            e.msgNum = msgNum;
            e.msgId = msgId;
            e.pid = pid;
            e.time = LocalTimeText();
            e.text = FormatEntryText(msgId, SplitInserts(buf, std::size(buf)));
            {
                std::lock_guard<std::mutex> lk(RingMtx());
                Ring().push_back(e);
                while (Ring().size() > kRingMax)
                    Ring().pop_front();
            }
            PushToSubscribers(e);
            idleMs = 10; // 有流量：回到快轮询
            continue;    // 立刻尝试取下一条（排空积压）
        }

        if (rc == kStatusAccessDenied) {
            // 非本会话 leader（api.c:725-733）——理论不可达（启动时已 set），
            // 慢轮询兜底避免热转
            idleMs = 1000;
        } else {
            // 无新消息（STATUS_NO_MORE_ENTRIES 等告警）或瞬时错误
            idleMs = idleMs >= 250 ? 250 : idleMs * 2;
        }
        if (WaitForSingleObject(stop, idleMs) == WAIT_OBJECT_0)
            break;
    }
}

// ---------------------------------------------------------------------------
// interactive queue 聚合泵（03 §6；事件源 2）。决策为默认无人值守策略
//（retval=0 拒绝）——经 server 的 y/N 交互需要请求-应答 op，后续波次。
// ---------------------------------------------------------------------------

std::atomic<bool>& IqRunning()
{
    static std::atomic<bool> r{ false };
    return r;
}
std::thread& IqThread()
{
    static std::thread t;
    return t;
}
std::atomic<bool>& IqActive()
{
    static std::atomic<bool> a{ false };
    return a;
}

std::wstring HumanSize(unsigned long long bytes)
{
    const wchar_t* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0)
        swprintf_s(buf, L"%llu B", bytes);
    else
        swprintf_s(buf, L"%.1f %ls", v, unit[u]);
    return buf;
}

// interactive 事件 → log.event 帧（data 加 interactive/kind/file_path/
// file_size 字段，向后兼容——旧 client 仅读 msgid/pid/time/text）
void PushInteractiveEvent(const queue::InteractiveRequest& ir)
{
    const int kindFileMigration = 1;   // MAN_FILE_MIGRATION
    std::wstring line;
    if (ir.kind == kindFileMigration)
        line = L"interactive: file migration request: " + ir.filePath
             + L" (" + HumanSize(ir.fileSize) + L") from pid "
             + std::to_wstring(ir.clientPid);
    else if (ir.kind == 2 /*MAN_INET_BLOCKADE*/)
        line = L"interactive: internet blockade notification from pid "
             + std::to_wstring(ir.clientPid);
    else
        line = L"interactive: unknown request kind "
             + std::to_wstring(ir.kind);
    line += L" (auto-denied)";   // 默认无人值守策略（03 §6）

    json::JsonValue d = json::JsonValue::Object();
    d.set(L"interactive", json::JsonValue(true));
    d.set(L"kind", json::JsonValue((long long)ir.kind));
    d.set(L"pid", json::JsonValue((long long)ir.clientPid));
    if (ir.kind == kindFileMigration) {
        d.set(L"file_path", json::JsonValue(ir.filePath));
        d.set(L"file_size", json::JsonValue((long long)ir.fileSize));
    }
    d.set(L"time", json::JsonValue(LocalTimeText()));
    d.set(L"text", json::JsonValue(line));

    json::JsonValue push = json::JsonValue::Object();
    push.set(L"op", json::JsonValue(L"log.event"));
    push.set(L"data", std::move(d));
    const std::string payload = json::SerializeUtf8(push);
    for (auto& c : ServerState::Get().LogSubscribers())
        c->EnqueuePush(payload);
}

void InteractivePumpLoop()
{
    queue::InteractiveSession iq;
    HANDLE stop = ServerState::Get().StopEvent();

    for (;;) {
        if (ServerState::Get().Stopping())
            break;

        if (!iq.active()) {
            // QueueClient::Create 的 LPC 经 SvcClient::Call → 必须在
            // SvcProxy 专职线程上执行（03 §1 线程亲和）
            SbieStatus st = SvcCall([&iq] { return iq.Start(); });
            if (!Ok(st)) {
                // SbieSvc 缺席/队列被占：退避重试（不热转）
                IqActive().store(false);
                if (WaitForSingleObject(stop, 5000) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            IqActive().store(true);
        }

        HANDLE hs[2] = { stop, iq.event() };
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 1000);
        if (w == WAIT_OBJECT_0)
            break;
        if (w == WAIT_OBJECT_0 + 1) {
            // 事件置位：抽干（GETREQ/PutRpl 同经 SvcClient::Call → SvcCall）
            SbieStatus st = SvcCall([&iq] {
                return iq.Drain(
                    [](const queue::InteractiveRequest& ir,
                       queue::InteractiveReply*) {
                        PushInteractiveEvent(ir);
                        return false;   // false = 默认自动应答（retval=0）
                    });
            });
            if (st == SbieStatus::ERR_SVC_TRANSPORT) {
                // 传输断线：本线程 Reset（无并发等待者——等待点仅在下方，
                // 此刻已返回），下轮循环经 SvcCall 重建（5s 退避）
                iq.Reset();
            }
        }
    }
    iq.Reset();
    IqActive().store(false);
}

} // namespace

bool StartLogPump()
{
    if (Running().exchange(true))
        return true;
    if (!drv::Loaded() || !drv::ApiP()->SbieApi_GetMessage
        || !ServerState::Get().StopEvent()) {
        Running().store(false);
        return false;
    }
    try {
        PumpThread() = std::thread(PumpLoop);
    } catch (...) {
        Running().store(false);
        return false;
    }
    ServerState::Get().SetLogPumpActive(true);
    return true;
}

void StopLogPump()
{
    if (PumpThread().joinable())
        PumpThread().join();
    Running().store(false);
    ServerState::Get().SetLogPumpActive(false);
}

bool StartInteractivePump()
{
    if (IqRunning().exchange(true))
        return true;
    if (!ServerState::Get().StopEvent()) {
        IqRunning().store(false);
        return false;
    }
    try {
        IqThread() = std::thread(InteractivePumpLoop);
    } catch (...) {
        IqRunning().store(false);
        return false;
    }
    return true;
}

void StopInteractivePump()
{
    if (IqThread().joinable())
        IqThread().join();
    IqRunning().store(false);
}

bool InteractivePumpActive()
{
    return IqActive().load();
}

bool LogPumpRunning()
{
    return ServerState::Get().LogPumpActive();
}

size_t LogRingCount()
{
    std::lock_guard<std::mutex> lk(RingMtx());
    return Ring().size();
}

json::JsonValue LogDumpJson(size_t lastN)
{
    json::JsonValue rows = json::JsonValue::Array();
    std::lock_guard<std::mutex> lk(RingMtx());
    size_t begin = lastN >= Ring().size() ? 0 : Ring().size() - lastN;
    for (size_t i = begin; i < Ring().size(); ++i)
        rows.pushBack(EntryJson(Ring()[i]));
    return rows;
}

void AppendSyntheticLog(const std::wstring& text)
{
    LogEntry e;
    e.msgNum = 0;   // 非驱动游标来源
    e.msgId = 0;    // 0 = 合成条目（guardian 等 server 内部行为）
    e.pid = 0;
    e.time = LocalTimeText();
    e.text = text;
    {
        std::lock_guard<std::mutex> lk(RingMtx());
        Ring().push_back(e);
        while (Ring().size() > kRingMax)
            Ring().pop_front();
    }
    PushToSubscribers(e);
}

} // namespace sbie::server
