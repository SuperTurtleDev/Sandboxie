// Sandboxie-OSS — SbieCore/Model/Recovery.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 文件恢复（快恢复）领域逻辑（06 §P0-12；契约登记：docs/04 §2.4/§14）。
//
// 行为基准（GPL core 可直接参考，01-license-map §2）：
//   * 扫描面 = box 的 RecoverFolder 多值设置（含模板注入项，驱动展开 %env%）
//     对应的沙箱内目录树（apps\control\BoxFile.cpp CreateQuickRecoveryFolders
//     语义）；
//   * 沙箱路径 → 真实路径映射 = QSbieAPI CSbieAPI::GetRealPath 规则（LGPL
//     参考）：\drive\X\ → X:\、\user\current → 用户目录、\user\all →
//     ProgramData、\user\public → 公共目录、\share → UNC、剥 \snapshot-*
//     前缀；逆向（真实 → 沙箱）= GetBoxedPath 规则（SeparateUserFolders
//     缺省 y 时用户三类目录优先于盘符）；
//   * 拷出 = CopyFileW（拷贝语义、保留 mtime；SbieCtrl 为移动语义——FO_MOVE，
//     CLI 采非破坏性拷贝，docs/04 §14 登记）。
//
// 纯用户态文件/配置读（驱动缓存），无 SbieSvc 依赖；add 走 ConfigStore 写
// 路径（经 SbieSvc）。

#pragma once

#include "Boxes.h"
#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

// 一个可恢复文件（recover list 的一行）
struct RecoverEntry {
    std::wstring sandboxPath;   // 沙箱内绝对 DOS 路径（C:\Sandbox\…\user\…）
    std::wstring boxPath;       // 相对 FileRoot（\user\current\Documents\f.txt）
    std::wstring targetPath;    // 恢复目标（真实 DOS 路径；\share 来源为 UNC）
    unsigned long long size = 0;   // 字节
};

// recover copy 的结果
struct RecoverCopyOutcome {
    unsigned long long copiedFiles = 0;
    unsigned long long copiedBytes = 0;
    std::wstring failedPath;          // 首个失败条目的目标路径（空=全部成功）
    SbieStatus status = SbieStatus::OK;   // 首个失败的语义码（成功=OK）
    unsigned long win32Error = 0;     // 首个失败的原始 GetLastError（诊断）
    // 波 B（07-P1-3）additive：OnFileRecovery 检查器拒绝的条目（跳过、不
    // 计失败——被拒文件保持沙箱内原样，输出侧列出）
    unsigned long long skippedFiles = 0;
    std::vector<std::wstring> skippedPaths;
};

// 波 B（07-P1-3）additive：recover copy 的行为选项
struct RecoverCopyOptions {
    bool move = false;          // CopyFileW 成功后 DeleteFileW 沙箱源（移动语义）
    bool runCheckers = true;    // 恢复前执行箱键 OnFileRecovery 的检查器命令
};

class RecoveryManager {
public:
    explicit RecoveryManager(const BoxInfo& box);

    // 列出可恢复文件：枚举 RecoverFolder 声明目录（含模板注入、展开 %env%）
    // → 映射为沙箱内路径 → 递归收集文件。结果按 sandboxPath 排序并去重
    //（索引即 recover copy 的 <index>）。沙箱未初始化（无 FileRoot）= 空表。
    std::vector<RecoverEntry> List();

    // 沙箱内绝对路径 → 恢复目标路径（GetRealPath 规则；失败 false）
    bool MapToRealPath(const std::wstring& sandboxPath, std::wstring* out) const;

    // 拷出（拷贝语义，保留 mtime）。toDir 空 = 恢复到原位（各条目的
    // targetPath）；非空 = toDir + FileRoot 相对结构（建目录）。
    // 目标已存在且 !overwrite → GENERIC（win32Error=ERROR_FILE_EXISTS 语义
    // 置 0，failedPath 指明目标）；首个失败即停（已拷出的条目保留）。
    SbieStatus Copy(const std::vector<std::wstring>& sandboxPaths,
                    const std::wstring& toDir, bool overwrite,
                    RecoverCopyOutcome* out);

    // 波 B（07-P1-3）additive 扩展签名：Copy 的超集——
    //   * opts.move：CopyFileW 成功后 DeleteFileW 沙箱源（SandMan 恢复的
    //     移动语义；删源失败 = 该条目失败、已拷出的目标保留）；
    //   * opts.runCheckers：恢复前对每个文件执行箱键 OnFileRecovery 的
    //     检查器命令（值原样读出 + %SANDBOX% 展开 + 追加带引号的沙箱路径
    //     参数，宿主执行、≤15s/条；任一检查器非零退出 = 拒绝该文件——
    //     记入 outcome.skipped*、跳过不拷贝、继续其余文件；07 记录的
    //     SandMan 校验器语义，docs/04 §17 决策）。
    // 被跳过条目不算失败（整体 OK）；其余语义与 Copy 一致。
    SbieStatus CopyEx(const std::vector<std::wstring>& sandboxPaths,
                      const std::wstring& toDir, bool overwrite,
                      const RecoverCopyOptions& opts, RecoverCopyOutcome* out);

private:
    BoxInfo box_;
};

// box recover add：追加 RecoverFolder 配置值。folder 接受 %var% 引用、
// DOS 盘符路径、UNC（\\server\share）或 NT 路径（\Device\…），**原样存储**
//（与 SandMan OnAddFolder 写 DOS 形态一致；驱动读路径的 Conf_Expand 对
// DOS 路径自动转 NT——conf_expand.c File_TranslateDosToNt 分支，两形态等价；
// 相对路径/无法判读 → INVALID）。经 SbieSvc 写路径（refresh）。
SbieStatus RecoverAddFolder(const std::wstring& box, const std::wstring& folder,
                            const std::wstring& password);

} // namespace sbie::model
