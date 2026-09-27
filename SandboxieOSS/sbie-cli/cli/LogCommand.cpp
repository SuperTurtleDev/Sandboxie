// Sandboxie-OSS — sbie-cli/cli/LogCommand.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// `sbie-cli log` — dmesg 风格驱动日志读取（docs/10 §9）。
//
//   sbie-cli log                        # dump 环形缓冲（dmesg）
//   sbie-cli log -w                     # 跟踪新消息（dmesg -w；Ctrl+C 退出）
//   sbie-cli log --last 50              # 尾部 N 条
//   sbie-cli log --type 13              # 消息号段过滤（13xx 进程/22xx 配置…）
//   sbie-cli log --box MC --pid 1234    # 文本/PID 过滤
//   sbie-cli log --json                 # NDJSON 流
//
// 会话领导权（唯一性约束，docs/00 §7）：SbieApi_GetMessage 仅会话 leader
// 可读（api.c:728-732），且 leader 无反注册 API、不可抢占。本命令在无人
// 持有时自任 leader；他人持有（SandMan 或公共 monitor）时报错并指明占用者
// pid。文档化约束：-w 跟踪期间不可同时跑 SandMan/monitor；盒运行期间读
// 日志先停 monitor（无任务时其自行退出）。
//
// 文案：msgid → SBIE 文本经 SbieDll_FormatMessage0（SbieMsg.dll 资源），
// 每个 msgid 的格式串仅取一次并缓存（该导出无配对 free，防 -w 长跑泄漏）；
// %N 以消息携带字符串替换（dll support.c SbieDll_FormatMessage 数组变体
// 同约定：ins[N-1]）。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace sbie::cli {

namespace {

// 大小写不敏感子串（日志文本短，朴素扫描够用；避免引 shlwapi）
const wchar_t* WcsIStr(const wchar_t* h, const wchar_t* n)
{
    if (!h || !n || !*n)
        return nullptr;
    for (const wchar_t* p = h; *p; ++p) {
        const wchar_t* a = p;
        const wchar_t* b = n;
        while (*a && *b && towlower(*a) == towlower(*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return p;
    }
    return nullptr;
}

struct LogFilter {
    bool follow = false;
    int typeBand = -1;              // --type 13 → 仅 1300-1399 号段
    bool hasBox = false;
    std::wstring box;
    ULONG pid = 0;                  // 0 = 不过滤
    size_t lastN = 0;               // 0 = 全量
};

struct LogEntry {
    ULONG seq = 0, msgid = 0, rawId = 0, pid = 0;
    std::vector<std::wstring> strings;
};

// N2（docs/11 复测 major）：条目超缓冲（长 cmdline）时驱动返回
// STATUS_BUFFER_TOO_SMALL 且不推进 *msg_num——条目本身已被环迭代越过，
// 同 seq 重试将永远命中同一条 => follower 投递死点。对策：
//   1) 缓冲扩至 64KB（堆，覆盖全部现实条目）；
//   2) 连续 3 次 TOO_SMALL 仍不前进时，seq+1 强制解卡（丢弃该条，计数
//      呈现在输出诊断）。
// N2 真身（终段）：SbieApi_GetMessage 的 DLL 包装把 Length 截断为
// USHORT（sbieapi.c:312 msgtext.MaximumLength = (USHORT)Length）——
// 65536 字节截断为 0 ⇒ 一切条目皆 BUFFER_TOO_SMALL ⇒ 永无投递。
// 缓冲字节数必须 < 65536：32760 WCHAR = 65520 B（保留对齐余量）。
constexpr ULONG kLogBufWChars = 32760;
constexpr ULONG kStatusBufferTooSmall = 0xC0000023;
ULONG g_lastFetchRc = 0;   // forensic（V2LOGDBG）

// 取一条的三态结果：N2 实测定位——环内存在超缓冲巨型条目（>32KB 单串），
// 驱动对其返回 BUFFER_TOO_SMALL 但仍推进 *msg_num（api.c:807 无条件写）：
// 巨型条会被"每次一跳 150ms"地爬行吞掉时间且不产出任何可见行（follower
// 投递死点的真身）。TooSmall 必须被上层立即重试（不睡眠）以快速跳过。
enum class FetchResult { Delivered, Empty, TooSmall };

FetchResult FetchOne(ULONG* seq, LogEntry* e)
{
    static std::vector<WCHAR> buf(kLogBufWChars);
    ULONG msgid = 0, pid = 0;
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    ULONG rc = drv::ApiP()->SbieApi_GetMessage(seq, sid, &msgid, &pid,
                                               buf.data(),
                                               kLogBufWChars * sizeof(WCHAR));
    g_lastFetchRc = rc;
    if (rc == kStatusBufferTooSmall)
        return FetchResult::TooSmall;   // 游标已被驱动推进（api.c:807）
    if (rc != 0)
        return FetchResult::Empty;
    e->seq = *seq;
    e->rawId = msgid;
    // B4：内核 Log_Msg_* 打包 msgid（实测 0x41020577 → 真值 1399）：
    // 真实 SBIE 消息号 = 低 16 位；高 16 位是打包位（未剥除时
    // SbieDll_FormatMessage0 恒走 err=… 兜底串，docs/11 blocker-4）。
    // 预格式化消息（SbieApi_Log 已合成文本者）id 无高位打包——统一剥除
    // 不影响其 <0x10000 的真值。
    e->msgid = msgid & 0xFFFF;
    e->pid = pid;
    const WCHAR* p = buf.data();
    const WCHAR* end = buf.data() + kLogBufWChars;
    while (p < end && *p) {
        const WCHAR* q = p;
        while (q < end && *q)
            ++q;
        e->strings.emplace_back(p, q - p);
        p = q + 1;
    }
    return FetchResult::Delivered;
}

// 文案渲染（B4 修复）：用 SbieDll_FormatMessage 数组变体（core\dll// support.c:882-906 —— FormatMessage(…, SbieMsgDll, code, lang, &out, 4,
// (va_list*)ins)），ins[1..5] = 条目字符串（导出约定 %N ↔ ins[N]，ins[0]
// 不用）。FormatMessage0（无 ins）恒落 "err=%08X..." 兜底（docs/11 B4 实测）
// ——据此检测兜底签名并回退自渲染。注意：该导出无配对 free，逐行调用有
// 每行一次的小额分配（~0.5KB）不回收；dump 量级可忽略，-w 长跑记录在案。
std::wstring RenderText(const LogEntry& e)
{
    if (drv::ApiP()->SbieDll_FormatMessage) {
        const wchar_t* ins[6] = {nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr};
        for (size_t i = 0; i < e.strings.size() && i < 5; ++i)
            ins[i + 1] = e.strings[i].c_str();
        if (WCHAR* t = drv::ApiP()->SbieDll_FormatMessage(e.msgid, ins)) {
            std::wstring out = t;
            if (out.rfind(L"err=", 0) != 0)   // 非兜底签名 = 真文案
                return out;
            if (e.strings.empty())
                return out;   // 兜底 + 无字符串：保留诊断形态
        }
    }
    // 回退：自拼接（msgid + 字符串）
    std::wstring r = L"SBIE" + std::to_wstring(e.msgid);
    for (const auto& x : e.strings)
        r += L" | " + x;
    return r;
}


bool Matches(const LogFilter& f, const LogEntry& e, const std::wstring& text)
{
    if (f.typeBand >= 0 && (int)(e.msgid / 100) != f.typeBand)
        return false;
    if (f.pid && e.pid != f.pid)
        return false;
    if (f.hasBox) {
        bool hit = false;
        for (const auto& s : e.strings)
            if (_wcsicmp(s.c_str(), f.box.c_str()) == 0) {
                hit = true;
                break;
            }
        if (!hit && WcsIStr(text.c_str(), f.box.c_str()) == nullptr)
            return false;
    }
    return true;
}

void EmitEntry(const GlobalOptions& opts, const LogEntry& e, const std::wstring& text)
{
    if (opts.json) {
        json::JsonValue o = json::JsonValue::Object();
        o.set(L"seq", json::JsonValue((long long)e.seq));
        o.set(L"msgid", json::JsonValue((long long)e.msgid));
        o.set(L"raw_id", json::JsonValue((long long)e.rawId));
        o.set(L"pid", json::JsonValue((long long)e.pid));
        o.set(L"text", json::JsonValue(text));
        util::PrintLineUtf8(json::SerializeUtf8(o));
    } else {
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + std::to_wstring(e.seq) + L"] SBIE" + std::to_wstring(e.msgid)
            + L" pid=" + std::to_wstring(e.pid) + L" " + text));
    }
}

} // namespace

int CmdLog(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    LogFilter f;
    for (size_t i = 1; i < ctx.args.size(); ++i) {   // log 无 target 位：选项即首参
        const std::wstring& a = ctx.args[i];
        if (a == L"-w" || a == L"--follow")
            f.follow = true;
        else if (a == L"--last" && i + 1 < ctx.args.size())
            f.lastN = (size_t)wcstoul(ctx.args[++i].c_str(), nullptr, 10);
        else if ((a == L"-n" || a == L"--level" || a == L"--type")
                 && i + 1 < ctx.args.size())
            f.typeBand = (int)wcstol(ctx.args[++i].c_str(), nullptr, 10);
        else if (a == L"--box" && i + 1 < ctx.args.size()) {
            f.hasBox = true;
            f.box = ctx.args[++i];
        } else if (a == L"--pid" && i + 1 < ctx.args.size())
            f.pid = (ULONG)wcstoul(ctx.args[++i].c_str(), nullptr, 10);
        else
            return EmitError(opts, SbieStatus::USAGE, L"unknown option: " + a);
    }

    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    // 领导权：先查占用者（get：实 session id + 出参；-1 走 token 通道不适用
    // 我们）。无人持有则 set 自任（set 要求非沙箱调用者，session.c:349）。
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    HANDLE holder = nullptr;
    LONG grc = drv::ApiP()->SbieApi_SessionLeader(sid, &holder);
    if (grc == 0 && holder) {
        return EmitError(opts, SbieStatus::ACCESS_DENIED,
                         L"session leader already held by pid "
                             + std::to_wstring((ULONG)(ULONG_PTR)holder)
                             + L" (SandMan or the v2 monitor; stop it to read logs)");
    }
    LONG src = drv::ApiP()->SbieApi_SessionLeader(0, nullptr);
    if (src != 0)
        return EmitError(opts, SbieStatus::ACCESS_DENIED,
                         std::wstring(L"cannot acquire session leadership: ")
                             + NtStatusText(src));

    // dump（--last 用滚动窗裁尾）。-w 不回放历史（dmesg -w 的 tail 语义：
    // 整环回放在环内存在巨型条/持续背景流量时耗时不可控，且历史行会污染
    // follower 的行计数——N2 复测定位）。
    ULONG seq = 0;
    std::vector<LogEntry> live;
    size_t skippedOversize = 0;
    if (!f.follow) {
    for (;;) {
        LogEntry e;
        FetchResult fr = FetchOne(&seq, &e);
        if (fr == FetchResult::Empty)
            break;
        if (fr == FetchResult::TooSmall) {
            ++skippedOversize;   // 巨型条目：游标已推进，跳过继续
            continue;
        }
        std::wstring text = RenderText(e);
        if (!Matches(f, e, text))
            continue;
        if (f.lastN) {
            live.push_back(std::move(e));
            if (live.size() > f.lastN)
                live.erase(live.begin());
        } else {
            EmitEntry(opts, e, text);
        }
    }
    if (skippedOversize && !opts.quiet && !opts.json)
        Diag(L"skipped " + std::to_wstring(skippedOversize)
             + L" oversized ring entries (>64KB strings)");
    for (auto& e : live) {
        std::wstring text = RenderText(e);
        EmitEntry(opts, e, text);
    }
    }

    // -w 跟踪（Ctrl+C = 默认控制台处理，进程直接终止）。
    // N2 真身（二段）：-w 先前也执行完整 dump（整环渲染，环内含大量巨型
    // 条与持续背景流量时耗时以分钟计），follow 循环迟迟不启动 = "dump 段
    // 后 0 投递"。修正：-w 不回放历史——静默快进到环头（不渲染、巨型条
    // 即跳），再进入跟随循环（dmesg -w 的 tail 语义）。
    if (f.follow) {
        {
            // 有界快进（≤750ms）：背景流量持续（实测 ~8 条/s）时无 Empty
            // 可等，无限快进 = 永不进入跟随循环（N2 第三段真身）
            ULONGLONG ffUntil = GetTickCount64() + 5000;
            LogEntry e;
            while (GetTickCount64() < ffUntil) {
                FetchResult fr = FetchOne(&seq, &e);
                if (fr == FetchResult::Empty)
                    break;
                // TooSmall / Delivered：游标已推进，继续
            }
        }
        if (!opts.json && !opts.quiet)
            Diag(L"following (Ctrl+C to stop)");
        for (;;) {
            LogEntry e;
            FetchResult fr = FetchOne(&seq, &e);
            if (fr == FetchResult::TooSmall)
                continue;   // 立即重试：快速跳过巨型条（不睡眠）
            if (fr == FetchResult::Empty) {
                static bool dbg = GetEnvironmentVariableW(L"V2LOGDBG", nullptr, 0) != 0;
                if (dbg) {
                    static ULONG last = 1;
                    ULONG cur = g_lastFetchRc;
                    if (cur != last) {
                        wchar_t b[16];
                        swprintf_s(b, L"%08X", (unsigned)cur);
                        Diag(std::wstring(L"follow rc=0x") + b
                             + L" seq=" + std::to_wstring(seq));
                        last = cur;
                    }
                }
                Sleep(150);
                continue;
            }
            std::wstring text = RenderText(e);
            if (Matches(f, e, text))
                EmitEntry(opts, e, text);
        }
    }
    return 0;
}

} // namespace sbie::cli
