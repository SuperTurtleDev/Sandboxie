// Sandboxie-OSS — SbieCore/Model/ConfigStore.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 配置存取（契约：04-modules.md §2.4；签名冻结）。
// M1 实现状态：Get/GetList/ListSettings/Locked 已实现（驱动缓存读，
// 无 SbieSvc 也能读）；Set*/Reload/Path 为占位桩（写路径经 SvcClient）。
//
// 注（契约具体化）：04 文档将 Set/SetAppend/SetInsert/Delete 写为 "…"
// 省略形参；本头按 §2.2 IniSetSetting 语义固定为
//   (section, setting, value, refresh, password)；
// Delete 增加 index（0x1814 value 空=整 setting 的反向控制）。

#pragma once

#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"

#include <optional>
#include <string>
#include <vector>

namespace sbie::model {

class ConfigStore {
public:
    // 读：SbieApi_QueryConf（驱动缓存，无 SbieSvc 也能读）
    std::optional<std::wstring> Get(const std::wstring& section,
                                    const std::wstring& setting,
                                    ULONG index = 0, bool noExpand = true,
                                    bool noTemplates = true);
    std::vector<std::wstring> GetList(const std::wstring& section,
                                      const std::wstring& setting,
                                      bool noExpand = true,
                                      bool noTemplates = true);  // index 递增到失败
    std::vector<std::wstring> ListSettings(const std::wstring& section); // 枚举 setting 名
    SbieStatus Set(const std::wstring& section, const std::wstring& setting,
                   const std::wstring& value, bool refresh = true,
                   const std::wstring& password = L"");
    SbieStatus SetAppend(const std::wstring& section, const std::wstring& setting,
                         const std::wstring& value, bool refresh = true,
                         const std::wstring& password = L"");
    SbieStatus SetInsert(const std::wstring& section, const std::wstring& setting,
                         const std::wstring& value, bool refresh = true,
                         const std::wstring& password = L"");
    SbieStatus Delete(const std::wstring& section, const std::wstring& setting,
                      const std::optional<ULONG>& index = std::nullopt,
                      bool refresh = true, const std::wstring& password = L"");
    SbieStatus Reload(bool reconfigureDrv);           // SbieApi_ReloadConf(-1, flag)
    SbieStatus Path(std::wstring* out, bool* isHome);
    bool Locked();                                    // GlobalSettings/EditPassword 非空
};

} // namespace sbie::model
