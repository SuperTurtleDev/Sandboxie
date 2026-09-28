// Sandboxie-OSS — sbie-cli/monitor/MonitorMain.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// MonitorMain.h 实现。日志：monitor.log（追加式，>1MB 截断重开）。

#include "MonitorMain.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/V2/V2Cache.h"
#include "../../SbieCore/Model/V2/V2Common.h"
#include "../../SbieCore/Model/V2/V2Registry.h"
#include "../../SbieCore/Model/V2/V2Task.h"
#include "../../SbieCore/Model/V2/V2EncBox.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <string>

namespace sbie::monitor {

namespace {

// ---------------------------------------------------------------------------
// 诊断日志（追加；超 1MB 重开，防无限增长）
// ---------------------------------------------------------------------------

void MLog(const std::wstring& line)
{
    // 超 1MB 轮转：直接删除，随后的 OPEN_ALWAYS 重建空文件（逐行开关
    // 追加句柄，无持久句柄可言）
    const std::wstring path = model::v2::MonitorLogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)
        && ((((unsigned long long)fad.nFileSizeHigh << 32) + fad.nFileSizeLow)
            > (1ull << 20)))
        DeleteFileW(path.c_str());
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::wstring t = model::v2::NowIsoTimestamp() + L" [" + std::to_wstring(GetCurrentProcessId()) + L"] " + line + L"\n";
    std::string u8 = util::WideToUtf8(t);
    DWORD got = 0;
    WriteFile(h, u8.data(), (DWORD)u8.size(), &got, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// 任务状态
// ---------------------------------------------------------------------------

struct TaskState {
    model::v2::TaskEntry task;
    int zeroStreak = 0;
    bool seenProc = false;
    ULONGLONG bornTick = 0;
    ULONGLONG serviceOnlySinceTick = 0;   // B1：仅剩自举服务的起始 tick
};

// 心跳状态文件（B2）：monitor 每 tick 原子重写；exec 侧健康判定 =
// 互斥体存在 ∧ 心跳新鲜（mtime < staleMs）。修复"挂起 monitor 持互斥体
// 令 exec 误判健康不补拉 → 全会话注销停摆"（docs/11 blocker-2）。
std::wstring StatusFilePath()
{
    return model::v2::MonitorsDir() + L"\\monitor.status";
}

void WriteHeartbeat(ULONGLONG tick)
{
    std::wstring t = L"pid=" + std::to_wstring(GetCurrentProcessId()) + L"\n"
                     L"tick=" + std::to_wstring(tick) + L"\n"
                     L"time=" + model::v2::NowIsoTimestamp() + L"\n";
    model::v2::WriteTextFileAtomic(StatusFilePath(),
                                   util::WideToUtf8(t));
}

// teardown：删锁 → 删缓存 → reload → 等注册消失 → 删任务。
// 返回 false = 期间用户进程复起（竞态保险，取消 teardown 保留任务）。
bool TeardownBox(const model::v2::TaskEntry& t)
{
    // 删锁前重查用户进程数（B1 统一口径：排除自举服务——RpcSs 的 linger
    // 自杀是事件驱动，上游竞态下可永久滞留，不应阻断 teardown）
    model::v2::V2Err ce;
    size_t n = model::v2::BoxUserProcessCount(t.box, &ce);
    if (ce.Ok() && n > 0) {
        MLog(L"teardown aborted, box '" + t.box + L"' has user processes again");
        return false;
    }
    MLog(L"teardown box '" + t.box + L"' (alias="
         + (t.alias.empty() ? L"-" : t.alias) + L")");
    if (!model::v2::DeleteLock(t.boxPath).Ok())
        MLog(L"  warning: running.lock delete failed for " + t.boxPath);
    model::v2::V2Err e = model::v2::DeleteBoxCache(t.box);
    if (!e.Ok())
        MLog(L"  warning: cache delete failed: " + e.msg);
    e = model::v2::ReloadDriverConf();
    if (!e.Ok())
        MLog(L"  warning: reload failed: " + e.msg);
    else if (!model::v2::WaitForRegistration(t.box, false, 10000))
        MLog(L"  warning: registration still visible after 10s (cache file absent, "
             L"next reload will clear)");
    model::v2::DeleteTask(t.box);
    // 加密盒兜底 unmount（AutoUnmount=true 时 SbieSvc 通常已在盒终止通知
    // 卸载——此处幂等补一刀，防 AutoUnmount 链路缺席时卷滞留）
    if (t.encrypted && !t.regRoot.empty()) {
        model::v2::V2Err ue = model::v2::UnmountEncBox(t.regRoot);
        if (!ue.Ok())
            MLog(L"  warning: encbox unmount failed: " + ue.msg);
    }
    // （无 <box>.dead 墓碑：R1 起结算窗口由 exec 侧 spawn 后探活反应式自愈
    // 处理，墓碑无读取方）
    MLog(L"teardown done for '" + t.box + L"'");
    return true;
}

std::wstring SessionMutexName()
{
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return L"Local\\SbieOSS_Monitor_S" + std::to_wstring(sid);
}

// 就绪事件：monitor 完成领导权获取（或确认降级）后置位——exec 侧等待它，
// 消除"spawn 落在 leader 真空窗"的竞态（实测 rc 交替 7/4 的根因）。
std::wstring SessionReadyEventName()
{
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return L"Local\\SbieOSS_MonitorReady_S" + std::to_wstring(sid);
}

} // namespace

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

int RunMonitor(const MonitorOptions& opts)
{
    using namespace model::v2;

    if (!drv::LoadSbieDll() || !drv::DriverAlive()) {
        MLog(L"monitor: driver unavailable, exiting");
        return 0;
    }

    // 会话单例：CreateMutex(FALSE) + 短等待——WAIT_TIMEOUT = 活实例在持有 →
    // 静默退出 0；WAIT_ABANDONED = 旧持有者已亡 → 接管（崩溃自愈）。
    HANDLE mutex = CreateMutexW(nullptr, FALSE, SessionMutexName().c_str());
    if (!mutex) {
        MLog(L"monitor: cannot create session mutex");
        return 0;
    }
    DWORD w = WaitForSingleObject(mutex, 2000);
    if (w == WAIT_TIMEOUT || w == WAIT_FAILED) {
        MLog(L"monitor: another instance owns the session, exiting");
        CloseHandle(mutex);
        return 0;
    }
    if (w == WAIT_ABANDONED)
        MLog(L"monitor: adopted abandoned session mutex (previous instance died)");

    // 会话领导权（实测竞态修复，docs/10 §8.4）：盒内进程启动期（SbieDll init
    // → epmapper/actkernel 链）要求本会话存在活的 leader。OSS dist 不带
    // SandMan——公共 monitor 是天然持有者。已有人持有时（SandMan 在跑）
    // set 返回 STATUS_DEVICE_ALREADY_ATTACHED：降级为纯轮询（行为不变）。
    // 会话 leader 同时是驱动日志的读取资格，本实现仍不消费日志（拍板 D1）。
    {
        LONG rc = drv::ApiP()->SbieApi_SessionLeader(0, nullptr);
        if (rc == 0)
            MLog(L"monitor: acquired session leadership "
                 L"(no other leader; boxed process start requires a live one)");
        else
            MLog(L"monitor: session leader not acquired (rc=0x"
                 + [rc]() { wchar_t b[16]; swprintf_s(b, L"%08X", (unsigned)rc);
                            return std::wstring(b); }()
                 + L") - polling-only mode");
    }

    // 就绪信号（exec 侧 EnsureMonitorRunning 等待）+ 首跳心跳（B2：心跳
    // 尽早开始，互斥体获取后立刻可被健康判定看见）
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, SessionReadyEventName().c_str());
    if (ready)
        SetEvent(ready);
    WriteHeartbeat(0);

    MLog(L"monitor started (poll=" + std::to_wstring(opts.pollMs) + L"ms)");

    std::map<std::wstring, TaskState, std::less<>> tasks;
    ULONGLONG tick = 0;
    int emptySeen = 0;

    for (;;) {
        ++tick;
        Sleep(opts.pollMs);
        WriteHeartbeat(tick);   // B2：每 tick 心跳

        // 1) 目录对账：新任务入表；任务文件消失 = CLI 手动注销 → 出表
        auto current = EnumTasks();
        std::map<std::wstring, bool, std::less<>> seen;
        for (const auto& t : current)
            seen[t.box] = true;
        for (auto it = tasks.begin(); it != tasks.end();) {
            if (!seen.count(it->first)) {
                MLog(L"task file removed for '" + it->first + L"', dropping");
                it = tasks.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& t : current) {
            if (!tasks.count(t.box)) {
                TaskState st;
                st.task = t;
                st.bornTick = tick;
                tasks[t.box] = std::move(st);
                MLog(L"task added for '" + t.box + L"'");
            } else {
                tasks[t.box].task = t;   // 刷新（alias 等字段可能更新）
            }
        }

        // 2) 逐任务判定（B1 统一口径：归零判定用"用户进程"数——排除
        // RpcSs/DcomLaunch/BITS/WUAU/Crypto 自举服务镜像。上游 linger.c
        // 事件驱动自杀漏事件时服务滞留 >3min，原全量计数令 teardown 永不
        // 触发（docs/11 blocker-1）；同时保留兜底：仅剩服务且滞留超过
        // serviceGraceMs（默认 15s）→ KillAll 清场后正常 teardown。）
        for (auto it = tasks.begin(); it != tasks.end();) {
            TaskState& st = it->second;
            V2Err ce;
            size_t user = BoxUserProcessCount(st.task.box, &ce);
            if (!ce.Ok()) {
                // 驱动瞬态失败：不推进计数（保守）
                ++it;
                continue;
            }
            if (user > 0) {
                st.zeroStreak = 0;
                st.seenProc = true;
                st.serviceOnlySinceTick = 0;
                ++it;
                continue;
            }
            // user == 0
            V2Err te;
            size_t total = BoxProcessCount(st.task.box, &te);
            bool servicesOnly = te.Ok() && total > 0;
            if (servicesOnly) {
                if (st.serviceOnlySinceTick == 0)
                    st.serviceOnlySinceTick = tick;
                ULONGLONG lingerMs = (tick - st.serviceOnlySinceTick)
                                     * (ULONGLONG)opts.pollMs;
                if (lingerMs >= (ULONGLONG)opts.serviceGraceMs) {
                    MLog(L"lingering bootstrap services in '" + st.task.box
                         + L"' for " + std::to_wstring(lingerMs / 1000)
                         + L"s (upstream linger race); KillAll cleanup");
                    sbie::svc::SvcClient::Instance().KillAll(st.task.box,
                                                             (ULONG)-1);
                    st.serviceOnlySinceTick = tick;   // 防重复击杀
                }
                ++it;
                continue;   // 服务尚在：不计 zeroStreak，等 KillAll 生效
            }
            st.serviceOnlySinceTick = 0;
            if (st.seenProc) {
                ++st.zeroStreak;
            } else if ((tick - st.bornTick) * (ULONGLONG)opts.pollMs
                       >= (ULONGLONG)opts.graceMs) {
                st.zeroStreak = opts.zeroStreak;   // 孤儿任务宽限期过，判死
            }
            if (st.zeroStreak >= opts.zeroStreak) {
                if (TeardownBox(st.task))
                    it = tasks.erase(it);
                else {
                    st.zeroStreak = 0;
                    ++it;
                }
            } else {
                ++it;
            }
        }

        // 3) 空表退出（留复用窗口）
        if (tasks.empty()) {
            if (++emptySeen >= opts.emptyTicks) {
                MLog(L"monitor: no tasks, exiting");
                break;
            }
        } else {
            emptySeen = 0;
        }
    }

    if (ready) {
        ResetEvent(ready);
        CloseHandle(ready);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI 侧拉起
// ---------------------------------------------------------------------------

bool EnsureMonitorRunning(const std::wstring& exePath)
{
    // 就绪 = ready 事件置位（monitor 已过领导权获取步骤）。
    auto readyOrWait = []() -> bool {
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, SessionReadyEventName().c_str());
        if (!ev)
            return false;   // 旧 monitor 未创建事件：按互斥体路径处理
        DWORD w = WaitForSingleObject(ev, 2000);
        CloseHandle(ev);
        return w == WAIT_OBJECT_0;
    };

    // 健康判定（B2）：互斥体存在 ∧ 心跳新鲜（status 文件 mtime < 3s）。
    // 仅有互斥体不足以判定——挂起的 monitor 持互斥体但永不收编任务
    // （docs/11 blocker-2，140 轮 1 次，零日志 1 线程阻塞形态）。
    auto heartbeatFresh = []() -> bool {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(StatusFilePath().c_str(),
                                  GetFileExInfoStandard, &fad))
            return false;
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) + nowFt.dwLowDateTime;
        ULONGLONG then = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32)
                         + fad.ftLastWriteTime.dwLowDateTime;
        return now > then && (now - then) < 3ull * 10000000ull;
    };

    {
        HANDLE probe = OpenMutexW(SYNCHRONIZE, FALSE, SessionMutexName().c_str());
        if (probe) {
            CloseHandle(probe);
            if (heartbeatFresh()) {
                readyOrWait();   // 健康：等其就绪信号（至多 2s；超时不阻断）
                return true;
            }
            // 挂起回收：从 status 文件读 pid，击杀（我们的 monitor 进程；
            // 互斥体随句柄关闭释放/abandoned，新实例接管），再走拉起路径。
            model::v2::V2Err e;
            std::wstring st = model::v2::FileReadAll(StatusFilePath(), &e);
            DWORD wedgedPid = 0;
            size_t p = st.find(L"pid=");
            if (p != std::wstring::npos)
                wedgedPid = (DWORD)wcstoul(st.c_str() + p + 4, nullptr, 10);
            if (wedgedPid && wedgedPid != GetCurrentProcessId()) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                       wedgedPid);
                if (h) {
                    TerminateProcess(h, 1);
                    WaitForSingleObject(h, 3000);
                    CloseHandle(h);
                }
            } else {
                DeleteFileW(StatusFilePath().c_str());   // 陈旧 status：清掉
            }
            // 落到下方拉起路径
        }
    }
    std::wstring cmd = L"\"" + exePath + L"\" --monitor";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    DWORD flags = DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, flags, nullptr,
                        nullptr, &si, &pi)) {
        // Job 不允许 breakaway → 无标志重试（00 §5 同款回退）
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                            DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            return false;
        }
    }
    // 等就绪信号（领导权步骤完成）+ 心跳可见，而非仅互斥体——消除 spawn
    // 落在 leader 真空窗的竞态
    for (int i = 0; i < 40; ++i) {
        Sleep(50);
        if (readyOrWait() && heartbeatFresh())
            break;
    }
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    return readyOrWait();
}

} // namespace sbie::monitor
