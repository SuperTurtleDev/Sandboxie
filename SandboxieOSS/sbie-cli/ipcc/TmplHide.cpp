// Sandboxie-OSS — sbie-cli/ipcc/TmplHide.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 08-P2-7（波次 E）：Tmpl.Hide=y 模板名集合的实现。头注见 TmplHide.h。

#include "TmplHide.h"

#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace sbie::tmplhide {

namespace {

bool FileExistsW(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirOfFile(std::wstring path)
{
    const size_t p = path.find_last_of(L'\\');
    if (p == std::wstring::npos)
        return L"";
    return path.substr(0, p);
}

// 注册表 SbieSvc 服务的 ImagePath → 目录（Model Templates.cpp 同款链路的
// 独立副本——多文件边界；drv 未加载时这是 Templates.ini 的主要定位途径）
std::wstring SbieSvcDirFromRegistry()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\SbieSvc",
                      0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = L"";
    DWORD cb = sizeof(buf) - sizeof(WCHAR);
    const LSTATUS rc = RegQueryValueExW(hk, L"ImagePath", nullptr, nullptr,
                                        (LPBYTE)buf, &cb);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS)
        return L"";
    buf[cb / sizeof(WCHAR)] = L'\0';
    wchar_t* s = buf;
    if (*s == L'"') {   // ImagePath 可能带引号
        ++s;
        wchar_t* end = wcschr(s, L'"');
        if (end)
            *end = L'\0';
    }
    return DirOfFile(s);
}

// 安装目录搜索链（与 Model FindTemplatesIni 同序）
std::wstring InstallDir()
{
    const std::wstring lf = drv::LoadedFrom();
    if (!lf.empty()) {
        const std::wstring d = DirOfFile(lf);
        if (!d.empty())
            return d;
    }
    std::wstring dos;
    if (drv::Loaded() && Ok(drv::GetHomePath(nullptr, &dos))
        && !dos.empty())
        return dos;
    const std::wstring reg = SbieSvcDirFromRegistry();
    if (!reg.empty())
        return reg;
    wchar_t self[MAX_PATH * 2] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, self, (DWORD)std::size(self));
    if (n > 0 && n < std::size(self))
        return DirOfFile(self);
    return L"";
}

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out->clear();
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n)
        out->insert(out->end(), buf, buf + n);
    CloseHandle(h);
    return true;
}

// Sandboxie 家族两编码：UTF-16LE+BOM（Sandboxie.ini）/ UTF-8(+BOM)
// （Templates.ini）——与 Model ParseIniBytes 同两态
bool ReadTextFile(const std::wstring& path, std::wstring* out)
{
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(path, &bytes))
        return false;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        out->assign((const wchar_t*)(bytes.data() + 2),
                    (bytes.size() - 2) / sizeof(WCHAR));
        return true;
    }
    const char* p = (const char*)bytes.data();
    size_t n = bytes.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    *out = util::Utf8ToWide(std::string(p, n));
    return true;
}

std::wstring& TrimInPlace(std::wstring& s)
{
    const size_t b = s.find_first_not_of(L" \t");
    const size_t e = s.find_last_not_of(L" \t");
    if (b == std::wstring::npos) {
        s.clear();
        return s;
    }
    return s = s.substr(b, e - b + 1);
}

std::wstring ToLower(std::wstring s)
{
    for (wchar_t& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

constexpr const wchar_t kTmplPrefix[] = L"Template_";
constexpr size_t kTmplPrefixLen = std::size(kTmplPrefix) - 1;

// 单文件扫描：[Template_<名>] 节内出现 Tmpl.Hide=y（大小写不敏感）即计入。
// 每次调用现读现扫（不缓存）：调用面 = template list / server tpl.list，
// 非热路径；无共享态故线程安全。
void ScanFileForHidden(const std::wstring& path,
                       std::vector<std::wstring>* out)
{
    std::wstring text;
    if (!ReadTextFile(path, &text))
        return;
    std::wstring cur;   // 当前节模板名（空 = 不在 [Template_*] 节）
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = eol == std::wstring::npos
            ? text.substr(pos) : text.substr(pos, eol - pos);
        if (eol == std::wstring::npos)
            pos = text.size() + 1;
        else
            pos = eol + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
            line.pop_back();
        TrimInPlace(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            continue;
        if (line.front() == L'[' && line.back() == L']' && line.size() >= 2) {
            std::wstring sec = line.substr(1, line.size() - 2);
            TrimInPlace(sec);
            cur = sec.size() > kTmplPrefixLen
                      && _wcsnicmp(sec.c_str(), kTmplPrefix,
                                   kTmplPrefixLen) == 0
                      ? sec.substr(kTmplPrefixLen) : std::wstring();
            continue;
        }
        if (cur.empty())
            continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        std::wstring key = line.substr(0, eq);
        TrimInPlace(key);
        std::wstring val = line.substr(eq + 1);
        TrimInPlace(val);
        if (_wcsicmp(key.c_str(), L"Tmpl.Hide") == 0
            && _wcsicmp(val.c_str(), L"y") == 0) {
            const std::wstring low = ToLower(cur);
            bool dup = false;
            for (const std::wstring& e : *out)
                if (e == low)
                    dup = true;
            if (!dup)
                out->push_back(low);
        }
    }
}

} // namespace

std::vector<std::wstring> HiddenNames(const std::wstring& sandboxieIniPath)
{
    std::vector<std::wstring> out;

    const std::wstring dir = InstallDir();
    if (!dir.empty())
        ScanFileForHidden(dir + L"\\Templates.ini", &out);   // 官方目录

    // 本地模板（Sandboxie.ini 的 [Template_*]，同名遮蔽官方条目）
    std::wstring ini = sandboxieIniPath;
    if (ini.empty()) {
        // 自行定位（client 语境；server worker 线程应显式传路径——头注）
        svc::SvcClient& svc = svc::SvcClient::Instance();
        std::wstring p;
        bool isHome = false;
        if (svc.Connected() && Ok(svc.IniGetPath(&p, &isHome))
            && !p.empty() && FileExistsW(p)) {
            ini = p;
        } else if (!dir.empty() && FileExistsW(dir + L"\\Sandboxie.ini")) {
            ini = dir + L"\\Sandboxie.ini";
        } else if (FileExistsW(L"C:\\Windows\\Sandboxie.ini")) {
            ini = L"C:\\Windows\\Sandboxie.ini";
        }
    }
    if (!ini.empty())
        ScanFileForHidden(ini, &out);
    return out;
}

bool Contains(const std::vector<std::wstring>& hidden,
              const std::wstring& name)
{
    const std::wstring low = ToLower(name);
    for (const std::wstring& e : hidden)
        if (e == low)
            return true;
    return false;
}

} // namespace sbie::tmplhide
