// Sandboxie-OSS — SbieCore/Model/Monitor.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 监控/trace 数据层（波 D1，docs/04 §20；07 深度差距 06-P2-1 收口）。
//
// 数据通路：API_MONITOR_GET2 批量拉取本会话监控环（DriverApi::MonitorGet2，
// 02 §3.6/§7 坑 1——SbieDll 无包装导出）→ 本模块解码记录体 → 条目文本。
// 记录体布局（drv session.c Session_MonitorPutEx :590-647 / Get2 :1176-1265）：
//   [LONGLONG 时间戳 8][ULONG type 4][ULONG pid 4][ULONG tid 4]
//   [若干 \0 结尾 WCHAR 串][可选 0xFFFF 栈标签区（MonitorStackTrace=y 时）]
// 串段语义（对齐 QSbieAPI CTraceEntry 的 LogData 切分）：
//   [0]=name（路径/对象名；message 型条目为空段）、[1]=message、[2]=subtype。
//
// 会话过滤：监控环按 Windows 会话隔离（写入侧 Session_Get(-1)=沙箱进程会话，
// 读取侧=调用方会话），同会话即全集——本模块不做二次过滤；box 过滤经
// pid→box 解析（MonitorPidResolver，SbieApi_QueryProcessEx2）。
//
// 类型缩写与 status 文本对齐 core 的 trace 语义（QSbieAPI SbieTrace.cpp
// GetTypeStr/GetStautsStr :127-205，行为参考）。

#pragma once

#include "../DriverApi/DriverApi.h"
#include "../Util/Status.h"

#include <cstddef>
#include <string>
#include <vector>

namespace sbie::model {

// 解码后的单条监控条目
struct MonitorEntry {
    ULONG type = 0;            // 原始 MONITOR_*（含 disposition/trace/user 位）
    ULONG pid = 0;
    ULONG tid = 0;
    ULONG64 timestamp = 0;     // 100ns / 1601 纪元（驱动 Util_GetTimestamp）
    std::vector<std::wstring> strings;  // NUL 分隔段（见文件头语义）
};

// MONITOR_TYPE_MASK 后的类型码 → 缩写（"File"/"Key"/"Pipe"/…；未知 → "Type%u"）
const wchar_t* MonitorTypeName(ULONG monitorType);

// disposition/trace 位 → 状态文本（"open"/"closed"/"open trace"/…；无位 → "-"）
std::wstring MonitorStatusText(ULONG type);

// 解码一条 GET2 记录体（entry 指向 size 字段之后的记录体；len = 记录体字节数）。
// 字段读取统一 memcpy（记录体在用户缓冲内仅保证 2 字节对齐）。false = 布局非法。
bool DecodeMonitorEntry(const unsigned char* entry, size_t len,
                        MonitorEntry* out);

// 遍历整块 GET2 缓冲（[ULONG size][记录体]…以 0 结尾）→ 条目数组。
// 返回解码成功的条数（损坏记录跳过并截断，防病态缓冲）。
size_t DecodeMonitorBuffer(const void* buffer, size_t bytes,
                           std::vector<MonitorEntry>* out);

// 条目 → 人类行文本（类型缩写[/.子类型][ (U)] 状态 pid box 值）。
// name=串[0]、message=串[1]（换行折叠为空格）、subtype=串[2]（并入类型）。
std::wstring FormatMonitorLine(const MonitorEntry& e, const std::wstring& box);

// 100ns/1601 时间戳 → 本地 "hh:mm:ss"（失败 "-"）
std::wstring MonitorTimeText(ULONG64 timestamp100ns);

// pid → box 解析（含负缓存：已退出/非沙箱 pid 记 "-"，避免逐条重复查询）。
// 单线程使用（泵线程/CLI 主线程各持一份）。
class MonitorPidResolver {
public:
    // box 未知时 *box 填 "-" 并返回 false
    bool Lookup(ULONG pid, std::wstring* box);

private:
    std::vector<std::pair<ULONG, std::wstring>> cache_;
};

// 直连自拉会话（client --no-server / 降级路径）：启停监控开关 + 拉尽。
// Start 记录开启前状态；Stop 仅在本会话自行开启时关回（他人——server/
// SandMan——开的监控不动）。
class MonitorDirectSession {
public:
    MonitorDirectSession();
    ~MonitorDirectSession();

    // 驱动在场 + （未启用时）MonitorControl set=1
    SbieStatus Start();
    // 拉尽监控环（含 STATUS_MORE_ENTRIES 续拉）。Ok = 正常（含零条）。
    SbieStatus Poll(std::vector<MonitorEntry>* out);
    // 幂等；由 Start 自行开启的监控在此关回
    void Stop();

    bool EnabledByUs() const { return enabledByUs_; }

private:
    bool started_ = false;
    bool enabledByUs_ = false;
    std::vector<unsigned char> buf_;   // 8 字节对齐拉取缓冲
};

} // namespace sbie::model
