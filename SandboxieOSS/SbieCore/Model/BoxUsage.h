// Sandboxie-OSS — SbieCore/Model/BoxUsage.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 磁盘占用扫描（契约：04-modules.md §2.4；签名冻结）。
// 行为 = 资源管理器"大小"（递归统计沙箱根目录）——01-license-map §5.2：
// 禁止参考 SandMan\BoxMonitor.cpp。
//
// 契约的 additive 扩展（box size 波次，docs/04 §14 登记）：原 bytes-only
// 签名保留；新增 BoxUsageStats 结构（total_bytes/files/dirs）与对应重载，
// 供 `box size` 的三列输出与 --json 使用。

#pragma once

#include "../Util/Status.h"

#include <functional>
#include <string>

namespace sbie::model {

// 递归统计结果：文件字节数（目录本身不计，重解析点跳过——与资源管理器
// "大小"列的口径一致）+ 文件数 + 目录数（不含根、不含被跳过的重解析点）。
struct BoxUsageStats {
    unsigned long long totalBytes = 0;
    unsigned long long files = 0;
    unsigned long long dirs = 0;
};

// 原契约签名（bytes-only）：内部委托 BoxUsageStats 版。
SbieStatus ScanBoxSize(const std::wstring& fileRoot, ULONGLONG* bytes,
                       const std::function<bool(ULONGLONG)>& progress /*可取消*/);

// 扩展签名（box size 波次）：progress 周期性收到累计字节（每 256 个条目），
// 返回 false 取消扫描（取消返回 GENERIC；当前无调用方取消）。
SbieStatus ScanBoxSize(const std::wstring& fileRoot, BoxUsageStats* out,
                       const std::function<bool(ULONGLONG)>& progress);

} // namespace sbie::model
