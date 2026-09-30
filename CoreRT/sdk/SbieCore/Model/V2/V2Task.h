// Sandboxie-OSS — SbieCore/Model/V2/V2Task.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 monitor 任务文件（docs/10-v2-design.md §8.2，方案 a：文件约定）：
//   * CLI ↔ 公共 monitor 的唯一通信通道 = monitors\<box>.task 文件
//     （与 ImportBox 目录通配同构——零 IPC，崩溃自愈）
//   * exec 启动进程后写任务文件；monitor 每 tick 重扫目录对账
//   * JSON 内容：盒名/盒目录/缓存路径/别名/创建者 pid/时间戳

#pragma once

#include "V2Common.h"

namespace sbie::model::v2 {

struct TaskEntry {
    std::wstring box, boxPath, cache, alias;
    DWORD creatorPid = 0;
    std::wstring created;
    bool encrypted = false;       // UseFileImage=y 盒：teardown 兜底 unmount
    std::wstring regRoot;         // 加密盒 KeyRootPath（NT），unmount 用
};

bool TaskExists(const std::wstring& box);
V2Err WriteTask(const TaskEntry& t);
V2Err DeleteTask(const std::wstring& box);       // 不存在 = 成功
// 全量枚举（坏文件跳过）
std::vector<TaskEntry> EnumTasks();

} // namespace sbie::model::v2
