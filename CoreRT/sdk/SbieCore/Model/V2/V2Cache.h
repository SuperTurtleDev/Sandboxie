// Sandboxie-OSS — SbieCore/Model/V2/V2Cache.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 展开缓存（docs/10-v2-design.md §5.4）：
//   * 缓存 = boxes\<box>.ini，单节 [<box>]（节名==文件基名，conf.c:1037-1046）
//   * 原子落位：boxes\<box>.ini.tmp → MoveFileEx(REPLACE_EXISTING)；
//     驱动通配扫描只认 .ini（conf.c:1008），中转文件永不可见 ⇒ 无半写导入
//   * 写后自检（硬性）：恰一节、零 Template= 行、FileRootPath==期望盒目录、
//     节名==文件基名==盒名。任一失败即报 CACHE 错误且不动原缓存。
//   * 溯源注释行（'#'）记录来源 sandbox.ini 与展开的模板引用。

#pragma once

#include "V2Common.h"

#include <vector>

namespace sbie::model::v2 {

// 生成缓存文本（不落盘；--verbose 诊断与自检复用）
std::string BuildCacheText(const std::wstring& box, const std::wstring& boxDir,
                           const std::wstring& sandboxIniPath,
                           const std::vector<IniKeyValue>& kv,
                           const std::vector<std::wstring>& appliedTemplates);

// 写缓存（tmp + 原子替换）+ 写后自检。成功后缓存即为注册工件。
// expectedRoot = 该盒声明的 FileRootPath（普通盒 = boxDir；加密盒 =
// <boxDir>\data——UseFileImage=y 时 FileRootPath 指向 junction 目标）。
V2Err WriteBoxCache(const std::wstring& box, const std::wstring& boxDir,
                    const std::wstring& sandboxIniPath,
                    const std::vector<IniKeyValue>& kv,
                    const std::vector<std::wstring>& appliedTemplates,
                    const std::wstring& expectedRoot = std::wstring());

// 自检（读回解析；也用于校验外部生成/手改的缓存）。expectedRoot 语义同上，
// 缺省 = boxDir（向后兼容）。
V2Err ValidateCacheFile(const std::wstring& box, const std::wstring& boxDir,
                        const std::wstring& expectedRoot = std::wstring());

bool CacheExists(const std::wstring& box);
V2Err DeleteBoxCache(const std::wstring& box);   // 不存在 = 成功（幂等）

} // namespace sbie::model::v2
