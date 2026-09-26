// Sandboxie-OSS — SbieCore/Model/Snapshots.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 快照领域逻辑（契约：04-modules.md §2.4；签名冻结）。
// 纯文件系统操作（<FileRoot>\Snapshots.ini + snapshot-<ID>\，行为对齐
// QSbieAPI SandBox.cpp:353-505，LGPL 仅参考——04 §8.3）。
// M1 状态：契约头；实现占位（M2 文件操作波次）。

#pragma once

#include "Boxes.h"
#include "../Util/Status.h"

#include <optional>
#include <string>
#include <vector>

namespace sbie::model {

struct SnapshotInfo {
    std::wstring id;      // 十进制字符串，从 1 递增
    std::wstring parentId, name, info;
    ULONGLONG date = 0;
};

class SnapshotManager {
public:
    explicit SnapshotManager(const BoxInfo& box);
    std::vector<SnapshotInfo> List(std::wstring* currentId, std::wstring* defaultId);
    bool HasAny();
    SbieStatus Take(const std::wstring& name);              // 要求沙箱内无进程
    SbieStatus Remove(const std::wstring& id);
    SbieStatus Select(const std::wstring& id);              // 切换当前（要求无进程）
    SbieStatus SetInfo(const std::wstring& id,
                       std::optional<std::wstring> name,
                       std::optional<std::wstring> info);

private:
    BoxInfo box_;
};

} // namespace sbie::model
