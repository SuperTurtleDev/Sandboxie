// Sandboxie-OSS — SbieCore/Model/TemplateGen.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 浏览器兼容模板生成器（07-P2-5，波次 D3；实现与语义决策记录 docs/04 §19）。
// Model 层 additive 新面（不动既有冻结契约）。
//
// 行为基准（01-license-map 允许范围）：
//   * 规格语义 = docs/07 §3.3 07-P2-5 记录的 BrowserTemplateWizard 行为
//     （选浏览器 → 自动识别引擎 Gecko/Chromium 与 profile 目录 → 勾选
//     cookies/密码/书签/偏好等直接访问项 → 生成 Local_ 模板节并可挂到箱）；
//     SandMan 源码未读未复制；
//   * 键面形态 = GPL core install/Templates.ini 的浏览器模板族
//     （[Template_Edge_*] 等：ForceProcess=<exe>、OpenFilePath=<exe>,<路径>、
//     Tmpl.Class=WebBrowser；本文件按键面形态自研，探测逻辑为注册表
//     App Paths + Program Files 探测 + 用户数据目录探测，全部自研）。
//
// 写入机制：模板节 = Sandboxie.ini 的 [Template_Local_<Name>] 节，经 SbieSvc
// "setting 空 + value=整节文本" 整节替换（04 §8.15 协议技巧，rename 同款）；
// --install 挂箱 = box 节 Append Template=Local_<Name>（04 §8.1 激活方式）；
// --remove = box 节删该 Template 值 + setting="*" 删模板节。全部 SbieSvc
// 直连（键面写命令，无 sbie-cli server op 需求）。

#pragma once

#include "../Util/Status.h"

#include <string>
#include <utility>
#include <vector>

namespace sbie::model {

// ---------------------------------------------------------------------------
// 探测结果
// ---------------------------------------------------------------------------

struct BrowserDetect {
    std::wstring key;        // "edge" / "chrome" / "firefox" / ...（CLI 名）
    std::wstring display;    // "Microsoft Edge"
    std::wstring engine;     // "chromium" | "gecko"
    std::wstring exeName;    // "msedge.exe"（ForceProcess 用）
    std::wstring exePath;    // 探测到的安装路径（空 = 未安装）
    std::wstring source;     // 探测来源："app-paths" | "program-files" | ""
    std::wstring profileDir; // 探测到的 profile 目录（%var% 变量化形态；
                             // 空 = 未找到用户数据）
    bool installed = false;  // exePath 非空且存在
    bool profileFound = false;
};

// 探测全部受支持浏览器（Chrome/Edge/Firefox/Chromium/Brave/Vivaldi/Opera）。
// 只读（注册表 + 文件系统存在性探测）；无驱动/SbieSvc 也能跑。
std::vector<BrowserDetect> DetectBrowsers();

// ---------------------------------------------------------------------------
// 模板生成
// ---------------------------------------------------------------------------

// 直接访问类别（BrowserTemplateWizard 勾选项的 CLI 形态；默认集 =
// bookmarks,cookies,passwords,preferences——07 记录的向导典型勾选）。
// 类别 → OpenFilePath 行的映射按引擎分档（GPL Templates.ini 键面形态）。
struct GeneratedTemplate {
    std::wstring name;       // "Local_Edge"（Template= 激活名）
    std::wstring section;    // "Template_Local_Edge"
    std::wstring title;      // Tmpl.Title 值
    std::vector<std::pair<std::wstring, std::wstring>> lines;  // 键值（有序）
    std::wstring text;       // 渲染后的整节文本（"Key=Value\n" 行集）
};

// 生成一个浏览器的 Local_ 模板节。
//   accessCategories：直接访问类别（小写；未知类别返回 INVALID）；
//   forceProcess：true = 含 ForceProcess=<exe> 行（缺省建议 true，任务书
//   规格"强制进程列表"；调用方给 --no-force 逃生）。
SbieStatus GenerateBrowserTemplate(const BrowserDetect& d,
                                   const std::vector<std::wstring>& accessCategories,
                                   bool forceProcess,
                                   GeneratedTemplate* out);

// ---------------------------------------------------------------------------
// 安装 / 卸载（SbieSvc 直连；refresh 收尾）
// ---------------------------------------------------------------------------

// 写模板节（整节替换，幂等重生成）+ 可选拖到箱（Append Template=<名>，
// box 非空且存在时）。密码用于锁配置（EditPassword）。
SbieStatus InstallBrowserTemplate(const GeneratedTemplate& t,
                                  const std::wstring& box /*可空*/,
                                  const std::wstring& password);

// 卸载：删 box 节的 Template=<名> 值（box 非空时）+ 删模板节（removeSection）。
// 未安装（节不存在）→ NOT_FOUND。
SbieStatus RemoveBrowserTemplate(const std::wstring& templateName,
                                 const std::wstring& box /*可空*/,
                                 const std::wstring& password);

// 模板节是否存在（读驱动缓存）。
bool BrowserTemplateInstalled(const std::wstring& templateName);

} // namespace sbie::model
