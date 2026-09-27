// Sandboxie-OSS — SbieCore/Model/Boxes.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（01-license-map §2 允许范围，LGPL 仅参考未复制）：
//   QSbieAPI SbieAPI.cpp ReloadBoxes（:1171-1200）、CreateBox（:1459-1477）、
//   ValidateName（:1412-1445）、HasProcesses（:1600 附近）。
//
// M2 写路径登记（坑见 docs/04-modules.md §8）：
//   * Rename 经 SbieSvc 协议技巧 "setting 为空 + value=整节文本" 一步替换
//     （sbieiniserver.cpp SetSetting → CIniFile::SetValue 的 replace-section
//     分支，common/ini.cpp），再以 "setting=*" 删除旧节（RemoveSection 分支）。
//   * 删节请求 value_len==0，必须满足服务端 CheckRequest 的
//     h.length >= sizeof(SBIE_INI_SETTING_REQ) 下限 —— M1 SvcClient::IniSetSetting
//     对空值的定长按 offsetof(value) 计（少 4 字节）会被拒；本文件经
//     ConfigStore（内部用 SvcClient::Call 原始组装）绕开。

#include "Boxes.h"
#include "ConfigStore.h"
#include "../DriverApi/DriverApi.h"
#include "../Util/PathMapper.h"

#include <windows.h>

#include <cwchar>

namespace sbie::model {

namespace {

// 递归删除目录（box delete --files / rename 留下的旧目录不搬移，本函数仅
// 供 Delete 使用；对齐 QSbieAPI NtIo_DeleteFolderRecursively 行为）
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
    // 目录句柄可能被驱动/SbieSvc 短暂持有（参考 NtIo_WaitForFolder 的 10s
    // 等待）；box 根目录带 READ-ONLY 属性（Sandboxie 建目录时设置），必须
    // 先清属性再删——实测 RemoveDirectoryW 对 R 目录返回 ACCESS_DENIED。
    for (int i = 0; i < 20; ++i) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(dir.c_str()))
            return SbieStatus::OK;
        if (i < 19)
            Sleep(500);
    }
    return SbieStatus::GENERIC;
}

} // namespace

BoxRepository::BoxRepository(sbie::drv::Api* api, sbie::svc::SvcClient& svc)
    : api_(api), svc_(svc)
{
}

std::vector<std::wstring> BoxRepository::EnumSections()
{
    std::vector<std::wstring> out;
    drv::EnumConfSections(&out);
    return out;
}

std::vector<BoxInfo> BoxRepository::EnumBoxes(bool enabledOnly)
{
    std::vector<BoxInfo> out;
    // 全节枚举 + IsBox 过滤（QSbieAPI ReloadBoxes 同款：对每个节调 IsBox，
    // STATUS_SUCCESS=启用；STATUS_ACCOUNT_RESTRICTION=存在但禁用；其余非 box）
    for (const std::wstring& section : EnumSections()) {
        bool enabled = false, exists = false;
        if (drv::IsBoxEnabled(section, &enabled, &exists) != SbieStatus::OK
            || !exists)
            continue;
        if (enabledOnly && !enabled)
            continue;
        BoxInfo bi;
        bi.name = section;
        bi.enabled = enabled;
        bi.exists = true;
        GetInfo(section, &bi);   // 顺带填三根路径与进程数；失败字段保持缺省
        out.push_back(std::move(bi));
    }
    return out;
}

SbieStatus BoxRepository::GetInfo(const std::wstring& name, BoxInfo* out)
{
    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (!exists)
        return SbieStatus::NOT_FOUND;
    out->name = name;
    out->exists = true;
    out->enabled = enabled;

    std::wstring fileRoot, regRoot, ipcRoot;
    st = drv::QueryBoxPath(name, &fileRoot, &regRoot, &ipcRoot);
    if (st == SbieStatus::OK) {
        util::NtToDosPath(&fileRoot);
        out->fileRoot = std::move(fileRoot);
        out->regRoot = std::move(regRoot);
        out->ipcRoot = std::move(ipcRoot);
    }
    std::vector<ULONG> pids;
    st = drv::EnumBoxProcesses(name, false, &pids);
    out->hasProcesses = (st == SbieStatus::OK) && !pids.empty();
    return SbieStatus::OK;
}

SbieStatus BoxRepository::Create(const std::wstring& name)
{
    SbieStatus st = ValidateName(name);
    if (st != SbieStatus::OK)
        return st;
    bool enabled = false, exists = false;
    st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (exists)
        return SbieStatus::NOT_FOUND; // 已存在（调用方映射"5=已存在"）
    // CreateBox 语义：仅 SET Enabled=y（不建目录，04 §8.4）
    return svc_.IniSetSetting(name, L"Enabled", L"y",
                              svc::SvcClient::SetMode::Update, true, L"");
}

SbieStatus BoxRepository::Rename(const std::wstring& oldN, const std::wstring& newN)
{
    SbieStatus st = ValidateName(newN);
    if (st != SbieStatus::OK)
        return st;
    bool enabled = false, exists = false;
    st = drv::IsBoxEnabled(oldN, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (!exists)
        return SbieStatus::NOT_FOUND;
    st = drv::IsBoxEnabled(newN, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (exists)
        return SbieStatus::NOT_FOUND; // 新名已被占用（CLI 语义 5）

    // 读旧节全部设置（驱动缓存，NO_TEMPLS → 仅真实文件项），按
    // "Key=Value\n" 每值一行组装整节文本（多值设置重复 Key 行）
    ConfigStore cfg;
    std::vector<std::wstring> settings = cfg.ListSettings(oldN);
    std::wstring sectionData;
    for (const std::wstring& key : settings) {
        for (const std::wstring& v : cfg.GetList(oldN, key))
            sectionData += key + L"=" + v + L"\n";
    }
    if (sectionData.empty())
        sectionData = L"Enabled=y\n";   // 空节兜底（保持 Create 语义）

    // 一步写新节：setting 为空 + 非空 value = 服务端整节替换（坑记录 §8.10）
    st = svc_.IniSetSetting(newN, L"", sectionData,
                            svc::SvcClient::SetMode::Update, false, L"");
    if (st != SbieStatus::OK)
        return st;
    // 删旧节：setting="*"（ConfigStore::Delete 内部按 sizeof 下限组包）
    st = cfg.Delete(oldN, L"*", std::nullopt, true /*refresh 收尾*/, L"");
    if (st != SbieStatus::OK) {
        // 回滚新节，避免一半成功状态
        cfg.Delete(newN, L"*", std::nullopt, false, L"");
        return st;
    }
    // 目录不搬移（新节首进程启动时按新名建目录；旧目录留给 --files 手动清）
    return SbieStatus::OK;
}

SbieStatus BoxRepository::Delete(const std::wstring& name, bool delFiles,
                                 bool delSection)
{
    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (!exists)
        return SbieStatus::NOT_FOUND;

    // 有活动进程先拒（04 §4.3：提示 kill-all；BOX_BUSY=9）
    std::vector<ULONG> pids;
    st = drv::EnumBoxProcesses(name, false, &pids);
    if (st != SbieStatus::OK)
        return st;
    if (!pids.empty())
        return SbieStatus::BOX_BUSY;

    if (delFiles) {
        // NeverDelete 保护（对齐 CSandBox::CleanBox 的 SB_DeleteProtect）
        auto nd = ConfigStore().Get(name, L"NeverDelete", 0, true, true);
        if (nd.has_value() && *nd == L"y")
            return SbieStatus::ACCESS_DENIED;
        BoxInfo bi;
        if (GetInfo(name, &bi) == SbieStatus::OK && !bi.fileRoot.empty()) {
            st = DeleteDirRecursive(bi.fileRoot);
            if (st != SbieStatus::OK)
                return st;
        }
    }
    if (delSection)
        return ConfigStore().Delete(name, L"*", std::nullopt, true, L"");
    return SbieStatus::OK;
}

SbieStatus BoxRepository::SetEnabled(const std::wstring& name, bool on)
{
    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(name, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (!exists)
        return SbieStatus::NOT_FOUND;
    return svc_.IniSetSetting(name, L"Enabled", on ? L"y" : L"n",
                              svc::SvcClient::SetMode::Update, true, L"");
}

SbieStatus BoxRepository::TerminateAll(const std::wstring& name)
{
    return svc_.KillAll(name, (ULONG)-1);
}

SbieStatus BoxRepository::ValidateName(const std::wstring& name)
{
    // 对齐 QSbieAPI ValidateName（SbieAPI.cpp:1412-1445）
    if (name.empty() || name.size() > 38)
        return SbieStatus::INVALID;
    for (wchar_t c : name) {
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
               || (c >= L'0' && c <= L'9') || c == L'_';
        if (!ok)
            return SbieStatus::INVALID;
    }
    static const wchar_t* reserved[] = {
        L"aux", L"con", L"nul", L"prn", L"clock$", nullptr,
    };
    for (const wchar_t** r = reserved; *r; ++r)
        if (_wcsicmp(name.c_str(), *r) == 0)
            return SbieStatus::INVALID;
    wchar_t base[8];
    for (wchar_t n = L'0'; n <= L'9'; ++n) {
        swprintf_s(base, L"com%c", n);
        if (_wcsicmp(name.c_str(), base) == 0)
            return SbieStatus::INVALID;
        swprintf_s(base, L"lpt%c", n);
        if (_wcsicmp(name.c_str(), base) == 0)
            return SbieStatus::INVALID;
    }
    if (_wcsicmp(name.c_str(), L"GlobalSettings") == 0)
        return SbieStatus::INVALID;
    if (_wcsnicmp(name.c_str(), L"UserSettings_", 13) == 0)
        return SbieStatus::INVALID;
    return SbieStatus::OK;
}

namespace {

// %SANDBOX% → box 名（大小写不敏感；%SANDBOXPATH% 等其余 SandMan 变量
// 07 未记录 → 不替换，04 §16 决策）。注意匹配含两侧 '%'，"%SANDBOXPATH%"
// 不会误匹配（%SANDBOX% 要求闭百分号紧跟 X 之后）。
void ReplaceSandboxVar(std::wstring* s, const std::wstring& box)
{
    static const wchar_t* kVar = L"%sandbox%";
    const size_t varLen = 9;
    for (size_t pos = 0; pos < s->size();) {
        size_t hit = std::wstring::npos;
        for (size_t i = pos; i + varLen <= s->size(); ++i) {
            size_t j = 0;
            for (; j < varLen; ++j) {
                wchar_t a = (*s)[i + j];
                wchar_t b = kVar[j];
                if (a >= L'A' && a <= L'Z')
                    a += 32;
                if (a != b)
                    break;
            }
            if (j == varLen) {
                hit = i;
                break;
            }
        }
        if (hit == std::wstring::npos)
            return;
        s->replace(hit, varLen, box);
        pos = hit + box.size();
    }
}

} // namespace

std::wstring ExpandSandboxVar(const std::wstring& text, const std::wstring& box)
{
    std::wstring out = text;
    ReplaceSandboxVar(&out, box);
    return out;
}

SbieStatus RunBoxTriggers(const std::wstring& box, const std::wstring& setting,
                          TriggerStats* out)
{
    // 原样读值（noExpand=true：见 Boxes.h 头注——驱动展开在 SYSTEM 上下文，
    // 会破坏 %TEMP% 等用户变量；展开交给子进程）
    std::vector<std::wstring> cmds = ConfigStore().GetList(box, setting);
    TriggerStats stats;
    for (const std::wstring& raw : cmds) {
        std::wstring cmd = raw;
        ReplaceSandboxVar(&cmd, box);
        if (cmd.empty())
            continue;
        // CreateProcessW 需要可写命令行缓冲
        std::wstring mutableCmd = cmd;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr,
                                 FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi);
        if (!ok) {
            ++stats.failed;
            continue;
        }
        // 逐条：等待完成（≤15s，超时不杀——04 §16 决策）
        if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            if (code != 0)
                ++stats.failed;
            else
                ++stats.run;
        } else {
            ++stats.failed;   // 超时：进程存活，按失败计数但继续
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (out)
        *out = stats;
    return SbieStatus::OK;
}

} // namespace sbie::model
