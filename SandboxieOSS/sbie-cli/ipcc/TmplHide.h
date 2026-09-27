// Sandboxie-OSS — sbie-cli/ipcc/TmplHide.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// Tmpl.Hide 过滤助手（08-P2-7，波次 E）：Templates.ini 的 [Template_*] 节可带
// Tmpl.Hide=y（Plus UI 约定 = 隐藏弃用模板；core 无读者，08 轴3 登记为呈现层
// 键）。`template list` 默认滤之、`--all` 显示——与 Plus 呈现对齐。
//
// 放在 ipcc/（client 命令与 server Dispatcher 共用；单 exe glob 编译）。
// 边界注：Model\Templates 不在本波允许面（仅 Processes/Snapshots additive），
// 故独立实现定位与解析（与 Model Templates.cpp 的 FindTemplatesIni 搜索链
// 同序：SbieDll 实际加载目录 → 驱动 home → 注册表 SbieSvc ImagePath → 本 exe
// 目录；编码两态 UTF-16LE+BOM / UTF-8(+BOM) 同样兼容）。

#pragma once

#include <string>
#include <vector>

namespace sbie::tmplhide {

// 收集带 Tmpl.Hide=y 的模板名（小写归一，比较用 Contains）。
// sandboxieIniPath 非空 = 只扫该文件的本地 [Template_*] 节；空 = 自行定位
// Sandboxie.ini（SbieSvc IniGetPath → 安装目录 → C:\Windows）。
// 线程亲和：自行定位会触 SbieSvc LPC——仅 client 进程语境安全；server
// worker 线程必须先经 SvcCall 取得路径后作参数传入（docs/04 §22.4 坑记录）。
std::vector<std::wstring> HiddenNames(const std::wstring& sandboxieIniPath = L"");

// 大小写不敏感的集合命中（模板名比较惯例 = _wcsicmp）。
bool Contains(const std::vector<std::wstring>& hidden,
              const std::wstring& name);

} // namespace sbie::tmplhide
