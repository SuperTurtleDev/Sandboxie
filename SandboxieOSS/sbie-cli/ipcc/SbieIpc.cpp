// Sandboxie-OSS — sbie-cli/ipcc/SbieIpc.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 会话级命名工具 + 帧编解码 + client 连接（server 波次实现）。

#include "SbieIpc.h"

#include <cstdio>
#include <cstring>

namespace sbie::ipc {

uint32_t CurrentSessionId()
{
    ULONG sid = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid))
        sid = 0; // 失败回退 0（00 §2）
    return sid;
}

std::wstring PipeName()
{
    wchar_t buf[80];
    swprintf_s(buf, L"\\\\.\\pipe\\SbieOSS_Cli_S%u", CurrentSessionId());
    return buf;
}

std::wstring MutexName()
{
    wchar_t buf[80];
    swprintf_s(buf, L"Local\\SbieOSS_Server_S%u", CurrentSessionId());
    return buf;
}

// ---------------------------------------------------------------------------
// 帧编解码
// ---------------------------------------------------------------------------

namespace {

// 读满 n 字节（消息模式管道：消息长于请求缓冲时 ReadFile 返回 TRUE +
// ERROR_MORE_DATA，属正常分段；短读 = 连接结束）
bool ReadAll(HANDLE h, void* buf, DWORD n)
{
    UCHAR* p = (UCHAR*)buf;
    DWORD left = n;
    while (left) {
        DWORD got = 0;
        if (!ReadFile(h, p, left, &got, nullptr))
            return false;
        if (got == 0)
            return false; // EOF / broken pipe
        p += got;
        left -= got;
    }
    return true;
}

} // namespace

bool WriteFrame(HANDLE h, uint32_t msgid, const void* payload, uint32_t payloadLen)
{
    if (payloadLen > kMaxPayloadLen)
        return false;
    FrameHeader hdr;
    hdr.magic = kIpcMagic;
    hdr.version = kIpcVersion;
    hdr.msgid = msgid;
    hdr.payloadLen = payloadLen;

    // 单次 WriteFile：消息模式管道下一条 WriteFile = 一条完整消息
    UCHAR buf[kFrameHeaderLen];
    memcpy(buf, &hdr, sizeof(hdr));
    DWORD done = 0;
    if (!WriteFile(h, buf, sizeof(buf), &done, nullptr) || done != sizeof(buf))
        return false;
    if (payloadLen) {
        done = 0;
        if (!WriteFile(h, payload, payloadLen, &done, nullptr)
            || done != payloadLen)
            return false;
    }
    return true;
}

bool ReadFrame(HANDLE h, FrameHeader* outHeader, std::vector<uint8_t>* outPayload)
{
    FrameHeader hdr;
    if (!ReadAll(h, &hdr, sizeof(hdr)))
        return false;
    if (hdr.magic != kIpcMagic || hdr.version != kIpcVersion)
        return false;
    if (hdr.payloadLen > kMaxPayloadLen)
        return false;
    outPayload->resize(hdr.payloadLen);
    if (hdr.payloadLen
        && !ReadAll(h, outPayload->data(), hdr.payloadLen))
        return false;
    *outHeader = hdr;
    return true;
}

// ---------------------------------------------------------------------------
// PipeClient
// ---------------------------------------------------------------------------

PipeClient::~PipeClient()
{
    Close();
}

bool PipeClient::Open(DWORD waitBusyMs)
{
    Close();
    const std::wstring name = PipeName();
    for (;;) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // 管道为消息类型；client 侧默认字节读模式，切回 MESSAGE
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr); // 失败容忍
            pipe_ = h;
            lastErr_ = 0;
            return true;
        }
        lastErr_ = GetLastError();
        if (lastErr_ == ERROR_PIPE_BUSY && waitBusyMs > 0) {
            DWORD slice = waitBusyMs > 1000 ? 1000 : waitBusyMs;
            WaitNamedPipeW(name.c_str(), slice); // 00 §5 (D)
            waitBusyMs = waitBusyMs > slice ? waitBusyMs - slice : 0;
            continue;
        }
        return false;
    }
}

void PipeClient::Close()
{
    if (pipe_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
}

bool PipeClient::RoundTrip(const char* op, const std::string& paramsJson,
                           std::string* replyJson)
{
    if (!pipe_) {
        lastErr_ = ERROR_NO_DATA;
        return false;
    }
    // 请求 payload：{"op":"<op>","params":<params>}。op 为 [A-Za-z.] 标识符，
    // 无需转义；params 由调用方保证为合法 JSON 对象文本（缺省 "{}"）。
    std::string payload = "{\"op\":\"";
    payload += op;
    payload += "\",\"params\":";
    payload += paramsJson.empty() ? std::string("{}") : paramsJson;
    payload += "}";

    const uint32_t id = nextMsgId_++;
    if (!WriteFrame(pipe_, id, payload.data(), (uint32_t)payload.size())) {
        lastErr_ = GetLastError();
        Close();
        return false;
    }
    FrameHeader hdr;
    std::vector<uint8_t> buf;
    if (!ReadFrame(pipe_, &hdr, &buf)) {
        lastErr_ = GetLastError();
        Close();
        return false;
    }
    if ((hdr.msgid & ~kMsgIdErrorFlag) != id) {
        lastErr_ = ERROR_INVALID_FUNCTION; // 帧错位：协议失步，废弃连接
        Close();
        return false;
    }
    replyJson->assign((const char*)buf.data(), buf.size());
    return true;
}

} // namespace sbie::ipc
