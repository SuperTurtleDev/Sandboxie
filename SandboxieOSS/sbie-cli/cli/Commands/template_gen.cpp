// Sandboxie-OSS — sbie-cli/cli/Commands/template_gen.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie template gen-browser（07-P2-5，波次 D3；docs/04 §19）：
//
//   gen-browser [--browser <name>] [--access <a,b,...>] [--no-force]
//               [--box <NAME> --install | --remove]
//
//   * 缺省 --dry-run：探测本机浏览器（注册表 App Paths + 安装目录 +
//     用户数据目录），对每个已安装的浏览器生成 [Template_Local_<Name>]
//     节文本并打印（不写盘）；
//   * --install：模板节写入 Sandboxie.ini（SbieSvc 整节替换）；给 --box
//     时追加 Template=Local_<Name> 到该 box；
//   * --remove：卸载（要求显式 --browser <name>；给 --box 时先摘除该 box
//     的 Template= 值再删模板节）；
//   * --access：直接访问类别（BrowserTemplateWizard 勾选项的 CLI 形态）：
//     bookmarks|history|cookies|passwords|preferences|profile；
//     缺省 bookmarks,cookies,passwords,preferences；
//   * --no-force：不写 ForceProcess 行（缺省写——强制浏览器进沙箱）。
//
// 纯 client 直连（探测 = 注册表/文件只读；写入 = SbieSvc 键面写），无
// sbie-cli server op 需求。退出码：0；7=名/类别非法；5=浏览器未装/box 或
// 模板不存在；6=锁配置；4=SbieSvc 不可用。

#include "BoxProcCommands.h"
#include "CfgTemplateCommands.h"
#include "../Output.h"
#include "../ServerConnect.h"

#include "Model/TemplateGen.h"

#include <cwchar>
#include <cwctype>
#include <utility>

namespace sbie::cli {

namespace {

// --access 值拆分（逗号分隔；空段忽略）
std::vector<std::wstring> SplitAccess(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

void NoteDirect(const GlobalOptions& o)
{
    if (o.showTransport)
        Diag(L"transport: direct (sbie-cli server op not needed; "
             L"SbieSvc / local detection)");
}

// 探测行 → 表格/JSON（dry-run 与安装结果共用）
void EmitDetection(const GlobalOptions& o,
                   const std::vector<model::BrowserDetect>& all,
                   const std::vector<model::GeneratedTemplate>& gens,
                   const wchar_t* action, const wchar_t* box)
{
    if (o.json) {
        json::JsonValue data = json::JsonValue::Object();
        json::JsonValue rows = json::JsonValue::Array();
        for (const auto& d : all) {
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"browser", json::JsonValue(d.key));
            r.set(L"display", json::JsonValue(d.display));
            r.set(L"engine", json::JsonValue(d.engine));
            r.set(L"installed", json::JsonValue(d.installed));
            r.set(L"exe_path", json::JsonValue(d.exePath));
            r.set(L"source", json::JsonValue(d.source));
            r.set(L"profile", json::JsonValue(d.profileDir));
            r.set(L"profile_found", json::JsonValue(d.profileFound));
            rows.pushBack(std::move(r));
        }
        data.set(L"browsers", std::move(rows));
        json::JsonValue tpls = json::JsonValue::Array();
        for (const auto& g : gens) {
            json::JsonValue t = json::JsonValue::Object();
            t.set(L"name", json::JsonValue(g.name));
            t.set(L"section", json::JsonValue(g.section));
            t.set(L"title", json::JsonValue(g.title));
            json::JsonValue lines = json::JsonValue::Array();
            for (const auto& kv : g.lines) {
                json::JsonValue ln = json::JsonValue::Object();
                ln.set(L"key", json::JsonValue(kv.first));
                ln.set(L"value", json::JsonValue(kv.second));
                lines.pushBack(std::move(ln));
            }
            t.set(L"lines", std::move(lines));
            tpls.pushBack(std::move(t));
        }
        data.set(L"templates", std::move(tpls));
        data.set(L"action", json::JsonValue(action));
        if (box && *box)
            data.set(L"box", json::JsonValue(box));
        EmitJsonOk(o, data);
        return;
    }

    util::TablePrinter t;
    t.AddColumn(L"BROWSER");
    t.AddColumn(L"ENGINE");
    t.AddColumn(L"INSTALLED");
    t.AddColumn(L"EXE");
    t.AddColumn(L"PROFILE");
    for (const auto& d : all) {
        t.AddRow({ d.key, d.engine, d.installed ? L"yes" : L"no",
                   d.installed ? d.exePath : L"-",
                   d.profileFound ? d.profileDir : L"-" });
    }
    EmitRows(o, t, json::JsonValue::Array(), L"no browsers detected");

    for (const auto& g : gens) {
        util::PrintLineUtf8(util::WideToUtf8(
            L"[" + g.section + L"]"));
        for (const auto& kv : g.lines)
            util::PrintLineUtf8(util::WideToUtf8(
                kv.first + L"=" + kv.second));
        util::PrintLineUtf8("");
    }
}

} // namespace

int CmdTemplateGenBrowser(const CommandContext& ctx)
{
    // 尾置全局旗标吸收（--json/--quiet 等出现在组名之后的兼容形态）
    GlobalOptions o;
    std::vector<std::wstring> rest =
        cfgtmpl::AbsorbTrailingGlobals(ctx, &o);
    NoteDirect(o);

    // 参数解析（rest = 组/子名之后的命令专属序列）
    std::wstring browser, access, box;
    bool install = false, remove = false, noForce = false;
    for (size_t i = 0; i < rest.size(); ++i) {
        const std::wstring& a = rest[i];
        if (a == L"--browser" && i + 1 < rest.size())
            browser = rest[++i];
        else if (a == L"--access" && i + 1 < rest.size())
            access = rest[++i];
        else if (a == L"--box" && i + 1 < rest.size())
            box = rest[++i];
        else if (a == L"--install")
            install = true;
        else if (a == L"--remove")
            remove = true;
        else if (a == L"--no-force")
            noForce = true;
        else if (a == L"--force")
            ;   // 缺省即 force；接受显式 --force
        else if (!a.empty() && a[0] == L'-')
            return EmitError(o, SbieStatus::USAGE,
                             L"unknown option: " + a);
        else
            return EmitError(o, SbieStatus::USAGE,
                             L"unexpected argument: " + a);
    }
    if (install && remove)
        return EmitError(o, SbieStatus::USAGE,
                         L"--install and --remove are mutually exclusive");
    if (remove && browser.empty())
        return EmitError(o, SbieStatus::USAGE,
                         L"--remove requires an explicit --browser <name>");
    if (!box.empty() && !install && !remove)
        return EmitError(o, SbieStatus::USAGE,
                         L"--box requires --install or --remove "
                         L"(dry-run prints what would be written)");

    std::vector<std::wstring> cats =
        { L"bookmarks", L"cookies", L"passwords", L"preferences" };
    if (!access.empty()) {
        cats = SplitAccess(access);
        if (cats.empty())
            return EmitError(o, SbieStatus::USAGE,
                             L"--access needs at least one category "
                             L"(bookmarks|history|cookies|passwords|"
                             L"preferences|profile)");
    }

    // 探测（只读；驱动/SbieSvc 缺席也可 dry-run）
    std::vector<model::BrowserDetect> all = model::DetectBrowsers();

    // 目标集：--browser 单个 / 全部已安装
    std::vector<model::BrowserDetect> targets;
    if (!browser.empty()) {
        bool hit = false;
        for (const auto& d : all) {
            if (_wcsicmp(d.key.c_str(), browser.c_str()) == 0) {
                if (!d.installed)
                    return EmitError(o, SbieStatus::NOT_FOUND,
                                     L"browser not installed: " + browser);
                targets.push_back(d);
                hit = true;
                break;
            }
        }
        if (!hit)
            return EmitError(o, SbieStatus::INVALID,
                             L"unknown browser '" + browser
                             + L"' (supported: chrome, edge, firefox, "
                               L"chromium, brave, vivaldi, opera)");
    } else {
        for (const auto& d : all)
            if (d.installed)
                targets.push_back(d);
        if (targets.empty() && !remove)
            return EmitError(o, SbieStatus::NOT_FOUND,
                             L"no supported browser installed");
    }

    const std::wstring pw = boxproc::ResolvePasswordArgs(o, rest);

    // ---- --remove：卸载（--browser 必给） ------------------------------
    if (remove) {
        std::wstring name = targets[0].key;
        name[0] = (wchar_t)towupper(name[0]);
        name = L"Local_" + name;
        if (!model::BrowserTemplateInstalled(name))
            return EmitError(o, SbieStatus::NOT_FOUND,
                             L"template not installed: " + name);
        SbieStatus st = model::RemoveBrowserTemplate(name, box, pw);
        if (st == SbieStatus::NOT_FOUND)
            return EmitError(o, st,
                             L"box not found: " + box);
        if (st != SbieStatus::OK)
            return EmitError(o, st,
                             L"remove failed (locked config? use "
                               L"--password)"
                                 + std::wstring(boxproc::PasswordHint(st,
                                                                      pw)));
        if (o.json) {
            json::JsonValue d = json::JsonValue::Object();
            d.set(L"template", json::JsonValue(name));
            if (!box.empty())
                d.set(L"box", json::JsonValue(box));
            d.set(L"message",
                  json::JsonValue(L"template '" + name + L"' removed"
                                  + (box.empty() ? L"" : L" (detached from "
                                                    L"box '" + box + L"')")));
            EmitJsonOk(o, d);
        } else {
            EmitMessage(o,
                        L"template '" + name + L"' removed"
                            + (box.empty() ? L""
                                           : L" (detached from box '" + box
                                                 + L"')"));
        }
        return 0;
    }

    // ---- 生成（dry-run / --install） ------------------------------------
    std::vector<model::GeneratedTemplate> gens;
    for (const auto& d : targets) {
        model::GeneratedTemplate g;
        SbieStatus st = model::GenerateBrowserTemplate(d, cats, !noForce, &g);
        if (st == SbieStatus::INVALID)
            return EmitError(o, st,
                             L"invalid access category in --access "
                             L"(bookmarks|history|cookies|passwords|"
                             L"preferences|profile)");
        if (st != SbieStatus::OK)
            return EmitError(o, st,
                             L"cannot generate template for '" + d.key
                             + L"' (not installed)");
        gens.push_back(std::move(g));
    }

    if (!install) {
        EmitDetection(o, all, gens, L"dry-run",
                      box.empty() ? L"" : box.c_str());
        if (!box.empty())
            Diag(L"dry-run: --box given without --install; nothing written");
        return 0;
    }

    // ---- --install -------------------------------------------------------
    for (const auto& g : gens) {
        SbieStatus st = model::InstallBrowserTemplate(g, box, pw);
        if (st == SbieStatus::NOT_FOUND)
            return EmitError(o, st,
                             box.empty() ? L"template write failed"
                                         : L"box not found: " + box);
        if (st != SbieStatus::OK)
            return EmitError(o, st,
                             L"install failed for '" + g.name + L"'"
                                 L" (locked config? use --password)"
                                 + boxproc::PasswordHint(st, pw));
    }

    if (o.json) {
        json::JsonValue d = json::JsonValue::Object();
        json::JsonValue names = json::JsonValue::Array();
        for (const auto& g : gens)
            names.pushBack(json::JsonValue(g.name));
        d.set(L"templates", std::move(names));
        if (!box.empty())
            d.set(L"box", json::JsonValue(box));
        d.set(L"message",
              json::JsonValue(std::to_wstring(gens.size())
                              + L" template(s) installed"
                              + (box.empty() ? L""
                                             : L" and attached to box '"
                                                   + box + L"'")));
        EmitJsonOk(o, d);
    } else {
        EmitMessage(o,
                    std::to_wstring(gens.size()) + L" template(s) installed"
                        + (box.empty() ? L""
                                       : L" and attached to box '" + box
                                             + L"'"));
    }
    return 0;
}

} // namespace sbie::cli
