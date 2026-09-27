// Sandboxie-OSS — sbie-cli/cli/Commands/cfg_d3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 波次 D3 节全量导出（06 P2-9 用户面收口，docs/04 §19）：
//
//   * sbie-cli box dump <name>        —— 打印 box 节的原始键面
//   * sbie-cli cfg dump [<section>]   —— 无参=全部节名清单；给节名=该节键面
//
// 语义（06 P2-9 规格）：
//   * list-setting + get 的便捷化：一次列全节内全部键（含多值键的全部值
//     行——Template=/OpenFilePath= 等天然多行）；
//   * 读的是驱动缓存的**原始节**（NO_TEMPLS|NO_EXPAND|NO_GLOBAL——即
//     Sandboxie.ini 里该节自身写下的内容，不含模板节合并、变量展开与
//     GlobalSettings 回退；生效视图用既有 box get/list-setting）；
//   * 文本输出 = ini 片段形态（"[节名]" 头 + Key=Value 行），可直接重定向
//     备份或 diff；--json = {count,lines:[{key,value}…]}（cfg dump 无参
//     形态为 {sections:[{name,settings}…]}）；
//   * 纯驱动缓存读（无 SbieSvc 也能跑），无 sbie-cli server op 需求。
// 退出码：0；5=节/箱不存在；2=用法错。

#include "BoxProcCommands.h"
#include "../Output.h"
#include "../ServerConnect.h"

#include "Model/ConfigStore.h"

// vendor：CONF_GET_* 标志（原始节读 = 追加 NO_GLOBAL，见下）
#include "api_flags.h"

#include <utility>

namespace sbie::cli {

namespace {

struct SectionLine {
    std::wstring key;
    std::wstring value;
};

// 多值键的原始值列表：QueryConf 直投，flags = NO_TEMPLS|NO_EXPAND|
// NO_GLOBAL。坑：不加 NO_GLOBAL 时多值键在本节值耗尽后会并进
// GlobalSettings 的同名键值（core conf.c:1549-1556 check_global 回退——
// [TestOss] 的 Template= 列表会混入全局模板），"原始节"语义要求关掉。
std::vector<std::wstring> GetListRaw(const std::wstring& section,
                                     const std::wstring& key)
{
    std::vector<std::wstring> out;
    if (!drv::Loaded() && !drv::LoadSbieDll())
        return out;
    for (ULONG i = 0; i < 4096; ++i) {
        WCHAR buf[130] = L"";
        drv::ApiP()->SbieApi_QueryConf(
            section.c_str(), key.c_str(),
            i | CONF_GET_NO_TEMPLS | CONF_GET_NO_EXPAND | CONF_GET_NO_GLOBAL,
            buf, sizeof(buf));
        if (buf[0] == L'\0')
            break;
        out.push_back(buf);
    }
    return out;
}

// 节 → 键值行集（原始节：键序 = 驱动枚举序；值 = 本节自有，无模板/展开/
// 全局回退——与 ini 文件该节内容一致）
std::vector<SectionLine> ReadSectionLines(const std::wstring& section)
{
    std::vector<SectionLine> out;
    model::ConfigStore cfg;
    for (const std::wstring& key : cfg.ListSettings(section))
        for (const std::wstring& v : GetListRaw(section, key))
            out.push_back({ key, v });
    return out;
}

// 键值行集 → 文本输出（ini 片段）+ JSON 形态
void EmitSection(const GlobalOptions& o, const std::wstring& section,
                 const std::vector<SectionLine>& lines)
{
    if (o.json) {
        json::JsonValue arr = json::JsonValue::Array();
        for (const auto& ln : lines) {
            json::JsonValue j = json::JsonValue::Object();
            j.set(L"key", json::JsonValue(ln.key));
            j.set(L"value", json::JsonValue(ln.value));
            arr.pushBack(std::move(j));
        }
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"section", json::JsonValue(section));
        d.set(L"count", json::JsonValue((long long)lines.size()));
        d.set(L"lines", std::move(arr));
        EmitJsonOk(o, d);
        return;
    }
    util::PrintLineUtf8(util::WideToUtf8(L"[" + section + L"]"));
    for (const auto& ln : lines)
        util::PrintLineUtf8(util::WideToUtf8(ln.key + L"=" + ln.value));
}

} // namespace

// ---------------------------------------------------------------------------
// sbie-cli box dump <name>（06 P2-9）
// ---------------------------------------------------------------------------

int CmdBoxDump(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (driver cache read; no server op needed)");

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() != 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box dump <name>");
    bool enabled = false, exists = false;
    if (!Ok(drv::IsBoxEnabled(pos[0], &enabled, &exists)) || !exists)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + pos[0] + L"' not found");

    EmitSection(ctx.opts, pos[0], ReadSectionLines(pos[0]));
    return 0;
}

// ---------------------------------------------------------------------------
// sbie-cli cfg dump [<section>]（06 P2-9）
// ---------------------------------------------------------------------------

int CmdCfgDump(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);
    if (ctx.opts.showTransport)
        Diag(L"transport: direct (driver cache read; no server op needed)");

    const std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() > 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli cfg dump [<section>]");

    // 无参：全部节清单（含 box 节、GlobalSettings、用户节、Template_*）。
    // 坑：驱动节枚举（Conf_Get_Setting_Name，core conf.c:1379-1382）恒跳过
    // GlobalSettings——清单显式补该项（与 ini 实际节集一致）
    if (pos.empty()) {
        std::vector<std::wstring> sections;
        if (!Ok(drv::EnumConfSections(&sections)))
            return EmitError(ctx.opts, SbieStatus::DRIVER_UNAVAILABLE,
                             L"failed to enumerate config sections");
        bool hasGlobal = false;
        for (const auto& s : sections)
            if (_wcsicmp(s.c_str(), L"GlobalSettings") == 0)
                hasGlobal = true;
        if (!hasGlobal)
            sections.insert(sections.begin(), L"GlobalSettings");
        model::ConfigStore cfg;
        util::TablePrinter t;
        t.AddColumn(L"SECTION");
        t.AddColumn(L"SETTINGS", true);
        json::JsonValue rows = json::JsonValue::Array();
        for (const auto& s : sections) {
            const size_t n = cfg.ListSettings(s).size();
            t.AddRow({ s, std::to_wstring(n) });
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"section", json::JsonValue(s));
            r.set(L"settings", json::JsonValue((long long)n));
            rows.pushBack(std::move(r));
        }
        EmitRows(ctx.opts, t, rows, L"no config sections");
        return 0;
    }

    // 有参：该节键面（节存在性 = 节名在节清单中——空节也应可 dump；
    // GlobalSettings 恒在：驱动枚举跳过它，见上）
    if (_wcsicmp(pos[0].c_str(), L"GlobalSettings") != 0) {
        std::vector<std::wstring> sections;
        if (Ok(drv::EnumConfSections(&sections))) {
            bool found = false;
            for (const auto& s : sections)
                if (_wcsicmp(s.c_str(), pos[0].c_str()) == 0) {
                    found = true;
                    break;
                }
            if (!found)
                return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                                 L"section not found: " + pos[0]);
        }
    }
    EmitSection(ctx.opts, pos[0], ReadSectionLines(pos[0]));
    return 0;
}

} // namespace sbie::cli
