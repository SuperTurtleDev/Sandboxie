// Sandboxie-OSS — SbieCore/QueueClient/QueueClient.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 协议规格：03-svc-protocol.md §5-§6（vendored queuewire.h/InteractiveWire.h）。
// 行为参考（按 01-license-map §2 允许范围）：QSbieAPI SbieAPI.cpp 的
// CSbieAPI__QueueCreate / GetQueueReq / SendQueueRpl（LGPL-2.1，仅协议参考）。

#include "QueueClient.h"
#include "../SvcClient/SvcClient.h"

// vendor 协议头（msgids.h：MSGID_QUEUE_*；queuewire.h：QUEUE_* 结构；
// InteractiveWire.h：MAN_FILE_MIGRATION / MAN_INET_BLOCKADE）
#include "msgids.h"
#include "queuewire.h"
#include "InteractiveWire.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace sbie::queue {

namespace {

// queueserver.cpp:298 / :409：队列当前为空（GETREQ/GETRPL 的"无更多"）
constexpr LONG kStatusEndOfFile = 0xC0000011L;
// session leader 之外调用、未知 interactive 类型回复等
constexpr LONG kStatusNotSupported = 0xC00000BBL;

// 队列名拷入定长 WCHAR[QUEUE_NAME_MAXLEN]（防御截断；调用方已限长）
void CopyQueueName(WCHAR (&dst)[QUEUE_NAME_MAXLEN], const std::wstring& name)
{
    wcsncpy_s(dst, name.c_str(), _TRUNCATE);
}

} // namespace

std::wstring InteractiveQueueName()
{
    ULONG sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    wchar_t buf[64];
    swprintf_s(buf, L"*%s_%08X", INTERACTIVE_QUEUE_NAME, sid);
    return buf;
}

// ---------------------------------------------------------------------------
// QueueClient（04 §2.3 契约；03 §5）
// ---------------------------------------------------------------------------

SbieStatus QueueClient::Create(const std::wstring& name, HANDLE* outEvent)
{
    if (outEvent)
        *outEvent = nullptr;
    if (name.empty() || name.size() >= QUEUE_NAME_MAXLEN)
        return SbieStatus::INVALID;

    // 03 §5：调用方先 CreateEvent（auto-reset），SbieSvc 有请求时 SetEvent
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev)
        return SbieStatus::GENERIC;

    QUEUE_CREATE_REQ req{};
    req.h.length = sizeof(req);
    req.h.msgid = MSGID_QUEUE_CREATE;
    CopyQueueName(req.queue_name, name);
    req.event_handle = (ULONG64)(ULONG_PTR)ev;

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = svc::SvcClient::Instance().Call(&req, sizeof(req), &rpl, &rplLen);
    if (Ok(st)) {
        QUEUE_CREATE_RPL* r = (QUEUE_CREATE_RPL*)rpl;
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
    }
    if (!Ok(st)) {
        CloseHandle(ev);
        return st;
    }
    if (outEvent)
        *outEvent = ev;
    else
        CloseHandle(ev);   // 调用方不要事件（诊断用 Create）也不泄漏
    return SbieStatus::OK;
}

SbieStatus QueueClient::GetReq(const std::wstring& name, ULONG* clientPid,
                               ULONG* reqId, std::vector<uint8_t>* data)
{
    if (clientPid)
        *clientPid = 0;
    if (reqId)
        *reqId = 0;
    if (data)
        data->clear();
    if (name.empty() || name.size() >= QUEUE_NAME_MAXLEN)
        return SbieStatus::INVALID;

    QUEUE_GETREQ_REQ req{};
    req.h.length = sizeof(req);
    req.h.msgid = MSGID_QUEUE_GETREQ;
    CopyQueueName(req.queue_name, name);

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = svc::SvcClient::Instance().Call(&req, sizeof(req), &rpl, &rplLen);
    if (!Ok(st))
        return st;
    QUEUE_GETREQ_RPL* r = (QUEUE_GETREQ_RPL*)rpl;
    LONG nt = (LONG)r->h.status;
    if (nt == kStatusEndOfFile) {           // 队列空：泵循环的正常出口
        free(rpl);
        return SbieStatus::NOT_FOUND;
    }
    if (nt < 0) {
        free(rpl);
        return FromNtStatus(nt);
    }

    if (clientPid)
        *clientPid = r->client_pid;
    if (reqId)
        *reqId = r->req_id;
    if (data) {
        // data_len 与实际回复长度双向校验，拒绝越界载荷（04 §7.3 同款防御姿态）
        const size_t dataOff = offsetof(QUEUE_GETREQ_RPL, data);
        if (r->data_len != 0
            && (rplLen < dataOff || r->data_len > rplLen - dataOff)) {
            free(rpl);
            return SbieStatus::GENERIC;
        }
        data->assign(r->data, r->data + r->data_len);
    }
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus QueueClient::PutRpl(const std::wstring& name, ULONG reqId,
                               const void* data, ULONG len)
{
    if (name.empty() || name.size() >= QUEUE_NAME_MAXLEN)
        return SbieStatus::INVALID;

    // 变长请求：定长头 + data 载荷（offsetof 避开 UCHAR data[1] 的尾随语义）
    const size_t dataOff = offsetof(QUEUE_PUTRPL_REQ, data);
    const size_t total = dataOff + len;
    QUEUE_PUTRPL_REQ* req = (QUEUE_PUTRPL_REQ*)calloc(1, total);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.length = (ULONG)total;
    req->h.msgid = MSGID_QUEUE_PUTRPL;
    CopyQueueName(req->queue_name, name);
    req->req_id = reqId;
    req->data_len = len;
    if (len)
        memcpy(req->data, data, len);

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = svc::SvcClient::Instance().Call(req, total, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    QUEUE_PUTRPL_RPL* r = (QUEUE_PUTRPL_RPL*)rpl;
    st = FromNtStatus((LONG)r->h.status);
    free(rpl);
    return st;
}

// ---------------------------------------------------------------------------
// interactive queue 泵（03 §6；additive）
// ---------------------------------------------------------------------------

InteractiveSession::~InteractiveSession()
{
    Reset();
}

SbieStatus InteractiveSession::Start()
{
    if (event_)
        return SbieStatus::OK;
    QueueClient qc;
    HANDLE ev = nullptr;
    SbieStatus st = qc.Create(name_, &ev);
    if (!Ok(st))
        return st;
    event_ = ev;
    return SbieStatus::OK;
}

void InteractiveSession::Reset()
{
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

SbieStatus InteractiveSession::Drain(const InteractiveSink& sink)
{
    if (!event_)
        return SbieStatus::ERR_SVC_TRANSPORT;   // 未启动：视为传输层缺位

    QueueClient qc;
    for (;;) {
        ULONG pid = 0, reqId = 0;
        std::vector<uint8_t> blob;
        SbieStatus st = qc.GetReq(name_, &pid, &reqId, &blob);
        if (st == SbieStatus::NOT_FOUND)
            break;                              // 队列已抽干
        if (!Ok(st))
            return st;                          // 传输错：上抛（调用方 Reconnect）

        // 03 §6：data[0..3]（ULONG）分派请求类型
        InteractiveRequest ir;
        ir.reqId = reqId;
        ir.clientPid = pid;
        if (blob.size() >= sizeof(ULONG)) {
            ULONG kind = 0;
            memcpy(&kind, blob.data(), sizeof(kind));
            ir.kind = (int)kind;
            if (kind == MAN_FILE_MIGRATION) {
                // MAN_FILE_MIGRATION_REQ {ULONG msgid; ULONGLONG file_size;
                //                        WCHAR file_path[256];}（尾部按长度防御截断）
                if (blob.size() >= 16)
                    memcpy(&ir.fileSize, blob.data() + 8, sizeof(ULONGLONG));
                const size_t kPathOff = 16;
                if (blob.size() > kPathOff) {
                    const size_t maxChars =
                        (blob.size() - kPathOff) / sizeof(WCHAR) < 256
                            ? (blob.size() - kPathOff) / sizeof(WCHAR) : 256;
                    const WCHAR* p = (const WCHAR*)(blob.data() + kPathOff);
                    size_t n = 0;
                    while (n + 1 < maxChars && p[n])
                        ++n;
                    ir.filePath.assign(p, n);
                }
            }
            // MAN_INET_BLOCKADE_REQ 仅 msgid；未知类型保持 kind 原值，由回执告知
        }

        InteractiveReply rpl;
        bool answered = false;
        if (sink)
            answered = sink(ir, &rpl);
        if (!answered) {
            // 04 §3（03 §6）默认无人值守策略：FILE_MIGRATION 拒绝（retval=0）、
            // INET_BLOCKADE 记日志（由 sink 侧打印）后 retval=0
            rpl.status = 0;
            rpl.retval = 0;
        }
        if (ir.kind != MAN_FILE_MIGRATION && ir.kind != MAN_INET_BLOCKADE)
            rpl.status = (ULONG)kStatusNotSupported;

        st = qc.PutRpl(name_, reqId, &rpl, sizeof(rpl));
        if (!Ok(st))
            return st;
    }
    return SbieStatus::OK;
}

SbieStatus Reconnect(InteractiveSession& s, unsigned backoffMs)
{
    s.Reset();
    SbieStatus st = s.Start();
    if (!Ok(st) && st == SbieStatus::ERR_SVC_TRANSPORT && backoffMs) {
        Sleep(backoffMs);          // 退避一次（SbieSvc 5.50.5+ 支持重连，03 §1）
        st = s.Start();
    }
    return st;
}

} // namespace sbie::queue
