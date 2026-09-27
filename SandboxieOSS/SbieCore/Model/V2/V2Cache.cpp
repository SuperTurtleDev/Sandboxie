// Sandboxie-OSS — SbieCore/Model/V2/V2Cache.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2Cache.h 实现。

#include "V2Cache.h"
#include "../../Util/Utf8.h"

#include <windows.h>

namespace sbie::model::v2 {

std::string BuildCacheText(const std::wstring& box, const std::wstring& boxDir,
                           const std::wstring& sandboxIniPath,
                           const std::vector<IniKeyValue>& kv,
                           const std::vector<std::wstring>& appliedTemplates)
{
    (void)boxDir;   // boxDir 进 kv（FileRootPath 恒显式）；参数保留给诊断扩展
    std::wstring t;
    t += L"# v2 expanded box cache (auto-generated; edit sandbox.ini + run sync-config)\n";
    t += L"# source: " + sandboxIniPath + L"\n";
    t += L"# generated: " + NowIsoTimestamp() + L"\n";
    if (!appliedTemplates.empty()) {
        t += L"# templates applied:";
        for (const auto& a : appliedTemplates)
            t += L" " + a;
        t += L"\n";
    }
    t += L"[" + box + L"]\n";
    for (const auto& e : kv)
        t += e.key + L"=" + e.value + L"\n";
    return util::WideToUtf8(t);
}

V2Err WriteBoxCache(const std::wstring& box, const std::wstring& boxDir,
                    const std::wstring& sandboxIniPath,
                    const std::vector<IniKeyValue>& kv,
                    const std::vector<std::wstring>& appliedTemplates)
{
    if (!EnsureRuntimeDirs())
        return {SbieStatus::GENERIC, L"cannot create runtime dirs under " + AppDataRoot()};
    std::string text = BuildCacheText(box, boxDir, sandboxIniPath, kv, appliedTemplates);
    V2Err e = WriteTextFileAtomic(CachePathFor(box), text);
    if (!e.Ok())
        return {SbieStatus::CACHE_INVALID, L"cache write failed: " + e.msg};
    return ValidateCacheFile(box, boxDir);
}

V2Err ValidateCacheFile(const std::wstring& box, const std::wstring& boxDir)
{
    const std::wstring path = CachePathFor(box);
    IniFileData ini;
    V2Err e = ParseIniFile(path, &ini);
    if (!e.Ok())
        return {SbieStatus::CACHE_INVALID, L"cache self-check: " + e.msg};
    if (ini.sections.size() != 1)
        return {SbieStatus::CACHE_INVALID,
                L"cache self-check: must be exactly one section"};
    const IniSectionData& sec = ini.sections[0];
    if (_wcsicmp(sec.name.c_str(), box.c_str()) != 0)
        return {SbieStatus::CACHE_INVALID,
                L"cache self-check: section name != box name"};
    bool haveRoot = false;
    for (const auto& kv : sec.entries) {
        if (_wcsicmp(kv.key.c_str(), L"Template") == 0)
            return {SbieStatus::CACHE_INVALID,
                    L"cache self-check: residual Template= line (must be zero)"};
        if (_wcsicmp(kv.key.c_str(), L"FileRootPath") == 0) {
            haveRoot = true;
            if (_wcsicmp(kv.value.c_str(), boxDir.c_str()) != 0)
                return {SbieStatus::CACHE_INVALID,
                        L"cache self-check: FileRootPath mismatch: " + kv.value};
        }
    }
    if (!haveRoot)
        return {SbieStatus::CACHE_INVALID, L"cache self-check: missing FileRootPath"};
    return {};
}

bool CacheExists(const std::wstring& box)
{
    return PathExists(CachePathFor(box));
}

V2Err DeleteBoxCache(const std::wstring& box)
{
    const std::wstring path = CachePathFor(box);
    // 顺手清本盒的陈旧原子写残骸（docs/10 §6.2：>10min 的 *.tmp；新鲜的
    // 可能是并发写者在途，不动。tmp 名 = <box>.ini.<pid>.<tick>.tmp，见
    // WriteTextFileAtomic 的唯一化命名）
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L".*.tmp").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32)
                            + nowFt.dwLowDateTime;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                ULONGLONG then = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                                 + fd.ftLastWriteTime.dwLowDateTime;
                if (now > then
                    && (now - then) > 10ull * 60 * 10000000ull /*10min*/)
                    DeleteFileW((BoxesDir() + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!PathExists(path))
        return {};
    if (!DeleteFileW(path.c_str()))
        return {SbieStatus::GENERIC, L"cannot delete cache: " + path + L" ("
                    + std::to_wstring(GetLastError()) + L")"};
    return {};
}

} // namespace sbie::model::v2
