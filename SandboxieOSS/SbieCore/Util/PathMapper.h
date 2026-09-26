// Sandboxie-OSS — SbieCore/Util/PathMapper.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// NT 路径 ↔ DOS 路径（04-modules.md §1 Util/PathMapper）。
//   NtToDosPath：优先 SbieDll_TranslateNtToDosPath（驱动符号链接解析，行为对齐
//   QSbieAPI Nt2DosPath 的用途；LGPL 仅参考），SbieDll 未加载时退回
//   QueryDosDeviceW 逐盘符匹配 + `\??\` 前缀剥离。
//   DosToNtPath：QueryDosDeviceW 反查。

#pragma once

#include <string>

namespace sbie::util {

// 就地转换失败时原样保留（调用方无需判错；显示用途）。
void NtToDosPath(std::wstring* path);
void DosToNtPath(std::wstring* path);

} // namespace sbie::util
