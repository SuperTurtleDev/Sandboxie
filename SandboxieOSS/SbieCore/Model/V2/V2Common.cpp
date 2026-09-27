// Sandboxie-OSS — SbieCore/Model/V2/V2Common.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2Common.h 实现。注意 /W4 /WX：全部符号显式比较、无未用参数。

#include "V2Common.h"

#include "../../DriverApi/DriverApi.h"
#include "../../Util/Utf8.h"

#include <shlobj.h>

#include <cwctype>
#include <cstdio>
#include <cstring>

namespace sbie::model::v2 {

namespace {

// 保留节名前缀/名字（驱动节名空间全局共享，conf.c Conf_GlobalSettings 等）
bool IsReservedSectionName(const std::wstring& n)
{
    static const wchar_t* kReserved[] = {
        L"GlobalSettings", L"DefaultTemplates", L"TemplateSettings",
        L"UserSettings", L"Template", L"ImportBox",
    };
    for (const wchar_t* r : kReserved)
        if (_wcsicmp(n.c_str(), r) == 0)
            return true;
    if (_wcsnicmp(n.c_str(), L"UserSettings_", 13) == 0)
        return true;
    if (_wcsnicmp(n.c_str(), L"Template_", 9) == 0)
        return true;
    return false;
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b]))
        ++b;
    while (e > b && iswspace(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

} // namespace

// ---------------------------------------------------------------------------
// 路径布局
// ---------------------------------------------------------------------------

std::wstring AppDataRoot()
{
    wchar_t base[MAX_PATH] = L"";
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))
        || !base[0]) {
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, base))
            || !base[0]) {
            return L"C:\\SandboxieOSS";
        }
    }
    std::wstring r = base;
    if (!r.empty() && r.back() != L'\\')
        r += L'\\';
    return r + L"SandboxieOSS";
}

std::wstring BoxesDir()      { return AppDataRoot() + L"\\boxes"; }
std::wstring MonitorsDir()   { return AppDataRoot() + L"\\monitors"; }
std::wstring AliasIndexPath(){ return AppDataRoot() + L"\\aliases.json"; }
std::wstring MonitorLogPath(){ return AppDataRoot() + L"\\monitor.log"; }
std::wstring CachePathFor(const std::wstring& box) { return BoxesDir() + L"\\" + box + L".ini"; }
std::wstring TaskPathFor(const std::wstring& box)  { return MonitorsDir() + L"\\" + box + L".task"; }

static bool EnsureDirOnce(const std::wstring& dir)
{
    if (CreateDirectoryW(dir.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool EnsureRuntimeDirs()
{
    return EnsureDirOnce(AppDataRoot()) && EnsureDirOnce(BoxesDir())
           && EnsureDirOnce(MonitorsDir());
}

// ---------------------------------------------------------------------------
// 盒名
// ---------------------------------------------------------------------------

V2Err ValidateBoxName(const std::wstring& name)
{
    if (name.empty() || name.size() > 38)
        return {SbieStatus::INVALID, L"box name must be 1..38 characters: " + name};
    for (wchar_t c : name) {
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
                  || (c >= L'0' && c <= L'9') || c == L'_';
        if (!ok)
            return {SbieStatus::INVALID,
                    L"box name allows only [A-Za-z0-9_] (no dots/spaces): " + name};
    }
    if (IsReservedSectionName(name))
        return {SbieStatus::INVALID, L"box name is reserved: " + name};
    return {};
}

std::wstring NormalizeDirPath(const std::wstring& path)
{
    if (path.empty())
        return L"";
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return L"";
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(full) / sizeof(wchar_t)),
                               full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(wchar_t))
        return L"";
    std::wstring r(full, n);
    while (r.size() > 3 && r.back() == L'\\')   // 保留 "C:\"
        r.pop_back();
    return r;
}

std::wstring BoxNameFromPath(const std::wstring& boxDir)
{
    std::wstring p = boxDir;
    while (p.size() > 3 && p.back() == L'\\')
        p.pop_back();
    size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? p : p.substr(cut + 1);
}

// ---------------------------------------------------------------------------
// ini 解析
// ---------------------------------------------------------------------------

const IniSectionData* IniFileData::Find(const std::wstring& name) const
{
    for (const auto& s : sections)
        if (_wcsicmp(s.name.c_str(), name.c_str()) == 0)
            return &s;
    return nullptr;
}

IniSectionData* IniFileData::Find(const std::wstring& name)
{
    for (auto& s : sections)
        if (_wcsicmp(s.name.c_str(), name.c_str()) == 0)
            return &s;
    return nullptr;
}

std::wstring FileReadAll(const std::wstring& path, V2Err* err)
{
    if (err)
        *err = {};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE
                               | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err)
            *err = {SbieStatus::NOT_FOUND, L"cannot open: " + path};
        return L"";
    }
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;
        bytes.append(buf, got);
        if (bytes.size() > 8u * 1024 * 1024) {   // 8MB 上限，防病态文件
            CloseHandle(h);
            if (err)
                *err = {SbieStatus::INVALID, L"file too large: " + path};
            return L"";
        }
    }
    CloseHandle(h);

    // BOM 判定：UTF-16LE / UTF-8；默认按 UTF-8 处理（ASCII 兼容）
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF
        && (unsigned char)bytes[1] == 0xFE) {
        const wchar_t* w = (const wchar_t*)(bytes.data() + 2);
        size_t wn = (bytes.size() - 2) / sizeof(wchar_t);
        return std::wstring(w, wn);
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF
        && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    return util::Utf8ToWide(bytes);
}

V2Err ParseIniFile(const std::wstring& path, IniFileData* out)
{
    out->sections.clear();
    V2Err rd;
    std::wstring text = FileReadAll(path, &rd);
    if (!rd.Ok())
        return rd;

    IniSectionData cur;   // name 为空 = 首节前的散行（V2 文件不应出现；归入伪节）
    cur.name = L"";
    size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(
            pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;
        ++lineno;
        while (!line.empty() && (line.back() == L'\r'))
            line.pop_back();
        line = Trim(line);
        if (line.empty())
            continue;
        if (line[0] == L'#')
            continue;   // 注释（驱动语义：只认 '#'，conf.c:834）
        if (line[0] == L'[') {
            size_t close = line.find(L']');
            if (close == std::wstring::npos || close < 2)
                return {SbieStatus::INVALID,
                        path + L":" + std::to_wstring(lineno) + L": bad section header"};
            if (!cur.name.empty() || !cur.entries.empty())
                out->sections.push_back(cur);
            cur = IniSectionData();
            cur.name = Trim(line.substr(1, close - 1));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
            return {SbieStatus::INVALID,
                    path + L":" + std::to_wstring(lineno) + L": expected key=value"};
        IniKeyValue kv;
        kv.key = Trim(line.substr(0, eq));
        kv.value = Trim(line.substr(eq + 1));
        if (kv.key.empty() || kv.value.empty())
            return {SbieStatus::INVALID,
                    path + L":" + std::to_wstring(lineno) + L": empty key or value"};
        cur.entries.push_back(std::move(kv));
    }
    if (!cur.name.empty() || !cur.entries.empty())
        out->sections.push_back(cur);
    return {};
}

V2Err WriteTextFileAtomic(const std::wstring& path, const std::string& utf8)
{
    // tmp 名唯一化（pid+tick）：并发写同一目标时互不独占冲突（docs/11
    // blocker-3——固定 tmp 名 + 零共享 CREATE_ALWAYS 令 teardown 窗口内的
    // 并发 exec 只有 1-2/5 成功）
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId())
                       + L"." + std::to_wstring(GetTickCount()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {SbieStatus::GENERIC, L"cannot create " + tmp};
    std::string bom = "\xEF\xBB\xBF";
    DWORD got = 0;
    BOOL ok = WriteFile(h, bom.data(), (DWORD)bom.size(), &got, nullptr) && got == bom.size();
    if (ok) {
        ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &got, nullptr)
             && got == utf8.size();
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return {SbieStatus::GENERIC, L"write failed: " + tmp};
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return {SbieStatus::GENERIC, L"replace failed: " + path};
    }
    return {};
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring NowIsoTimestamp()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t buf[40];
    _snwprintf_s(buf, _TRUNCATE, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

size_t BoxProcessCount(const std::wstring& box, V2Err* err)
{
    if (err)
        *err = {};
    std::vector<ULONG> pids;
    SbieStatus s = drv::EnumBoxProcesses(box, true, &pids);
    if (s != SbieStatus::OK) {
        if (err)
            *err = {s, L"EnumBoxProcesses failed for " + box};
        return SIZE_MAX;
    }
    return pids.size();
}

size_t BoxUserProcessCount(const std::wstring& box, V2Err* err)
{
    if (err)
        *err = {};
    std::vector<ULONG> pids;
    SbieStatus s = drv::EnumBoxProcesses(box, true, &pids);
    if (s != SbieStatus::OK) {
        if (err)
            *err = {s, L"EnumBoxProcesses failed for " + box};
        return SIZE_MAX;
    }
    static const wchar_t* kBootstrapImages[] = {
        L"SandboxieRpcSs.exe", L"SandboxieDcomLaunch.exe", L"SandboxieBITS.exe",
        L"SandboxieWUAU.exe", L"SandboxieCrypto.exe",
    };
    size_t user = 0;
    for (ULONG pid : pids) {
        drv::ProcQuery pq;
        if (drv::QueryProcessById(pid, &pq) != SbieStatus::OK) {
            ++user;   // 瞬态（刚起未登记/刚死未清）：保守计为用户进程
            continue;
        }
        bool service = false;
        for (const wchar_t* img : kBootstrapImages)
            if (_wcsicmp(pq.image.c_str(), img) == 0) {
                service = true;
                break;
            }
        if (!service)
            ++user;
    }
    return user;
}

} // namespace sbie::model::v2
