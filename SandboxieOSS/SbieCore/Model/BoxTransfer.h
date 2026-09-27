// Sandboxie-OSS — SbieCore/Model/BoxTransfer.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 建箱类型预设 / 沙箱复制 / 导出导入（07-P1-1/2/4；实现与语义决策记录
// docs/04 §17）。全部为 Model 层 additive 新面（不动既有冻结契约）。
//
// 行为基准（01-license-map §2 允许范围——仅规格语义，未复制任何代码）：
//   * 类型预设键组：docs/07 §3.2 07-P1-1 记录的 NewBoxWizard 规格
//     （Hardened→UseSecurityMode=y；+Data Protection→叠加 UsePrivacyMode=y；
//     Compartment→NoSecurityIsolation=y + Template=RpcPortBindingsExt 追加；
//     加密类 UseFileImage 链路许可证禁，不出预设表）；
//   * 复制：整节读出（ListSettings+GetList）→ 节替换写入新名（Rename 的
//     前半，04 §8.15 协议技巧）+ 可选目录树拷贝（Duplicate Box with Content）；
//   * 导出/导入：ini 节文本 + FileRoot 目录树；包内布局 box.ini + content/…；
//     归档形态 = 自研最小 zip（Util/Zip，store），目录形态 = 原样拷贝。
//
// 触发器交互（波 A RunBoxTriggers 设施）：本文件所有操作**不删除**任何
// 节/内容，因此不触发 OnBoxDelete（复制语义——源箱只读）；新节写入不产生
// 进程事件，守护监视器（07-P0-2）不受影响。

#pragma once

#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

// ---------------------------------------------------------------------------
// 07-P1-1 建箱类型预设
// ---------------------------------------------------------------------------

struct BoxTypeKey {
    const wchar_t* key;
    const wchar_t* value;
    bool append;   // true = SetAppend（Template= 多值），false = Set（单值键）
};

struct BoxTypePreset {
    const wchar_t* type;    // CLI --type 值（小写连字符形）
    const wchar_t* label;   // box types 的说明列
    std::vector<BoxTypeKey> keys;
};

// 预设表（6 类 = NewBoxWizard 7 类去掉许可证禁用的 Confidential Encrypted；
// standard = 仅基础键 Enabled=y，预设表内 0 键）。
const std::vector<BoxTypePreset>& BoxTypePresets();

// --type 值查表（大小写不敏感）；未知返回 nullptr。
const BoxTypePreset* FindBoxTypePreset(const std::wstring& type);

// 应用预设键组（Enabled=y 写入之后调用；refresh 收在最后一个键——
// 中间写不热重载，与 rename/HBoxSet 的重放节流同款）。standard（0 键）
// 为空操作。
SbieStatus ApplyBoxTypeKeys(const std::wstring& box,
                            const BoxTypePreset& preset,
                            const std::wstring& password);

// ---------------------------------------------------------------------------
// 共享：节文本 / 目录树拷贝
// ---------------------------------------------------------------------------

// 内容统计（copy --content / export / import 的输出行）
struct TransferStats {
    unsigned long long files = 0;
    unsigned long long dirs = 0;
    unsigned long long bytes = 0;
};

// 整节读出为 "Key=Value\n" 行集（驱动缓存，NO_TEMPLS/NO_EXPAND——rename
// 同款；空节返回空串）。
SbieStatus ReadBoxSection(const std::wstring& box, std::wstring* text);

// 目录树递归拷贝（FindFirstFileExW 遍历 + CreateDirectoryW + CopyFileW
// 保留 mtime；重解析点跳过——与 BoxUsage/Recovery 口径一致）。
SbieStatus CopyDirTree(const std::wstring& srcDir, const std::wstring& dstDir,
                       TransferStats* stats);

// ---------------------------------------------------------------------------
// 07-P1-4 复制沙箱
// ---------------------------------------------------------------------------

// 整节复制（含模板引用 Template= 多值原样随节）+ 可选内容拷贝。
//   src 不存在 / dst 已存在 → NOT_FOUND；src==dst / dst 名非法 → INVALID；
//   不触发任何触发器、不动源箱（复制语义）。dst 节以 refresh=true 落盘。
SbieStatus CopyBox(const std::wstring& src, const std::wstring& dst,
                   bool withContent, const std::wstring& password,
                   TransferStats* contentStats = nullptr);

// ---------------------------------------------------------------------------
// 07-P1-2 导出 / 导入
// ---------------------------------------------------------------------------

// 导出：dest 为归档（asArchive=true，覆盖既有文件）或目录（目录形态，
// box.ini + content\ 落在 dest 下；dest 已存在则并入/覆盖同名文件）。
// 导出物 = ini 节文本（box.ini）+ FileRoot 目录树（content/…）。
// detail（可空）承接文件/包级错误详情（CLI/server 拼进错误消息）。
SbieStatus ExportBox(const std::wstring& box, const std::wstring& dest,
                     bool asArchive, TransferStats* out,
                     std::wstring* detail = nullptr);

// 导入：packagePath 为归档文件（fromArchive=true）或目录形态包；
// newName 合法且不存在（已存在 → NOT_FOUND）；包缺 box.ini → INVALID
// （坏包）。导入 = 建节（refresh）+ 还原目录树到新箱 FileRoot。
SbieStatus ImportBox(const std::wstring& packagePath,
                     const std::wstring& newName, bool fromArchive,
                     const std::wstring& password, TransferStats* out,
                     std::wstring* detail = nullptr);

} // namespace sbie::model
