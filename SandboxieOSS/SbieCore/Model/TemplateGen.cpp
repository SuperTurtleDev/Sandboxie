// Sandboxie-OSS — SbieCore/Model/TemplateGen.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 浏览器兼容模板生成器（07-P2-5）——实现见 TemplateGen.h 头注。
//
// 探测（全部只读）：
//   * 安装路径：注册表 App Paths（HKLM 64/32 视图 + HKCU，default 值 =
//     完整 exe 路径）→ 标准安装目录探测（Program Files / LocalAppData）；
//   * profile：Chromium 系 = <User Data>\Default，缺省时取首个 "Profile *"，
//     再缺省回退 User Data 根；Gecko 系 = Profiles\*（官方模板同款通配形，
//     覆盖全部 profile）；
//   * 变量化：命中 %LocalAppData% / %AppData% 实际根时改写为
//     "%Local AppData%" / "%AppData%"（官方 Templates.ini 形态；驱动
//     conf_expand.c 两种写法都展开——LocalAppData 映射注册表值名
//     "Local AppData"，core\drv\conf_expand.c:666-667）。
//
// 键面形态（GPL core install/Templates.ini 浏览器族参考，键面非代码）：
//   ForceProcess=<exe>（*_Force 族）；OpenFilePath=<exe>,<profile>\模式
//   （*_Bookmarks/Cookies/Passwords/Preferences/History/Profile_DirectAccess 族）。

#include "TemplateGen.h"
#include "ConfigStore.h"
#include "DriverApi/DriverApi.h"
#include "SvcClient/SvcClient.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <iterator>
#include <utility>

namespace sbie::model {

namespace {

//---------------------------------------------------------------------------
// 目录/注册表小工具
//---------------------------------------------------------------------------

bool DirExists(const std::wstring& path)
{
    const DWORD at = GetFileAttributesW(path.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExistsW(const std::wstring& path)
{
    const DWORD at = GetFileAttributesW(path.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty())
        return b;
    if (!a.empty() && a.back() == L'\\')
        return a + b;
    return a + L"\\" + b;
}

// 环境变量展开（%LocalAppData% 等标准 Windows 变量；探测用）
std::wstring ExpandVars(const std::wstring& s)
{
    wchar_t buf[MAX_PATH * 4] = L"";
    if (ExpandEnvironmentStringsW(s.c_str(), buf, (DWORD)std::size(buf)))
        return buf;
    return s;
}

// 注册表 App Paths 探测：hive 视图 × App Paths\<exe> 的 default 值
bool AppPathsLookup(HKEY root, REGSAM wow64, const wchar_t* exe,
                    std::wstring* out)
{
    const wchar_t* kBase =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
    std::wstring sub = std::wstring(kBase) + exe;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE | wow64, &k)
        != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD cb = sizeof(buf) - sizeof(WCHAR);
    const LSTATUS rc = RegQueryValueExW(k, nullptr, nullptr, nullptr,
                                        (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS)
        return false;
    buf[cb / sizeof(WCHAR)] = L'\0';
    std::wstring path = buf;
    // App Paths 值可能带引号
    if (!path.empty() && path.front() == L'"') {
        const size_t end = path.find(L'"', 1);
        path = end == std::wstring::npos ? path.substr(1)
                                         : path.substr(1, end - 1);
    }
    if (path.empty() || !FileExistsW(path))
        return false;
    *out = path;
    return true;
}

// 实际 %LocalAppData% / %AppData% 根（变量化改写用；展开环境变量取真值）
std::wstring LocalAppDataReal()
{
    return ExpandVars(L"%LocalAppData%");
}
std::wstring AppDataReal()
{
    return ExpandVars(L"%AppData%");
}

// 命中已知根 → 官方变量形（"%Local AppData%"/"%AppData%"）；未命中原样。
std::wstring Variablize(const std::wstring& dir)
{
    const std::wstring lad = LocalAppDataReal();
    if (!lad.empty() && _wcsnicmp(dir.c_str(), lad.c_str(), lad.size()) == 0
        && (dir.size() == lad.size() || dir[lad.size()] == L'\\'))
        return L"%Local AppData%" + dir.substr(lad.size());
    const std::wstring ad = AppDataReal();
    if (!ad.empty() && _wcsnicmp(dir.c_str(), ad.c_str(), ad.size()) == 0
        && (dir.size() == ad.size() || dir[ad.size()] == L'\\'))
        return L"%AppData%" + dir.substr(ad.size());
    return dir;
}

//---------------------------------------------------------------------------
// 浏览器目录（静态表）
//---------------------------------------------------------------------------

struct BrowserSpec {
    const wchar_t* key;        // CLI 名（小写）
    const wchar_t* display;
    const wchar_t* engine;     // L"chromium" | L"gecko"
    const wchar_t* exe;        // 主进程 exe（ForceProcess / App Paths 键名）
    // 标准安装目录（%var% 形态，逐个探测；App Paths 未命中时用）
    const wchar_t* const* installDirs;
    // 用户数据目录（chromium）或 profiles 根（gecko）；%var% 形态
    const wchar_t* userDataDir;
};

const wchar_t* const kChromeInst[] = {
    L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
    L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
    L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe",
    nullptr,
};
const wchar_t* const kEdgeInst[] = {
    L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
    L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
    nullptr,
};
const wchar_t* const kFirefoxInst[] = {
    L"%ProgramFiles%\\Mozilla Firefox\\firefox.exe",
    L"%ProgramFiles(x86)%\\Mozilla Firefox\\firefox.exe",
    nullptr,
};
const wchar_t* const kChromiumInst[] = {
    L"%LocalAppData%\\Chromium\\Application\\chrome.exe",
    L"%ProgramFiles%\\Chromium\\Application\\chrome.exe",
    nullptr,
};
const wchar_t* const kBraveInst[] = {
    L"%LocalAppData%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
    L"%ProgramFiles%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
    nullptr,
};
const wchar_t* const kVivaldiInst[] = {
    L"%LocalAppData%\\Vivaldi\\Application\\vivaldi.exe",
    L"%ProgramFiles%\\Vivaldi\\Application\\vivaldi.exe",
    nullptr,
};
const wchar_t* const kOperaInst[] = {
    L"%LocalAppData%\\Programs\\Opera\\opera.exe",
    L"%ProgramFiles%\\Opera\\opera.exe",
    nullptr,
};

// 表序即输出去重序（chrome 在前，detected-only 行不影响）
const BrowserSpec kBrowsers[] = {
    { L"chrome",   L"Google Chrome",   L"chromium", L"chrome.exe",
      kChromeInst,   L"%LocalAppData%\\Google\\Chrome\\User Data" },
    { L"edge",     L"Microsoft Edge",  L"chromium", L"msedge.exe",
      kEdgeInst,     L"%LocalAppData%\\Microsoft\\Edge\\User Data" },
    { L"firefox",  L"Mozilla Firefox", L"gecko",    L"firefox.exe",
      kFirefoxInst,  L"%AppData%\\Mozilla\\Firefox\\Profiles" },
    { L"chromium", L"Chromium",        L"chromium", L"chrome.exe",
      kChromiumInst, L"%LocalAppData%\\Chromium\\User Data" },
    { L"brave",    L"Brave",           L"chromium", L"brave.exe",
      kBraveInst,    L"%LocalAppData%\\BraveSoftware\\Brave-Browser\\User Data" },
    { L"vivaldi",  L"Vivaldi",         L"chromium", L"vivaldi.exe",
      kVivaldiInst,  L"%LocalAppData%\\Vivaldi\\User Data" },
    { L"opera",    L"Opera",           L"chromium", L"opera.exe",
      kOperaInst,    L"%AppData%\\Opera Software\\Opera Stable" },
};

const BrowserSpec* FindSpec(const std::wstring& key)
{
    for (const BrowserSpec& b : kBrowsers)
        if (_wcsicmp(b.key, key.c_str()) == 0)
            return &b;
    return nullptr;
}

// Chromium 系 profile 目录：User Data\Default → 首个 "Profile *" →
// User Data 根（存在时）；返回空 = 未找到
std::wstring DetectChromiumProfile(const std::wstring& userDataReal)
{
    if (userDataReal.empty() || !DirExists(userDataReal))
        return L"";
    const std::wstring def = JoinPath(userDataReal, L"Default");
    if (DirExists(def))
        return def;
    // "Profile 1" 等命名 profile
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((userDataReal + L"\\Profile *").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::wstring first;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                first = fd.cFileName;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!first.empty())
            return JoinPath(userDataReal, first);
    }
    return userDataReal;   // 有 User Data 无命名 profile：根兜底
}

// 首字母大写（Local_<Edge>）
std::wstring Capitalize(const std::wstring& s)
{
    if (s.empty())
        return s;
    std::wstring out = s;
    out[0] = (wchar_t)towupper(out[0]);
    return out;
}

} // namespace

//---------------------------------------------------------------------------
// 探测
//---------------------------------------------------------------------------

std::vector<BrowserDetect> DetectBrowsers()
{
    std::vector<BrowserDetect> out;
    for (const BrowserSpec& b : kBrowsers) {
        BrowserDetect d;
        d.key = b.key;
        d.display = b.display;
        d.engine = b.engine;
        d.exeName = b.exe;

        // 1) App Paths（HKLM 64 视图 → HKLM 32 视图 → HKCU）
        std::wstring path;
        if (AppPathsLookup(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, b.exe, &path)) {
            d.source = L"app-paths";
        } else if (AppPathsLookup(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
                                   b.exe, &path)) {
            d.source = L"app-paths";
        } else if (AppPathsLookup(HKEY_CURRENT_USER, 0, b.exe, &path)) {
            d.source = L"app-paths";
        } else {
            // 2) 标准安装目录探测
            for (const wchar_t* const* p = b.installDirs; *p; ++p) {
                const std::wstring cand = ExpandVars(*p);
                if (FileExistsW(cand)) {
                    path = cand;
                    d.source = L"program-files";
                    break;
                }
            }
        }
        d.exePath = path;
        d.installed = !path.empty();

        // 3) profile 目录（与安装无关——浏览器可卸载残留数据，反之亦然）
        const std::wstring udReal = ExpandVars(b.userDataDir);
        if (_wcsicmp(b.engine, L"gecko") == 0) {
            // Gecko：Profiles 根存在 → 通配形（官方模板风格，覆盖全部 profile）
            if (DirExists(udReal)) {
                d.profileDir = Variablize(udReal) + L"\\*";
                d.profileFound = true;
            }
        } else {
            const std::wstring prof = DetectChromiumProfile(udReal);
            if (!prof.empty()) {
                d.profileDir = Variablize(prof);
                d.profileFound = true;
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

//---------------------------------------------------------------------------
// 生成
//---------------------------------------------------------------------------

SbieStatus GenerateBrowserTemplate(
    const BrowserDetect& d,
    const std::vector<std::wstring>& accessCategories,
    bool forceProcess,
    GeneratedTemplate* out)
{
    out->lines.clear();
    out->text.clear();
    if (!d.installed && !d.profileFound)
        return SbieStatus::NOT_FOUND;

    const bool gecko = _wcsicmp(d.engine.c_str(), L"gecko") == 0;

    // 类别合法性 + 收集 OpenFilePath 模式（有序去重）
    std::vector<std::wstring> patterns;
    auto addPattern = [&patterns](const std::wstring& p) {
        for (const auto& e : patterns)
            if (_wcsicmp(e.c_str(), p.c_str()) == 0)
                return;
        patterns.push_back(p);
    };
    for (const std::wstring& cat : accessCategories) {
        const std::wstring P = d.profileDir;
        if (_wcsicmp(cat.c_str(), L"bookmarks") == 0) {
            if (gecko) {
                addPattern(P + L"\\bookmark*");
                addPattern(P + L"\\places*");
            } else {
                addPattern(P + L"\\Bookmarks*");
                addPattern(P + L"\\Favicons*");
            }
        } else if (_wcsicmp(cat.c_str(), L"history") == 0) {
            if (gecko) {
                addPattern(P + L"\\places*");   // places.sqlite = 书签+历史
            } else {
                addPattern(P + L"\\Bookmarks*");
                addPattern(P + L"\\Favicons*");
                addPattern(P + L"\\*History*");
                addPattern(P + L"\\Current *");
                addPattern(P + L"\\Last *");
                addPattern(P + L"\\Visited Links*");
            }
        } else if (_wcsicmp(cat.c_str(), L"cookies") == 0) {
            addPattern(gecko ? P + L"\\cookies*"
                             : P + L"\\Network\\Cookies*");
        } else if (_wcsicmp(cat.c_str(), L"passwords") == 0) {
            if (gecko) {
                addPattern(P + L"\\logins.json");
                addPattern(P + L"\\key*.db");
            } else {
                addPattern(P + L"\\Login Data*");
            }
        } else if (_wcsicmp(cat.c_str(), L"preferences") == 0) {
            addPattern(gecko ? P + L"\\prefs.js"
                             : P + L"\\Preferences*");
        } else if (_wcsicmp(cat.c_str(), L"profile") == 0) {
            addPattern(P + L"\\*");   // 整 profile 直接访问（=官方 *_Profile）
        } else {
            return SbieStatus::INVALID;
        }
    }
    if (patterns.empty())
        return SbieStatus::INVALID;   // 至少一个类别

    out->name = L"Local_" + Capitalize(d.key);
    out->section = L"Template_" + out->name;
    out->title = d.display + std::wstring(L" (generated by sbie-cli gen-browser)");

    out->lines.emplace_back(L"Tmpl.Title", out->title);
    out->lines.emplace_back(L"Tmpl.Class", L"WebBrowser");
    if (forceProcess && d.installed)
        out->lines.emplace_back(L"ForceProcess", d.exeName);
    for (const std::wstring& p : patterns)
        out->lines.emplace_back(L"OpenFilePath",
                                d.exeName + L"," + p);

    for (const auto& kv : out->lines)
        out->text += kv.first + L"=" + kv.second + L"\n";
    return SbieStatus::OK;
}

//---------------------------------------------------------------------------
// 安装 / 卸载
//---------------------------------------------------------------------------

SbieStatus InstallBrowserTemplate(const GeneratedTemplate& t,
                                  const std::wstring& box,
                                  const std::wstring& password)
{
    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return SbieStatus::ERR_SVC_TRANSPORT;

    // 0) 挂箱前置判定：box 存在性 + 是否已挂（幂等——Append 会叠加重复值行）
    bool needAttach = false;
    if (!box.empty()) {
        bool enabled = false, exists = false;
        if (!Ok(drv::IsBoxEnabled(box, &enabled, &exists)) || !exists)
            return SbieStatus::NOT_FOUND;
        needAttach = true;
        ConfigStore cfg;
        for (const auto& v : cfg.GetList(box, L"Template")) {
            if (_wcsicmp(v.c_str(), t.name.c_str()) == 0) {
                needAttach = false;   // 已挂载（幂等）
                break;
            }
        }
    }

    // 1) 整节替换（04 §8.15 技巧：setting 空 + value=整节文本）。refresh 只在
    //    收尾：无挂箱跟随时本调用收尾，否则挂箱写收尾
    SbieStatus st = svc.IniSetSetting(t.section, L"", t.text,
                                      svc::SvcClient::SetMode::Update,
                                      !needAttach, password);
    if (st != SbieStatus::OK)
        return st;

    // 2) 挂箱（Append Template=<名>；refresh 收尾）
    if (needAttach) {
        st = svc.IniSetSetting(box, L"Template", t.name,
                               svc::SvcClient::SetMode::Append, true,
                               password);
        if (st != SbieStatus::OK)
            return st;
    }
    return SbieStatus::OK;
}

SbieStatus RemoveBrowserTemplate(const std::wstring& templateName,
                                 const std::wstring& box,
                                 const std::wstring& password)
{
    if (!BrowserTemplateInstalled(templateName))
        return SbieStatus::NOT_FOUND;

    svc::SvcClient& svc = svc::SvcClient::Instance();
    if (!svc.Connected())
        return SbieStatus::ERR_SVC_TRANSPORT;

    // 1) box 节删 Template=<名> 值（0x1814 value=RemoveValue；box 给定且仍
    //    挂载时——已摘除则跳过，删除不存在的值行为未定义）
    if (!box.empty()) {
        bool enabled = false, exists = false;
        if (!Ok(drv::IsBoxEnabled(box, &enabled, &exists)) || !exists)
            return SbieStatus::NOT_FOUND;
        bool attached = false;
        ConfigStore cfg;
        for (const auto& v : cfg.GetList(box, L"Template")) {
            if (_wcsicmp(v.c_str(), templateName.c_str()) == 0) {
                attached = true;
                break;
            }
        }
        if (attached) {
            const SbieStatus st = svc.IniSetSetting(
                box, L"Template", templateName,
                svc::SvcClient::SetMode::Delete, false, password);
            if (st != SbieStatus::OK)
                return st;
        }
    }

    // 2) 删模板节（setting="*" = RemoveSection，rename/delete 同款）
    ConfigStore cfg;
    return cfg.Delete(L"Template_" + templateName, L"*", std::nullopt,
                      true /*refresh 收尾*/, password);
}

bool BrowserTemplateInstalled(const std::wstring& templateName)
{
    ConfigStore cfg;
    const auto v = cfg.Get(L"Template_" + templateName, L"Tmpl.Class", 0,
                           true, true);
    return v.has_value();
}

} // namespace sbie::model
