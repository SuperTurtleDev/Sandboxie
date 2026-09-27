// Sandboxie-OSS — sbie-cli/server/Guardian.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 空箱守护监视器实现（契约：本文件 + Guardian.h；规格 docs/07 §3.1 07-P0-2；
// 语义决策记录 docs/04 §16）。
//
// 线程模型：专职监视线程（1s 轮询）。行为执行（触发器 CreateProcess、文件
// 清理、配置读）在本线程直接执行；唯一触 SbieSvc 的路径（AutoRemove 的节
// 删除）经 SvcProxy 专职线程（03 §1 线程亲和，SvcCall）。

#include "Guardian.h"
#include "LogPump.h"
#include "ServerState.h"
#include "SvcProxy.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/Boxes.h"
#include "../../SbieCore/Model/ConfigStore.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <iterator>
#include <map>
#include <optional>
#include <thread>
#include <vector>

namespace sbie::server {

namespace {

constexpr DWORD kPollMs = 1000;   // 轮询粒度（任务书认可 1-2s；空箱→动作 ≤2s）

// ---------------------------------------------------------------------------
// 文件清理（与 cli\Commands\box_manage.cpp 同构副本——cli/server 模块隔离，
// 04 §1 禁止互相引用；原 Dispatcher.cpp 副本已并入本文件统一）
// ---------------------------------------------------------------------------

SbieStatus DeleteDirRecursive(const std::wstring& dir)
{
    if (dir.empty())
        return SbieStatus::GENERIC;
    DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES)
        return SbieStatus::OK;   // 不存在 = 已删
    if (!(at & FILE_ATTRIBUTE_DIRECTORY))
        return SbieStatus::GENERIC;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            st = DeleteDirRecursive(full);
        } else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (st != SbieStatus::OK)
        return st;
    // 目录句柄可能被驱动/SbieSvc 短暂持有；box 根目录带 READ-ONLY 属性，必须
    // 先清属性再删（实测 RemoveDirectoryW 对 R 目录返回 ACCESS_DENIED）
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(dir.c_str()))
            return SbieStatus::OK;
        if (i < 19)
            Sleep(500);
    }
    return SbieStatus::GENERIC;
}

SbieStatus CleanDirContents(const std::wstring& dir)
{
    DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES)
        return SbieStatus::OK;   // 未初始化的 box：无目录即已清空
    if (!(at & FILE_ATTRIBUTE_DIRECTORY))
        return SbieStatus::GENERIC;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            st = DeleteDirRecursive(full);
        else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return st;
}

// ---------------------------------------------------------------------------
// 监视器状态
// ---------------------------------------------------------------------------

std::atomic<bool>& GdRunning()
{
    static std::atomic<bool> r{ false };
    return r;
}
std::thread& GdThread()
{
    static std::thread t;
    return t;
}
std::atomic<bool>& GdActive()
{
    static std::atomic<bool> a{ false };
    return a;
}
std::atomic<long long>& GdFired()
{
    static std::atomic<long long> f{ 0 };
    return f;
}

// 行为可见性：guardian 动作合成日志（log dump / log watch 呈现）
void GLog(const std::wstring& text)
{
    AppendSyntheticLog(L"guardian: " + text);
}

// 配置键读取（驱动缓存，任意线程）：值为 y/Y → true
bool KeyIsY(const std::wstring& box, const wchar_t* key)
{
    auto v = model::ConfigStore().Get(box, key, 0, true, true);
    return v.has_value() && (*v == L"y" || *v == L"Y");
}

// 一次性沙箱命名规则（07 记录：Temp_ / Local_Temp_ 前缀，大小写不敏感）
bool IsOneShotBox(const std::wstring& name)
{
    return _wcsnicmp(name.c_str(), L"Temp_", 5) == 0
        || _wcsnicmp(name.c_str(), L"Local_Temp_", 11) == 0;
}

// ---------------------------------------------------------------------------
// 空箱事件处理（进程数 非零→零 转换时调用一次）
// ---------------------------------------------------------------------------

void OnBoxEmpty(const std::wstring& name)
{
    // ① OnBoxTerminate：最后一个进程退出时执行（先于清理——对齐 SandMan
    //    OnBoxClosed 的执行序）
    model::TriggerStats ts;
    (void)model::RunBoxTriggers(name, L"OnBoxTerminate", &ts);
    if (ts.run > 0 || ts.failed > 0)
        GLog(L"box '" + name + L"' became empty: OnBoxTerminate ran "
             + std::to_wstring(ts.run) + L" command(s)"
             + (ts.failed ? L" (" + std::to_wstring(ts.failed)
                            + L" failed/timed out)"
                          : std::wstring(L"")));

    // ② 行为键（Temp_/Local_Temp_ 前缀等同 AutoRemove）
    const bool autoRemove = KeyIsY(name, L"AutoRemove") || IsOneShotBox(name);
    const bool autoDelete = KeyIsY(name, L"AutoDelete");
    if (!autoRemove && !autoDelete)
        return;   // 无守护配置：仅触发器（若有）已执行
    GdFired().fetch_add(1);

    // ③ 清理内容（含 OnBoxDelete 触发器——"复用 clean 逻辑+触发器"）
    SbieStatus st = ExecuteBoxPurge(name, autoRemove ? PurgeMode::RemoveRoot
                                                     : PurgeMode::CleanContents,
                                    false);
    if (st == SbieStatus::ACCESS_DENIED) {
        GLog(L"box '" + name + L"' became empty: cleanup skipped"
             L" (NeverDelete=y)");
        return;
    }
    if (st != SbieStatus::OK) {
        GLog(L"box '" + name + L"' became empty: content cleanup failed"
             L" (file error)");
        return;
    }

    // ④ AutoRemove：连 ini 节删除（经 SvcProxy；锁配置无密码 → 记录失败）
    if (autoRemove) {
        SbieStatus s2 = SvcCall([&name] {
            return model::ConfigStore().Delete(name, L"*", std::nullopt, true,
                                               L"");
        });
        if (s2 != SbieStatus::OK) {
            GLog(L"box '" + name + L"' content purged but section removal"
                 L" failed (SbieSvc unavailable or config locked)");
            return;
        }
        GLog(L"box '" + name + L"' removed (AutoRemove"
             + (IsOneShotBox(name) ? std::wstring(L"/one-shot Temp_")
                                   : std::wstring(L""))
             + L")");
    } else {
        GLog(L"box '" + name + L"' auto-cleaned (AutoDelete)");
    }
}

void GuardianLoop()
{
    HANDLE stop = ServerState::Get().StopEvent();
    // box → 上次轮询是否有进程（只有 true→false 转换才触发；空箱不会被重复
    // 触发，新进程进来重新武装）
    std::map<std::wstring, bool> hadProcs;

    for (;;) {
        if (ServerState::Get().Stopping())
            break;
        if (WaitForSingleObject(stop, kPollMs) == WAIT_OBJECT_0)
            break;
        if (!drv::Loaded() || !drv::DriverAlive())
            continue;   // 驱动缺席/重启中：保持状态表，不误判

        // 全部真实 box（含 disabled——disable 不杀既有进程，仍需监视其归零）
        model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
        std::vector<model::BoxInfo> boxes = repo.EnumBoxes(false);

        for (const model::BoxInfo& bi : boxes) {
            // 空箱判定按全会话进程数（他 session 进程仍在时不误清；监视器
            // 自身的 session 归属不影响安全性）
            std::vector<ULONG> pids;
            if (drv::EnumBoxProcesses(bi.name, true, &pids) != SbieStatus::OK)
                continue;   // 枚举失败：不更新状态，下轮重试
            const bool now = !pids.empty();
            bool& before = hadProcs[bi.name];   // 缺省 false（新见 box 未武装）
            if (before && !now)
                OnBoxEmpty(bi.name);
            before = now;
        }

        // 收缩状态表：节已删除/改名的 box 移除（防无限增长）
        if (hadProcs.size() > boxes.size() * 2 + 16) {
            for (auto it = hadProcs.begin(); it != hadProcs.end();) {
                bool seen = false;
                for (const model::BoxInfo& bi : boxes)
                    if (bi.name == it->first) {
                        seen = true;
                        break;
                    }
                it = seen ? std::next(it) : hadProcs.erase(it);
            }
        }
    }
}

} // namespace

SbieStatus ExecuteBoxPurge(const std::wstring& name, PurgeMode mode,
                           bool noTriggers)
{
    // NeverDelete 保护（对齐 CSandBox::CleanBox 的 SB_DeleteProtect；与
    // box clean/delete 命令路径同序：保护检查先于触发器）
    if (KeyIsY(name, L"NeverDelete"))
        return SbieStatus::ACCESS_DENIED;

    // OnBoxDelete 触发器：删除内容前逐条执行（07-P0-1）
    if (!noTriggers) {
        model::TriggerStats ts;
        (void)model::RunBoxTriggers(name, L"OnBoxDelete", &ts);
    }

    model::BoxInfo bi;
    SbieStatus st = model::BoxRepository(nullptr,
                                         svc::SvcClient::Instance())
                        .GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return st;
    if (bi.fileRoot.empty())
        return SbieStatus::OK;   // 无根路径（未初始化）= 无内容可清
    return mode == PurgeMode::RemoveRoot ? DeleteDirRecursive(bi.fileRoot)
                                         : CleanDirContents(bi.fileRoot);
}

bool StartGuardians(bool forceOff)
{
    if (GdRunning().exchange(true))
        return GdActive().load();
    if (forceOff) {
        GdRunning().store(false);
        return false;
    }
    // 配置开关：GlobalSettings\GuardiansEnabled=n 关闭（缺省开）
    auto v = model::ConfigStore().Get(L"GlobalSettings", L"GuardiansEnabled",
                                      0, true, true);
    if (v.has_value() && (*v == L"n" || *v == L"N")) {
        GdRunning().store(false);
        return false;
    }
    if (!drv::Loaded() || !ServerState::Get().StopEvent()) {
        GdRunning().store(false);
        return false;
    }
    try {
        GdThread() = std::thread(GuardianLoop);
    } catch (...) {
        GdRunning().store(false);
        return false;
    }
    GdActive().store(true);
    return true;
}

void StopGuardians()
{
    if (GdThread().joinable())
        GdThread().join();
    GdRunning().store(false);
    GdActive().store(false);
}

bool GuardiansActive()
{
    return GdActive().load();
}

long long GuardiansFired()
{
    return GdFired().load();
}

} // namespace sbie::server
