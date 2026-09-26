// Sandboxie-OSS — SbieCore/Model/Boxes.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 沙箱领域逻辑（契约：04-modules.md §2.4；签名冻结）。
// M1 实现状态：EnumSections / EnumBoxes / GetInfo / ValidateName 已实现
// （纯驱动读路径，可降级直连）；写路径（Create/Rename/Delete/SetEnabled/
// TerminateAll）为占位桩（需 SbieSvc，server 波次接通）。
//
// 行为参考（01-license-map §2 允许范围）：QSbieAPI SbieAPI.cpp ReloadBoxes
// （:1171-1200 节枚举 + IsBox 过滤）、CreateBox（:1459-1477 仅写 Enabled=y，
// 不建目录——04 §8.4）、ValidateName（:1412-1445）。

#pragma once

#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

struct BoxInfo {
    std::wstring name;
    bool enabled = false;
    bool exists = false;
    std::wstring fileRoot, regRoot, ipcRoot;   // QueryBoxPath 两段式
    bool hasProcesses = false;
};

class BoxRepository {
public:
    explicit BoxRepository(sbie::drv::Api* api, sbie::svc::SvcClient& svc);

    std::vector<std::wstring> EnumSections();  // 含 Template_*/UserSettings_*（枚举节）
    std::vector<BoxInfo> EnumBoxes(bool enabledOnly);  // EnumBoxesEx + IsBoxEnabled
    SbieStatus GetInfo(const std::wstring& name, BoxInfo* out);
    SbieStatus Create(const std::wstring& name);   // ValidateName + SET "Enabled"="y"
    SbieStatus Rename(const std::wstring& oldN, const std::wstring& newN); // 复制节+删旧节（refresh 后）
    SbieStatus Delete(const std::wstring& name, bool delFiles, bool delSection);
    SbieStatus SetEnabled(const std::wstring& name, bool on);
    SbieStatus TerminateAll(const std::wstring& name);
    // 名称校验（对齐 ValidateName，SbieAPI.cpp:1412-1445）：<=38 WCHAR；仅
    // [A-Za-z0-9_]；非 aux/con/nul/prn/com0-9/lpt0-9/clock$；非 GlobalSettings /
    // UserSettings_ 前缀
    static SbieStatus ValidateName(const std::wstring& name);

private:
    sbie::drv::Api* api_;      // 供后续直连 IOCTL 扩展用（当前薄封装自持状态）
    sbie::svc::SvcClient& svc_;
};

} // namespace sbie::model
