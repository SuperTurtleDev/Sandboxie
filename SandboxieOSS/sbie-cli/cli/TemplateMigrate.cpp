// Sandboxie-OSS — sbie-cli/cli/TemplateMigrate.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V1 → V2 模板迁移工具（--migrate-templates；docs/10-v2-design.md §11）：
//   * 输入：官方 install\Templates.ini（GPLv3 资产）
//   * 输出：V2 模板树 <outDir>\<类别>\<名字>.ini（[Template] 单节）
//   * 规则：
//     - 有 Tmpl.Class 的模板（390 个）→ 类别目录原样迁（键面不动）
//     - [DefaultTemplates] 引用的 10 个无类默认模板 → System\
//     - [TemplateSettings] → 不迁（变量已固化进引擎内置表，V2Template.cpp）
//     - 其余无类节（~34 个远古死壳：Neon/Maxthus2/DefenseWall/…）→ 丢弃
//     - 额外生成 BoxTypes\Standard.ini（嵌套引用 System\ 十默认 + Enabled 基线）
//   * 内容键原样复制（驱动/SbieDll/SbieSvc 仍消费这些键）；Tmpl.* 元数据保留。

#include "Commands.h"
#include "Output.h"
#include "../../SbieCore/Model/V2/V2Common.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace sbie::cli {

using namespace sbie::model::v2;

namespace {

// V1 [DefaultTemplates] 清单（Templates.ini:31-41；迁 System\ 的无类默认集）
const wchar_t* kDefaultSet[] = {
    L"RpcPortBindings",   L"SpecialImages", L"COM",       L"WindowsExplorer",
    L"ThirdPartyIsolation", L"BlockSoftwareUpdaters",     L"BlockWinRM",
    L"OpenWinInetCache",  L"CredentialUIBroker",          L"MSI_Lite",
};

bool WriteUtf8File(const std::wstring& path, const std::string& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD got = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &got, nullptr)
              && got == text.size();
    CloseHandle(h);
    return ok;
}

} // namespace

int CmdMigrateTemplates(const std::wstring& src, const std::wstring& outDir)
{
    IniFileData ini;
    V2Err e = ParseIniFile(src, &ini);
    if (!e.Ok()) {
        util::PrintErrLineUtf8(util::WideToUtf8(L"cannot parse " + src + L": " + e.msg));
        return 1;
    }

    // [DefaultTemplates] 的 Template= 引用集
    std::set<std::wstring> defaultSet;
    for (const wchar_t* n : kDefaultSet)
        defaultSet.insert(n);
    // 默认集成员 → 实际类别（迁移中回填；Standard.ini 引用用）
    std::map<std::wstring, std::wstring> defaultCat;

    int migrated = 0, skipped = 0;
    std::wstring curCat;

    for (const auto& sec : ini.sections) {
        if (sec.name.empty())
            continue;
        if (_wcsicmp(sec.name.c_str(), L"DefaultTemplates") == 0
            || _wcsicmp(sec.name.c_str(), L"TemplateSettings") == 0)
            continue;   // 结构节（后者变量已内置）
        if (_wcsnicmp(sec.name.c_str(), L"Template_", 9) != 0)
            continue;   // 非模板节（如沙盒节混入）

        std::wstring name = sec.name.substr(9);   // 剥 "Template_"
        if (name.empty())
            continue;

        // 类别：Tmpl.Class；无类但属默认集 → System；否则丢弃（死壳/示例）
        std::wstring cat;
        bool inDefault = defaultSet.count(name) > 0;
        for (const auto& kv : sec.entries) {
            if (_wcsicmp(kv.key.c_str(), L"Tmpl.Class") == 0) {
                cat = kv.value;
                break;
            }
        }
        if (_wcsicmp(cat.c_str(), L"Local") == 0) {
            ++skipped;   // 文档示例桩（Local_ExampleSoft）
            continue;
        }
        if (cat.empty() && inDefault)
            cat = L"System";
        if (cat.empty()) {
            ++skipped;
            continue;
        }
        // 类别目录名：空格/特殊字符折叠（V1 类值如 "WebBrowser" 本就干净）
        for (auto& c : cat)
            if (c == L' ' || c == L'/' || c == L'\\')
                c = L'_';

        // 名字合法性：模板名仅用户态解析（不进驱动），允许 [A-Za-z0-9_.-]
        bool nameOk = !name.empty() && name.size() <= 60
                      && name.front() != L'.' && name.back() != L'.';
        for (wchar_t c : name) {
            bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
                      || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-'
                      || c == L'.';
            if (!ok)
                nameOk = false;
        }
        if (!nameOk) {
            ++skipped;
            continue;
        }

        // 默认集成员的实际类别（供 Standard.ini 引用）
        if (inDefault)
            defaultCat[name] = cat;

        std::wstring dir = outDir + L"\\" + cat;
        std::wstring path = dir + L"\\" + name + L".ini";
        CreateDirectoryW(dir.c_str(), nullptr);

        std::wstring t = L"[Template]\n";
        for (const auto& kv : sec.entries) {
            // Tmpl.Class 重写为目录名（迁移校验项：类目=目录）
            if (_wcsicmp(kv.key.c_str(), L"Tmpl.Class") == 0)
                t += L"Tmpl.Class=" + cat + L"\n";
            else
                t += kv.key + L"=" + kv.value + L"\n";
        }
        if (!WriteUtf8File(path, util::WideToUtf8(t))) {
            util::PrintErrLineUtf8(util::WideToUtf8(L"cannot write " + path));
            return 1;
        }
        ++migrated;
    }

    // BoxTypes\Standard.ini：嵌套引用默认集 + Enabled 基线
    {
        std::wstring dir = outDir + L"\\BoxTypes";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring t = L"[Template]\n"
                         L"Tmpl.Title=Standard Box\n"
                         L"Tmpl.Class=BoxTypes\n";
        for (const wchar_t* n : kDefaultSet) {
            auto it = defaultCat.find(n);
            std::wstring cat = it != defaultCat.end() ? it->second : L"System";
            t += std::wstring(L"Template=") + cat + L"\\" + n + L"\n";
        }
        t += L"Enabled=y\n";
        if (!WriteUtf8File(dir + L"\\Standard.ini", util::WideToUtf8(t))) {
            util::PrintErrLineUtf8("cannot write BoxTypes\\Standard.ini");
            return 1;
        }
        ++migrated;
    }

    util::PrintLineUtf8("migrated " + std::to_string(migrated)
                        + " templates, skipped " + std::to_string(skipped)
                        + " (legacy/dead) -> " + util::WideToUtf8(outDir));
    return 0;
}

} // namespace sbie::cli
