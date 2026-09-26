// Sandboxie-OSS — SbieCore/Model/Templates.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 模板枚举：直接读安装目录 Templates.ini（[Template_<名>] 节；类目 = Tmpl.Class，
// 描述 = Tmpl.Title）+ Sandboxie.ini 中的本地 [Template_*] 节（Local 模板，
// Templates.ini 头部注释声明的机制）。激活方式（04 §8.1 已核实）：box 节多值
// 设置 "Template=<名>"；Applied()/check 的"已激活对照"读驱动缓存 + 盘上 ini。
// 行为参考 QSbieAPI SbieTemplates.cpp:334-356（LGPL 仅参考）。
//
// 编码事实（实机核对，docs/04 验收记录）：Templates.ini = UTF-8+BOM；
// Sandboxie.ini = UTF-16LE+BOM——解析器两者兼容。

#include "Templates.h"
#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"
#include "../Util/Utf8.h"

#include <windows.h>

#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace sbie::model {

namespace {

//---------------------------------------------------------------------------
// 极简 ini 镜像（保序；重复 key 行 = 多值 setting）
//---------------------------------------------------------------------------

struct IniData {
    // 节名 → [(key, value) ...]（按文件行序）
    std::vector<std::pair<std::wstring,
        std::vector<std::pair<std::wstring, std::wstring>>>> secs;

    const std::vector<std::pair<std::wstring, std::wstring>>*
    Find(const wchar_t* name) const
    {
        for (const auto& s : secs)
            if (_wcsicmp(s.first.c_str(), name) == 0)
                return &s.second;
        return nullptr;
    }
};

std::wstring& TrimInPlace(std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t");
    size_t e = s.find_last_not_of(L" \t");
    if (b == std::wstring::npos) {
        s.clear();
        return s;
    }
    s = s.substr(b, e - b + 1);
    return s;
}

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out->clear();
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n)
        out->insert(out->end(), buf, buf + n);
    CloseHandle(h);
    return true;   // 短读不致命：按已读内容解析
}

// Sandboxie 家族两编码：UTF-16LE+BOM（Sandboxie.ini）/ UTF-8(+BOM)（Templates.ini）
void ParseIniBytes(const std::vector<uint8_t>& bytes, IniData* out)
{
    out->secs.clear();
    std::wstring text;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        text.assign((const wchar_t*)(bytes.data() + 2),
                    (bytes.size() - 2) / sizeof(WCHAR));
    } else {
        const char* p = (const char*)bytes.data();
        size_t n = bytes.size();
        if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
            && (unsigned char)p[2] == 0xBF) {
            p += 3;
            n -= 3;
        }
        text = util::Utf8ToWide(std::string(p, n));
    }

    // 分行（保留顺序；\r 逐行剥除）
    std::vector<std::wstring> lines;
    {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t eol = text.find(L'\n', pos);
            if (eol == std::wstring::npos) {
                lines.push_back(text.substr(pos));
                break;
            }
            lines.push_back(text.substr(pos, eol - pos));
            pos = eol + 1;
        }
    }

    size_t cur = (size_t)-1;
    for (std::wstring& line : lines) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
            line.pop_back();
        TrimInPlace(line);
        // 注释（# / ;——Templates.ini 头部用 #）
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            continue;
        if (line.front() == L'[' && line.back() == L']' && line.size() >= 2) {
            std::wstring name = line.substr(1, line.size() - 2);
            out->secs.emplace_back(TrimInPlace(name),
                                   std::vector<std::pair<std::wstring,
                                                         std::wstring>>());
            cur = out->secs.size() - 1;
        } else if (cur != (size_t)-1) {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;   // 非键值行（罕见）忽略
            std::wstring key = line.substr(0, eq);
            std::wstring val = line.substr(eq + 1);
            out->secs[cur].second.emplace_back(TrimInPlace(key),
                                               TrimInPlace(val));
        }
    }
}

bool LoadIniFile(const std::wstring& path, IniData* out)
{
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(path, &bytes))
        return false;
    ParseIniBytes(bytes, out);
    return true;
}

bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirOfFile(std::wstring path)
{
    size_t p = path.find_last_of(L'\\');
    if (p == std::wstring::npos)
        return L"";
    return path.substr(0, p);
}

// 注册表 SbieSvc 服务的 ImagePath → 目录（与 DriverApi 同款链路；此处独立
// 实现以免越界改动 DriverApi.cpp——多 agent 边界）
std::wstring SbieSvcDirFromRegistry()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\SbieSvc",
                      0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = L"";
    DWORD cb = sizeof(buf) - sizeof(WCHAR);
    LSTATUS rc = RegQueryValueExW(hk, L"ImagePath", nullptr, nullptr,
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

// 安装目录搜索链：SbieDll 实际加载目录 → 驱动 home → 注册表 SbieSvc → 本 exe 目录
std::wstring InstallDir()
{
    std::wstring lf = drv::LoadedFrom();
    if (!lf.empty()) {
        std::wstring d = DirOfFile(lf);
        if (!d.empty())
            return d;
    }
    std::wstring dos;
    if (drv::Loaded() && Ok(drv::GetHomePath(nullptr, &dos)) && !dos.empty())
        return dos;
    std::wstring reg = SbieSvcDirFromRegistry();
    if (!reg.empty())
        return reg;
    wchar_t self[MAX_PATH * 2] = L"";
    DWORD n = GetModuleFileNameW(nullptr, self, (DWORD)std::size(self));
    if (n > 0 && n < std::size(self))
        return DirOfFile(self);
    return L"";
}

bool FindTemplatesIni(std::wstring* path)
{
    std::wstring dir = InstallDir();
    if (!dir.empty()) {
        std::wstring p = dir + L"\\Templates.ini";
        if (FileExists(p)) {
            *path = p;
            return true;
        }
    }
    return false;
}

// Sandboxie.ini 权威路径：SbieSvc IniGetPath；SbieSvc 缺席时回退常见位置
// （check 命令尽力而为；写路径一律仍走 SbieSvc，不经此函数）
bool FindSandboxieIni(std::wstring* path)
{
    svc::SvcClient& svc = svc::SvcClient::Instance();
    std::wstring p;
    bool isHome = false;
    if (svc.Connected() && Ok(svc.IniGetPath(&p, &isHome))) {
        if (!p.empty() && FileExists(p)) {
            *path = p;
            return true;
        }
    }
    std::wstring dir = InstallDir();
    if (!dir.empty()) {
        std::wstring hp = dir + L"\\Sandboxie.ini";
        if (FileExists(hp)) {
            *path = hp;
            return true;
        }
    }
    if (FileExists(L"C:\\Windows\\Sandboxie.ini")) {
        *path = L"C:\\Windows\\Sandboxie.ini";
        return true;
    }
    return false;
}

// 常量初始化（非 wstring 全局）：本文件可能经 SBIE_CLI_DIRECT 通道在静态
// 初始化期被调用（cfg_read.cpp 自测分发器），std::wstring 命名空间级全局的
// 动态初始化顺序在那时未定——字面量常量无此依赖（docs/04 坑记录）。
constexpr const wchar_t kTmplPrefix[] = L"Template_";
constexpr size_t kTmplPrefixLen = std::size(kTmplPrefix) - 1;

void FillTemplateInfo(const std::wstring& sectionName,
                      const std::vector<std::pair<std::wstring, std::wstring>>& kv,
                      TemplateInfo* ti)
{
    ti->name = sectionName.size() > kTmplPrefixLen
                   ? sectionName.substr(kTmplPrefixLen) : sectionName;
    ti->clazz.clear();
    ti->descr.clear();
    for (const auto& e : kv) {
        if (ti->clazz.empty() && _wcsicmp(e.first.c_str(), L"Tmpl.Class") == 0)
            ti->clazz = e.second;
        // 描述键实为 Tmpl.Title（实机 Templates.ini 395 处；04 §4.6 初稿写
        // Tmpl.Name 有误——保留 Name 兜底以防本地模板笔误）
        if (ti->descr.empty() && _wcsicmp(e.first.c_str(), L"Tmpl.Title") == 0)
            ti->descr = e.second;
        if (ti->descr.empty() && _wcsicmp(e.first.c_str(), L"Tmpl.Name") == 0)
            ti->descr = e.second;
    }
}

// SbieSvc 0x1811-0x1814 写路径：SvcClient::IniSetSetting 便捷层（其 M1 长度
// 缺陷已于本波次修复——value_len=wcslen、h.length ≥ sizeof 结构体，QSbieAPI
// SbieAPI.cpp:1259-1288 同款约定；此前此处的 SvcIniSetLocal 过渡实现已退役，
// 见 docs/04 坑记录 §8.10）。

} // namespace

TemplateRegistry::TemplateRegistry(sbie::drv::Api* api,
                                   sbie::svc::SvcClient& svc)
    : api_(api), svc_(svc)
{
}

std::vector<TemplateInfo> TemplateRegistry::List(const std::wstring& clazzFilter)
{
    std::vector<TemplateInfo> out;

    std::wstring tplPath;
    IniData templates;
    bool haveTemplates = FindTemplatesIni(&tplPath)
                         && LoadIniFile(tplPath, &templates);

    // 本地模板（Sandboxie.ini 的 [Template_*]，含 Template_Local_*）
    std::wstring iniPath;
    IniData localIni;
    bool haveLocal = FindSandboxieIni(&iniPath)
                     && LoadIniFile(iniPath, &localIni);

    auto emitSections = [&](const IniData& data) {
        for (const auto& s : data.secs) {
            if (_wcsnicmp(s.first.c_str(), kTmplPrefix, kTmplPrefixLen) != 0)
                continue;
            TemplateInfo ti;
            FillTemplateInfo(s.first, s.second, &ti);
            if (ti.name.empty())
                continue;
            if (clazzFilter != L"*"
                && _wcsicmp(ti.clazz.c_str(), clazzFilter.c_str()) != 0)
                continue;
            // 官方目录在前，本地同名模板不重复列出
            bool dup = false;
            for (const auto& e : out)
                if (_wcsicmp(e.name.c_str(), ti.name.c_str()) == 0)
                    dup = true;
            if (!dup)
                out.push_back(std::move(ti));
        }
    };

    if (haveTemplates)
        emitSections(templates);
    if (haveLocal)
        emitSections(localIni);
    return out;
}

SbieStatus TemplateRegistry::Info(
    const std::wstring& name,
    std::vector<std::pair<std::wstring, std::wstring>>* settings)
{
    settings->clear();
    if (name.empty())
        return SbieStatus::NOT_FOUND;
    const std::wstring section = std::wstring(kTmplPrefix) + name;

    // 本地模板优先（同名遮蔽官方条目——与 Sandboxie 查找次序一致）
    std::wstring iniPath;
    IniData localIni;
    if (FindSandboxieIni(&iniPath)
        && LoadIniFile(iniPath, &localIni)) {
        if (const auto* kv = localIni.Find(section.c_str())) {
            *settings = *kv;
            return SbieStatus::OK;
        }
    }

    std::wstring tplPath;
    IniData templates;
    if (FindTemplatesIni(&tplPath) && LoadIniFile(tplPath, &templates)) {
        if (const auto* kv = templates.Find(section.c_str())) {
            *settings = *kv;
            return SbieStatus::OK;
        }
    }
    return SbieStatus::NOT_FOUND;
}

SbieStatus TemplateRegistry::Apply(const std::wstring& box,
                                   const std::wstring& tmpl)
{
    // box 节 Append Template=<名>（04 §8.1 激活方式）；经修复后的
    // SvcClient::IniSetSetting 便捷层
    return svc_.IniSetSetting(box, L"Template", tmpl,
                              svc::SvcClient::SetMode::Append, true, L"");
}

SbieStatus TemplateRegistry::Revoke(const std::wstring& box,
                                    const std::wstring& tmpl)
{
    // 0x1814 携带 value = RemoveValue（sbieiniserver.cpp:1079-1091）——删该值
    // 本身，无需 List→重放
    return svc_.IniSetSetting(box, L"Template", tmpl,
                              svc::SvcClient::SetMode::Delete, true, L"");
}

std::vector<std::wstring> TemplateRegistry::Applied(const std::wstring& box)
{
    std::vector<std::wstring> out;
    drv::QueryConfList(box, L"Template", true, true, &out);
    return out;
}

} // namespace sbie::model
