// Sandboxie-OSS — SbieCore/Util/Version.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie-cli 自身版本（`sbie version` 首行）与冻结基线常量。

#pragma once

namespace sbie {

constexpr const char*    kCliVersion     = "2.0.0";     // V2：无 server/模板引擎/ImportBox 注册
constexpr const wchar_t* kCliVersionW    = L"2.0.0";
// 冻结基线 5.73.5（Sandboxie\common\my_version.h）：
constexpr unsigned long  kExpectedAbi    = 0x57230;     // MY_ABI_VERSION
constexpr const wchar_t* kExpectedDriver = L"5.73.5";   // MY_VERSION_STRING

} // namespace sbie
