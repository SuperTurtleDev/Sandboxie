// Sandboxie-OSS — SbieCore/Model/Processes.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 沙箱内进程领域逻辑（契约：04-modules.md §2.4；签名冻结）。
// M1 实现状态：Enum 已实现（EnumProcessEx + QueryProcessEx2，直连）；
// Info/Kill/Suspend/Start 依赖 SbieSvc，占位桩（server 波次）。

#pragma once

#include "Boxes.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

struct ProcEntry {
    ULONG pid = 0;
    std::wstring box, image;
    ULONG sessionId = 0;
    ULONG64 createTime = 0;
    ULONG flags = 0;
};

class ProcessRepository {
public:
    explicit ProcessRepository(sbie::drv::Api* api, sbie::svc::SvcClient& svc);

    std::vector<ProcEntry> Enum(bool allSessions, const std::wstring& box = L"");
    SbieStatus Info(ULONG pid, svc::ProcInfo* out);
    SbieStatus Kill(ULONG pid);
    SbieStatus KillBox(const std::wstring& box);
    SbieStatus Suspend(ULONG pid);
    SbieStatus Resume(ULONG pid);
    SbieStatus Start(const std::wstring& box, const std::wstring& cmd,
                     const std::wstring& dir, bool elevated, svc::RunResult* out);

private:
    sbie::drv::Api* api_;
    sbie::svc::SvcClient& svc_;
};

} // namespace sbie::model
