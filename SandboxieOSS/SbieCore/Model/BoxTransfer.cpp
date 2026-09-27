// Sandboxie-OSS — SbieCore/Model/BoxTransfer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 实现见 BoxTransfer.h 文件头。规格锚点：docs/07 §3.2 07-P1-1/2/4
//（类型键组 / Duplicate Box / BoxTransfer 导出导入的 CLI 化建议）。

#include "BoxTransfer.h"
#include "Boxes.h"
#include "ConfigStore.h"
#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/PathMapper.h"
#include "../Util/Utf8.h"
#include "../Util/Zip.h"

#include <windows.h>

#include <cwchar>

namespace sbie::model {

namespace {

// 递归建目录（已存在不视为错误）
bool EnsureDirRecursive(const std::wstring& dir)
{
    if (dir.size() < 2)
        return false;
    const DWORD at = GetFileAttributesW(dir.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        return (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t cut = dir.find_last_of(L"\\");
    if (cut != std::wstring::npos && cut > 2) {
        if (!EnsureDirRecursive(dir.substr(0, cut)))
            return false;
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != 0
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

void TrimTrailingBackslash(std::wstring* s)
{
    while (s->size() > 3 && (*s)[s->size() - 1] == L'\\')
        s->pop_back();
}

// 目录是否存在（重解析点不算——口径与拷贝遍历一致）
bool DirExists(const std::wstring& dir)
{
    const DWORD at = GetFileAttributesW(dir.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 07-P1-1 类型预设
// ---------------------------------------------------------------------------

const std::vector<BoxTypePreset>& BoxTypePresets()
{
    // 键组 = docs/07 §3.2 07-P1-1 记录的 NewBoxWizard 类型定义规格
    //（Wizards/NewBoxWizard.cpp:206-226 落键语义：+Data Protection 为叠加
    // UsePrivacyMode=y；Compartment 叠加 NoSecurityIsolation=y 与
    // Template=RpcPortBindingsExt 追加）。向导另写 BorderColor（GUI 主题色）
    // ——07 规格记录未列入键组，CLI 不写（07-N-A-1 视觉键域，决策 docs/04 §17）。
    static const std::vector<BoxTypePreset> table = {
        { L"hardening", L"Security Hardened (UseSecurityMode=y)",
          { { L"UseSecurityMode", L"y", false } } },
        { L"hardened-plus",
          L"Security Hardened + Data Protection (UseSecurityMode=y,"
          L" UsePrivacyMode=y)",
          { { L"UsePrivacyMode", L"y", false },
            { L"UseSecurityMode", L"y", false } } },
        { L"standard", L"Standard sandbox (Enabled=y only; wizard default)",
          {} },
        { L"standard-plus", L"Standard + Data Protection (UsePrivacyMode=y)",
          { { L"UsePrivacyMode", L"y", false } } },
        { L"app",
          L"Application Compartment (NoSecurityIsolation=y,"
          L" Template=RpcPortBindingsExt)",
          { { L"NoSecurityIsolation", L"y", false },
            { L"Template", L"RpcPortBindingsExt", true } } },
        { L"app-plus",
          L"Application Compartment + Data Protection (NoSecurityIsolation=y,"
          L" UsePrivacyMode=y, Template=RpcPortBindingsExt)",
          { { L"UsePrivacyMode", L"y", false },
            { L"NoSecurityIsolation", L"y", false },
            { L"Template", L"RpcPortBindingsExt", true } } },
    };
    return table;
}

const BoxTypePreset* FindBoxTypePreset(const std::wstring& type)
{
    for (const BoxTypePreset& p : BoxTypePresets())
        if (_wcsicmp(p.type, type.c_str()) == 0)
            return &p;
    return nullptr;
}

SbieStatus ApplyBoxTypeKeys(const std::wstring& box,
                            const BoxTypePreset& preset,
                            const std::wstring& password)
{
    ConfigStore cfg;
    SbieStatus st = SbieStatus::OK;
    for (size_t i = 0; i < preset.keys.size() && st == SbieStatus::OK; ++i) {
        const BoxTypeKey& k = preset.keys[i];
        const bool refresh = i + 1 == preset.keys.size();
        st = k.append ? cfg.SetAppend(box, k.key, k.value, refresh, password)
                      : cfg.Set(box, k.key, k.value, refresh, password);
    }
    return st;
}

// ---------------------------------------------------------------------------
// 节文本 / 目录树拷贝
// ---------------------------------------------------------------------------

SbieStatus ReadBoxSection(const std::wstring& box, std::wstring* text)
{
    text->clear();
    ConfigStore cfg;
    std::vector<std::wstring> settings = cfg.ListSettings(box);
    for (const std::wstring& key : settings) {
        for (const std::wstring& v : cfg.GetList(box, key))
            *text += key + L"=" + v + L"\n";
    }
    return SbieStatus::OK;
}

SbieStatus CopyDirTree(const std::wstring& srcDir, const std::wstring& dstDir,
                       TransferStats* stats)
{
    if (!DirExists(srcDir))
        return SbieStatus::NOT_FOUND;
    if (!EnsureDirRecursive(dstDir))
        return SbieStatus::GENERIC;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((srcDir + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;   // 重解析点跳过（BoxUsage/Recovery 同口径）
        const std::wstring srcFull = srcDir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            st = CopyDirTree(srcFull, dstDir + L"\\" + name, stats);
            if (st == SbieStatus::OK && stats)
                ++stats->dirs;
        } else {
            if (!CopyFileW(srcFull.c_str(), (dstDir + L"\\" + name).c_str(),
                           FALSE /*允许覆盖*/)) {
                st = SbieStatus::GENERIC;
                break;
            }
            if (stats) {
                ++stats->files;
                stats->bytes += ((unsigned long long)fd.nFileSizeHigh << 32)
                                | fd.nFileSizeLow;
            }
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return st;
}

// ---------------------------------------------------------------------------
// 07-P1-4 复制沙箱
// ---------------------------------------------------------------------------

SbieStatus CopyBox(const std::wstring& src, const std::wstring& dst,
                   bool withContent, const std::wstring& password,
                   TransferStats* contentStats)
{
    if (contentStats)
        *contentStats = TransferStats{};
    if (_wcsicmp(src.c_str(), dst.c_str()) == 0)
        return SbieStatus::INVALID;
    SbieStatus st = BoxRepository::ValidateName(dst);
    if (st != SbieStatus::OK)
        return st;

    bool enabled = false, exists = false;
    st = drv::IsBoxEnabled(src, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (!exists)
        return SbieStatus::NOT_FOUND;   // 源不存在
    st = drv::IsBoxEnabled(dst, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (exists)
        return SbieStatus::NOT_FOUND;   // 目标已存在（CLI 语义 5）

    std::wstring sectionData;
    ReadBoxSection(src, &sectionData);
    if (sectionData.empty())
        sectionData = L"Enabled=y\n";   // 空节兜底（保持 Create 语义）

    // 节替换写入新名（Rename 前半，04 §8.15；refresh=true 使新箱即刻可见）
    st = ConfigStore().Set(dst, L"", sectionData, true, password);
    if (st != SbieStatus::OK)
        return st;

    if (withContent) {
        BoxRepository repo(nullptr, svc::SvcClient::Instance());
        BoxInfo s, d;
        if (repo.GetInfo(src, &s) != SbieStatus::OK
            || repo.GetInfo(dst, &d) != SbieStatus::OK)
            return SbieStatus::GENERIC;
        if (s.fileRoot.empty() || d.fileRoot.empty())
            return SbieStatus::GENERIC;
        std::wstring srcRoot = s.fileRoot, dstRoot = d.fileRoot;
        TrimTrailingBackslash(&srcRoot);
        TrimTrailingBackslash(&dstRoot);
        // 源未初始化（无目录）= 空内容复制（成功、0 文件）
        if (DirExists(srcRoot))
            return CopyDirTree(srcRoot, dstRoot, contentStats);
    }
    return SbieStatus::OK;
}

// ---------------------------------------------------------------------------
// 07-P1-2 导出 / 导入
// ---------------------------------------------------------------------------

namespace {

// 归档形态：目录树 → content/<rel> 条目（先目录后文件，父先子的序）
SbieStatus ZipAddTree(util::ZipWriter& z, const std::wstring& dir,
                      const std::wstring& rel, TransferStats* st)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus stt = SbieStatus::OK;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        const std::wstring full = dir + L"\\" + name;
        const std::wstring entry = rel.empty() ? name : rel + L"/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!z.AddDir(entry + L"/")) {
                stt = SbieStatus::GENERIC;
                break;
            }
            if (st)
                ++st->dirs;
            stt = ZipAddTree(z, full, entry, st);
        } else {
            if (!z.AddFile(entry, full)) {
                stt = SbieStatus::GENERIC;
                break;
            }
            if (st) {
                ++st->files;
                st->bytes += ((unsigned long long)fd.nFileSizeHigh << 32)
                             | fd.nFileSizeLow;
            }
        }
        if (stt != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return stt;
}

// 文本文件整读（UTF-8；容忍/剥除 BOM 与 \r）
bool ReadTextFile(const std::wstring& path, std::wstring* out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || sz.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(f);
        return false;   // 8 MiB 上限（ini 节文本量级远小于此）
    }
    std::string bytes((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = TRUE;
    if (sz.QuadPart > 0)
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr)
             && got == bytes.size();
    CloseHandle(f);
    if (!ok)
        return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF
        && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    *out = util::Utf8ToWide(bytes);
    // \r\n → \n（包可能经外部编辑器触碰过）
    std::wstring norm;
    norm.reserve(out->size());
    for (wchar_t c : *out) {
        if (c != L'\r')
            norm.push_back(c);
    }
    *out = std::move(norm);
    return true;
}

} // namespace

SbieStatus ExportBox(const std::wstring& box, const std::wstring& dest,
                     bool asArchive, TransferStats* out, std::wstring* detail)
{
    if (out)
        *out = TransferStats{};
    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    BoxInfo bi;
    SbieStatus st = repo.GetInfo(box, &bi);
    if (st == SbieStatus::NOT_FOUND || !bi.exists)
        return SbieStatus::NOT_FOUND;
    if (st != SbieStatus::OK)
        return st;

    std::wstring sectionData;
    ReadBoxSection(box, &sectionData);
    if (sectionData.empty())
        sectionData = L"Enabled=y\n";
    const std::string ini = util::WideToUtf8(sectionData);

    std::wstring root = bi.fileRoot;
    TrimTrailingBackslash(&root);
    const bool hasContent = DirExists(root);

    if (asArchive) {
        util::ZipWriter z;
        if (!z.Open(dest)) {
            if (detail)
                *detail = z.LastError();
            return SbieStatus::GENERIC;
        }
        if (!z.AddData(L"box.ini", ini.data(), ini.size())
            || (hasContent && !z.AddDir(L"content/"))) {
            if (detail)
                *detail = z.LastError();
            z.Abandon();
            return SbieStatus::GENERIC;
        }
        if (hasContent) {
            st = ZipAddTree(z, root, L"content", out);
            if (st != SbieStatus::OK) {
                if (detail)
                    *detail = z.LastError();
                z.Abandon();
                return st;
            }
        }
        if (!z.Finalize()) {
            if (detail)
                *detail = z.LastError();
            return SbieStatus::GENERIC;
        }
        return SbieStatus::OK;
    }

    // 目录形态：dest/box.ini + dest/content/…
    if (!EnsureDirRecursive(dest)) {
        if (detail)
            *detail = L"cannot create export directory: " + dest;
        return SbieStatus::GENERIC;
    }
    HANDLE f = CreateFileW((dest + L"\\box.ini").c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (detail)
            *detail = L"cannot write box.ini in: " + dest;
        return SbieStatus::GENERIC;
    }
    DWORD written = 0;
    const BOOL ok = ini.empty()
        ? TRUE
        : WriteFile(f, ini.data(), (DWORD)ini.size(), &written, nullptr)
              && written == (DWORD)ini.size();
    CloseHandle(f);
    if (!ok) {
        if (detail)
            *detail = L"write failed: " + dest + L"\\box.ini";
        return SbieStatus::GENERIC;
    }
    if (hasContent) {
        st = CopyDirTree(root, dest + L"\\content", out);
        if (st != SbieStatus::OK) {
            if (detail)
                *detail = L"content copy failed under: " + dest + L"\\content";
            return st;
        }
    }
    return SbieStatus::OK;
}

SbieStatus ImportBox(const std::wstring& packagePath,
                     const std::wstring& newName, bool fromArchive,
                     const std::wstring& password, TransferStats* out,
                     std::wstring* detail)
{
    if (out)
        *out = TransferStats{};
    SbieStatus st = BoxRepository::ValidateName(newName);
    if (st != SbieStatus::OK)
        return st;
    bool enabled = false, exists = false;
    st = drv::IsBoxEnabled(newName, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    if (exists)
        return SbieStatus::NOT_FOUND;   // 同名已存在（CLI 语义 5）

    std::wstring sectionData;
    if (fromArchive) {
        // 包文件不存在 → NOT_FOUND（与目录形态"package directory not
        // found"同码；其余打不开/非 zip = GENERIC 坏包路径）
        const DWORD pat = GetFileAttributesW(packagePath.c_str());
        if (pat == INVALID_FILE_ATTRIBUTES) {
            if (detail)
                *detail = L"package not found: " + packagePath;
            return SbieStatus::NOT_FOUND;
        }
        util::ZipReader z;
        if (!z.Open(packagePath)) {
            if (detail)
                *detail = z.LastError();
            return SbieStatus::GENERIC;   // 坏包（打不开/非 zip）
        }
        const util::ZipReader::Entry* ini = z.Find(L"box.ini");
        if (!ini) {
            if (detail)
                *detail = L"bad package (box.ini missing): " + packagePath;
            return SbieStatus::INVALID;
        }
        std::vector<uint8_t> data;
        if (!z.ReadEntry(*ini, &data)) {
            if (detail)
                *detail = z.LastError();
            return SbieStatus::GENERIC;
        }
        sectionData = util::Utf8ToWide(std::string(
            (const char*)data.data(), data.size()));
        // 排除 \r（robustness，与目录形态同口径）
        std::wstring norm;
        norm.reserve(sectionData.size());
        for (wchar_t c : sectionData)
            if (c != L'\r')
                norm.push_back(c);
        sectionData = std::move(norm);
        if (sectionData.empty()) {
            if (detail)
                *detail = L"bad package (empty box.ini): " + packagePath;
            return SbieStatus::INVALID;
        }

        // 建节（fail-fast：密码/锁配置在此暴露）
        st = ConfigStore().Set(newName, L"", sectionData, true, password);
        if (st != SbieStatus::OK)
            return st;

        // 新箱 FileRoot（节已建，GetInfo 可得）→ 还原 content/…
        BoxRepository repo(nullptr, svc::SvcClient::Instance());
        BoxInfo bi;
        if (repo.GetInfo(newName, &bi) != SbieStatus::OK
            || bi.fileRoot.empty()) {
            if (detail)
                *detail = L"cannot resolve FileRoot for: " + newName;
            return SbieStatus::GENERIC;
        }
        std::wstring root = bi.fileRoot;
        TrimTrailingBackslash(&root);
        for (size_t i = 0; i < z.Count(); ++i) {
            const util::ZipReader::Entry* e = z.At(i);
            if (!e)
                break;
            std::wstring rel;
            if (e->name.size() >= 8
                && (_wcsnicmp(e->name.c_str(), L"content/", 8) == 0
                    || _wcsnicmp(e->name.c_str(), L"content\\", 8) == 0))
                rel = e->name.substr(8);
            else
                continue;   // box.ini 与未知条目跳过
            for (wchar_t& c : rel)
                if (c == L'/')
                    c = L'\\';
            if (rel.empty())
                continue;
            const std::wstring dest = root + L"\\" + rel;
            if (e->isDir) {
                if (!EnsureDirRecursive(dest)) {
                    if (detail)
                        *detail = L"cannot create directory: " + dest;
                    return SbieStatus::GENERIC;
                }
                if (out)
                    ++out->dirs;
                continue;
            }
            const size_t cut = dest.find_last_of(L'\\');
            if (cut != std::wstring::npos
                && !EnsureDirRecursive(dest.substr(0, cut))) {
                if (detail)
                    *detail = L"cannot create directory: "
                              + dest.substr(0, cut);
                return SbieStatus::GENERIC;
            }
            if (!z.ExtractTo(*e, dest)) {
                if (detail)
                    *detail = z.LastError();
                return SbieStatus::GENERIC;
            }
            if (out) {
                ++out->files;
                out->bytes += e->size;
            }
        }
        return SbieStatus::OK;
    }

    // 目录形态包：packagePath/box.ini + packagePath/content/
    std::wstring ini = packagePath;
    TrimTrailingBackslash(&ini);
    if (!DirExists(ini)) {
        if (detail)
            *detail = L"package directory not found: " + packagePath;
        return SbieStatus::NOT_FOUND;
    }
    if (!ReadTextFile(ini + L"\\box.ini", &sectionData)) {
        if (detail)
            *detail = L"bad package (box.ini missing/unreadable): " + ini;
        return SbieStatus::INVALID;
    }
    if (sectionData.empty()) {
        if (detail)
            *detail = L"bad package (empty box.ini): " + ini;
        return SbieStatus::INVALID;
    }
    st = ConfigStore().Set(newName, L"", sectionData, true, password);
    if (st != SbieStatus::OK)
        return st;

    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    BoxInfo bi;
    if (repo.GetInfo(newName, &bi) != SbieStatus::OK || bi.fileRoot.empty()) {
        if (detail)
            *detail = L"cannot resolve FileRoot for: " + newName;
        return SbieStatus::GENERIC;
    }
    std::wstring root = bi.fileRoot;
    TrimTrailingBackslash(&root);
    if (DirExists(ini + L"\\content")) {
        st = CopyDirTree(ini + L"\\content", root, out);
        if (st != SbieStatus::OK) {
            if (detail)
                *detail = L"content restore failed into: " + root;
            return st;
        }
    }
    return SbieStatus::OK;
}

} // namespace sbie::model
