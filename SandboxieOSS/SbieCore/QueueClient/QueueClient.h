// Sandboxie-OSS — SbieCore/QueueClient/QueueClient.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// MSGID_QUEUE_* 封装 + interactive queue（契约：04-modules.md §2.3；
// 协议规格：03-svc-protocol.md §5-§6）。
//
// ── 契约（冻结）───────────────────────────────────────────────────────────
//   class QueueClient { Create / GetReq / PutRpl } 与
//   InteractiveQueueName() 的签名语义按 04 §2.3，不得变更。
// ── M2 对契约的补充（additive）────────────────────────────────────────────
//   * InteractiveSession/InteractiveRequest/InteractiveReply/Reconnect：
//     03 §6 的 GETREQ 循环 + 按 data[0..3] 分发 + PUTRPL 回复 + 断线重建，
//     供 log watch --interactive 与（后续波次）server 队列泵共用。
//   * 全部 LPC 请求经 SvcClient::Call（同一端口；03 §1 线程亲和由
//     SvcClient 内部 mutex 串行保证）。

#pragma once

#include "../Util/Status.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sbie::queue {

class QueueClient {           // "*MANPROXY_%08X"（会话 id 十六进制大写，03 §6）
public:
    SbieStatus Create(const std::wstring& name, HANDLE* outEvent);
    SbieStatus GetReq(const std::wstring& name, ULONG* clientPid, ULONG* reqId,
                      std::vector<uint8_t>* data);
    SbieStatus PutRpl(const std::wstring& name, ULONG reqId, const void* data, ULONG len);
};

// 会话 interactive 队列名（含前导 '*'；03 §6）
std::wstring InteractiveQueueName();

// ---------------------------------------------------------------------------
// interactive queue 泵（03 §6；additive）
// ---------------------------------------------------------------------------

// GETREQ 取到的一条请求（按 data 前 4 字节 msgid 分派解析）
struct InteractiveRequest {
    ULONG reqId = 0;
    ULONG clientPid = 0;
    int kind = 0;                      // 1=MAN_FILE_MIGRATION 2=MAN_INET_BLOCKADE 0=未知
    unsigned long long fileSize = 0;   // FILE_MIGRATION：待决策文件大小（字节）
    std::wstring filePath;             // FILE_MIGRATION：真实路径（原样转送）
};

// PUTRPL 载荷 = 裸 {ULONG status; ULONG retval;}（InteractiveWire.h，不带 MSG_HEADER）。
// retval 语义（file_copy.c:348-356 实读核实）：!=0 允许继续把文件拷进沙箱，
// ==0 拒绝拷贝（文档初稿 TODO-VERIFY 已解除，见 docs/03 §6）。
struct InteractiveReply {
    ULONG status = 0;   // NTSTATUS，通常 STATUS_SUCCESS(0)
    ULONG retval = 0;   // 默认自动策略 = 0（拒绝）
};

// 决策回调：返回 true = 已填 *rpl 并回送；false = 走默认自动应答（retval=0）
using InteractiveSink = std::function<bool(const InteractiveRequest&, InteractiveReply*)>;

// 一轮"创建-泵"会话；断线（ERR_SVC_TRANSPORT）后 Reconnect 整体重建
class InteractiveSession {
public:
    InteractiveSession() = default;
    ~InteractiveSession();

    // QueueClient::Create(InteractiveQueueName())；成功后 event() 可等待
    SbieStatus Start();
    // 事件置位后调用：GETREQ 循环到空（STATUS_END_OF_FILE），逐条
    // sink→PutRpl；未知类型回复 {STATUS_NOT_SUPPORTED, 0}
    SbieStatus Drain(const InteractiveSink& sink);
    // 丢弃本会话（关闭事件句柄；SbieSvc 侧队列随请求超时/断连回收）
    void Reset();

    HANDLE event() const { return event_; }
    bool active() const { return event_ != nullptr; }
    const std::wstring& name() const { return name_; }

private:
    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    std::wstring name_ = InteractiveQueueName();
    HANDLE event_ = nullptr;
};

// 断线重连语义：Reset → Start；Start 传输失败时 Sleep(backoffMs) 后再试一次。
// 返回最终 Start 状态（调用方决定继续还是放弃该源）。
SbieStatus Reconnect(InteractiveSession& s, unsigned backoffMs);

} // namespace sbie::queue
