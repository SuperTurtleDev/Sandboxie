// Sandboxie-OSS — SbieCore/Model/Monitor.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（按 01-license-map §2 允许范围，只记规格语义）：
//   - Sandboxie\core\drv\session.c（GPLv3）：Session_MonitorPutEx :590-647 的
//     记录体布局（[Time 8][Type 4][PID 4][TID 4][串…]）、Session_Api_MonitorGet2
//     :1176-1265 的用户缓冲布局（[ULONG size][记录体]…以 0 结尾）。
//   - SandboxiePlus\QSbieAPI\SbieAPI.cpp（LGPL-3.0，仅参考）：GetMonitor 的
//     批量缓冲遍历与串段切分（:3075-3121）。
//   - SandboxiePlus\QSbieAPI\SbieTrace.cpp（LGPL-3.0，仅参考）：类型缩写/
//     状态文本/串段语义（GetTypeStr :127-151、GetStautsStr :185-205、
//     CTraceEntry 构造 :58-110）。

#include "Monitor.h"

// vendor：MONITOR_* 常量（api_defs.h 内嵌 api_flags.h）
#include "api_defs.h"

#include <cstring>

namespace sbie::model {

namespace {

// GET2 拉取缓冲容量（QSbieAPI m_traceBufferLen 同值：256 * PAGE_SIZE）
constexpr ULONG kFetchBufferBytes = 256 * 4096;

// 记录体定长头：时间戳 8 + type 4 + pid 4 + tid 4
constexpr size_t kEntryHeaderBytes = 20;

// 串段终止标记（MonitorStackTrace=y 时其后为 [tag_id][tag_len][data] 栈标签）
constexpr WCHAR kStringsEndMarker = 0xFFFF;

// pid 负缓存上限（防长跑 watch 会话的病态增长；超限整体重置）
constexpr size_t kPidCacheMax = 4096;

} // namespace

// ---------------------------------------------------------------------------
// 类型/状态文本（对齐 core trace 语义）
// ---------------------------------------------------------------------------

const wchar_t* MonitorTypeName(ULONG monitorType)
{
    switch (monitorType & MONITOR_TYPE_MASK) {
    case MONITOR_APICALL:  return L"ApiCall";
    case MONITOR_SYSCALL:  return L"SysCall";
    case MONITOR_PIPE:     return L"Pipe";
    case MONITOR_IPC:      return L"Ipc";
    case MONITOR_RPC:      return L"Rpc";
    case MONITOR_WINCLASS: return L"WinClass";
    case MONITOR_DRIVE:    return L"Drive";
    case MONITOR_COMCLASS: return L"ComClass";
    case MONITOR_RTCLASS:  return L"RtClass";
    case MONITOR_IGNORE:   return L"Ignore";
    case MONITOR_IMAGE:    return L"Image";
    case MONITOR_FILE:     return L"File";
    case MONITOR_KEY:      return L"Key";
    case MONITOR_NETFW:    return L"Socket";
    case MONITOR_DNS:      return L"Dns";
    case MONITOR_SCM:      return L"SCM";
    case MONITOR_HOOK:     return L"Hook";
    case MONITOR_OTHER:    return L"Debug";
    default:               return L"Unknown";
    }
}

std::wstring MonitorStatusText(ULONG type)
{
    std::wstring s;
    const ULONG disp = type & MONITOR_DISPOSITION_MASK;
    if (disp == MONITOR_OPEN)
        s = L"open";
    else if (disp == MONITOR_DENY)
        s = L"closed";
    if (type & MONITOR_TRACE) {
        if (!s.empty())
            s += L' ';
        s += L"trace";
    }
    return s.empty() ? std::wstring(L"-") : s;
}

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------

bool DecodeMonitorEntry(const unsigned char* entry, size_t len,
                        MonitorEntry* out)
{
    if (!entry || !out || len < kEntryHeaderBytes)
        return false;
    // 统一 memcpy：记录体在用户缓冲内仅保证 2 字节对齐（记录 stride 可为
    // 4k+2 型偏移——驱动端逐字节搬运不强对齐）
    memcpy(&out->timestamp, entry, 8);
    memcpy(&out->type, entry + 8, 4);
    memcpy(&out->pid, entry + 12, 4);
    memcpy(&out->tid, entry + 16, 4);

    out->strings.clear();
    const WCHAR* s = reinterpret_cast<const WCHAR*>(entry + kEntryHeaderBytes);
    const size_t remWChars = (len - kEntryHeaderBytes) / 2;
    size_t i = 0;
    while (i < remWChars) {
        if (s[i] == kStringsEndMarker)
            break;   // 其后为栈标签区，本模块不解码
        size_t wlen = 0;
        while (i + wlen < remWChars && s[i + wlen])
            ++wlen;
        out->strings.emplace_back(s + i, wlen);
        i += wlen + 1;   // 跳过结尾 NUL
    }
    return true;
}

size_t DecodeMonitorBuffer(const void* buffer, size_t bytes,
                           std::vector<MonitorEntry>* out)
{
    if (!out)
        return 0;
    const size_t startCount = out->size();
    const unsigned char* p = static_cast<const unsigned char*>(buffer);
    const unsigned char* end = p + bytes;
    while (p + sizeof(ULONG) <= end) {
        ULONG size = 0;
        memcpy(&size, p, sizeof(size));
        if (size == 0)
            break;   // 驱动端恒置的终止记录
        p += sizeof(ULONG);
        if (size < kEntryHeaderBytes || p + size > end)
            break;   // 病态布局：截断丢弃余部（防御）
        MonitorEntry e;
        if (DecodeMonitorEntry(p, size, &e))
            out->push_back(std::move(e));
        p += size;
    }
    return out->size() - startCount;
}

// ---------------------------------------------------------------------------
// 行文本与时间
// ---------------------------------------------------------------------------

std::wstring MonitorTimeText(ULONG64 timestamp100ns)
{
    if (timestamp100ns == 0)
        return L"-";
    FILETIME utc{ (DWORD)(timestamp100ns & 0xFFFFFFFF),
                  (DWORD)(timestamp100ns >> 32) };
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local)
        || !FileTimeToSystemTime(&local, &st))
        return L"-";
    wchar_t buf[16];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring FormatMonitorLine(const MonitorEntry& e, const std::wstring& boxIn)
{
    std::wstring name, message, subtype;
    if (e.strings.size() > 0)
        name = e.strings[0];
    if (e.strings.size() > 1)
        message = e.strings[1];
    if (e.strings.size() > 2)
        subtype = e.strings[2];

    // SysCall：subtype = 系统调用名（CTraceEntry 并入 message 的同款语义）
    if ((e.type & MONITOR_TYPE_MASK) == MONITOR_SYSCALL && !subtype.empty()) {
        message += L", name=" + subtype;
        subtype.clear();
    }

    std::wstring type = MonitorTypeName(e.type);
    if (!subtype.empty())
        type += L"/" + subtype;
    if (e.type & MONITOR_USER)
        type += L" (U)";   // 用户态来源（SbieDll）

    // message 折叠换行（CTraceEntry 同款）
    std::wstring msg;
    msg.reserve(message.size());
    for (wchar_t c : message) {
        if (c == L'\r')
            continue;
        msg += (c == L'\n') ? L' ' : c;
    }

    std::wstring value = name;
    if (!msg.empty()) {
        if (!value.empty())
            value += L' ';
        value += msg;
    }
    if (value.empty())
        value = L"(empty)";

    const std::wstring& box = boxIn.empty() ? std::wstring(L"-") : boxIn;
    return type + L" " + MonitorStatusText(e.type) + L" "
         + std::to_wstring(e.pid) + L" " + box + L" " + value;
}

// ---------------------------------------------------------------------------
// pid → box 解析
// ---------------------------------------------------------------------------

bool MonitorPidResolver::Lookup(ULONG pid, std::wstring* box)
{
    for (const auto& kv : cache_) {
        if (kv.first == pid) {
            *box = kv.second;
            return kv.second != L"-";
        }
    }
    std::wstring res = L"-";
    drv::ProcQuery q;
    if (drv::QueryProcessById(pid, &q) == SbieStatus::OK && !q.box.empty())
        res = q.box;
    if (cache_.size() >= kPidCacheMax)
        cache_.clear();
    cache_.emplace_back(pid, res);
    *box = res;
    return res != L"-";
}

// ---------------------------------------------------------------------------
// 直连自拉会话
// ---------------------------------------------------------------------------

MonitorDirectSession::MonitorDirectSession()
{
    // vector 数据经 operator new 分配：x64 下按 16 字节对齐（满足 8 字节要求）
    buf_.resize(kFetchBufferBytes);
}

MonitorDirectSession::~MonitorDirectSession()
{
    Stop();
}

SbieStatus MonitorDirectSession::Start()
{
    if (started_)
        return SbieStatus::OK;
    if (!drv::Loaded() && !drv::LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (!drv::DriverAlive())
        return SbieStatus::DRIVER_UNAVAILABLE;

    ULONG old = 0;
    SbieStatus st = drv::MonitorControl(nullptr, &old);
    if (st != SbieStatus::OK)
        return st;
    enabledByUs_ = (old == 0);   // 他者（server/SandMan）已开的不动
    if (old == 0) {
        ULONG on = 1;
        st = drv::MonitorControl(&on, nullptr);
        if (st != SbieStatus::OK) {
            enabledByUs_ = false;
            return st;
        }
    }
    started_ = true;
    return SbieStatus::OK;
}

SbieStatus MonitorDirectSession::Poll(std::vector<MonitorEntry>* out)
{
    if (!started_)
        return SbieStatus::INVALID;
    out->clear();
    for (int guard = 0; guard < 64; ++guard) {   // 1MiB/轮 × 64 防病态积压
        ULONG len = kFetchBufferBytes;
        bool more = false;
        drv::MonitorFetch f = drv::MonitorGet2(buf_.data(), &len, &more);
        if (f == drv::MonitorFetch::Empty)
            return SbieStatus::OK;
        if (f == drv::MonitorFetch::Ok) {
            DecodeMonitorBuffer(buf_.data(), len, out);
            if (!more)
                return SbieStatus::OK;
            continue;   // STATUS_MORE_ENTRIES：积压未尽，立即续拉
        }
        if (f == drv::MonitorFetch::NotEnabled)
            return SbieStatus::GENERIC;   // 监控被并发关闭：交调用方重 Start
        return FromNtStatus(drv::LastNtStatus());
    }
    return SbieStatus::OK;
}

void MonitorDirectSession::Stop()
{
    if (!started_)
        return;
    started_ = false;
    if (enabledByUs_) {
        enabledByUs_ = false;
        ULONG off = 0;
        (void)drv::MonitorControl(&off, nullptr);   // 尽力而为
    }
}

} // namespace sbie::model
