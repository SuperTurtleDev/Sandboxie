// Sandboxie-OSS — SbieCore/Model/V2/V2Registry.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 注册协议（docs/10-v2-design.md §6/§7）：
//   * 注册 = 生成缓存（展开+自检）落位 boxes\<box>.ini + SbieApi_ReloadConf
//     IOCTL 直调（Conf_Api_Reload 无 admin/leader 门槛，conf.c:1696-1702）。
//     主 ini 的 [GlobalSettings] ImportBox=<boxes 目录>\ 通配行 = 部署期一次性。
//   * 注销 = 删缓存文件 + ReloadConf。文件放删的原子性替代主 ini 键级写竞争。
//   * 探测（"注册尚在/已消失"）= QueryConf(<box>, Enabled, NO_GLOBAL)：
//     rc>=0 即节在（值含启用态）；rc<0 即不在。
//   * 冲突预检 = QueryConf(<box>, FileRootPath, NO_GLOBAL|NO_EXPAND) 与本盒目录
//     比对：同名节已归属其他目录 = NAME_COLLISION。
//   * running.lock（盒目录内，生命周期标记）与 alias 索引（aliases.json，
//     Local\SbieOSS_AliasLock 互斥下读-改-写，原子替换）。
//
// 依赖：DriverApi（QueryConf/ReloadConf/EnumBoxProcesses）。无 SbieSvc。

#pragma once

#include "V2Common.h"
#include "V2Template.h"

#include <vector>

namespace sbie::model::v2 {

// ---------------------------------------------------------------------------
// 探测
// ---------------------------------------------------------------------------

enum class RegState {
    Absent,          // 驱动配置里无该节
    Registered,      // 节在且 Enabled=y
    RegisteredDisabled, // 节在但未启用（异常形态，ps 呈现）
};

// 注册探测（enabledOut 可空）
V2Err ProbeRegistration(const std::wstring& box, RegState* state);
// 同名碰撞检查（缓存文件为源——文件名=盒名=节名唯一；不用驱动内存侧
// Conf 查询，reload 前的内存可能有已删缓存的幽灵节）。返回 true = 既有
// 缓存指向其他目录（ownerOut 填该目录）。
bool CacheBelongsToOtherDir(const std::wstring& box, const std::wstring& boxDir,
                            std::wstring* ownerOut /*=nullptr*/);

// 轮询等待（wantPresent=true 等注册可见 / false 等注册消失）。
// 50ms 起指数退避至 200ms；超时 ms 返回 false（非错误码——调用方定语境）。
bool WaitForRegistration(const std::wstring& box, bool wantPresent, DWORD timeoutMs);

// ---------------------------------------------------------------------------
// 注册 / 注销（完整流水线）
// ---------------------------------------------------------------------------

// 注册 = 展开 sandbox.ini → 写缓存（自检）→ ReloadConf → 等注册可见。
// 幂等：同名同目录已注册时仅刷新缓存。outApplied 可空（溯源）。
// password：ImportBox 行自动部署经 SbieSvc 写通道的密码（EditPassword 机器；
// 空串 = 无密码——调用方可先行交互获取，R2/R3）。
V2Err RegisterBox(const std::wstring& box, const std::wstring& boxDir,
                  std::vector<std::wstring>* outApplied /*=nullptr*/,
                  const std::wstring& password = std::wstring());

// 注销 = 删缓存 → ReloadConf → 等注册消失。
// busyCheck：盒内有进程 → BOX_BUSY（cli/monitor 传入是否启用该检查）。
V2Err UnregisterBox(const std::wstring& box, bool busyCheck);

// sync-config = 重展开重写缓存 + ReloadConf（盒运行中由调用方决定放行与否）。
V2Err SyncBoxConfig(const std::wstring& box, const std::wstring& boxDir,
                    const std::wstring& password = std::wstring());

// ReloadConf 的薄包装（折叠 NTSTATUS 细节）
V2Err ReloadDriverConf();

// 部署前提自愈：主 ini [GlobalSettings] 必须有一条 ImportBox=<boxes 目录>\
// 通配行（尾随 '\' 触发驱动目录通配，conf.c:1141-1149）。已存在等值行=幂等
// 通过；缺失则经 SbieSvc MSGID_SBIE_INI ADD_SETTING 追加并 refresh（官方
// 写通道；EditPassword 设置时失败并给出手工指引）。register 前调用。
V2Err EnsureImportBoxLine(const std::wstring& password = std::wstring());

// ---------------------------------------------------------------------------
// running.lock（盒目录内）
// ---------------------------------------------------------------------------

struct LockInfo {
    std::wstring box, alias, boxPath, cache;
    DWORD creatorPid = 0;
    std::wstring created;
};

bool LockExists(const std::wstring& boxDir);
LockInfo ReadLock(const std::wstring& boxDir);           // 缺字段回空结构
V2Err WriteLock(const std::wstring& boxDir, const LockInfo& info);
V2Err DeleteLock(const std::wstring& boxDir);
// stale 诊断：creator_pid 已亡（OpenProcess 失败）
bool LockCreatorDead(const LockInfo& info);

// ---------------------------------------------------------------------------
// alias 索引（aliases.json）
// ---------------------------------------------------------------------------

struct AliasEntry {
    std::wstring alias, box, boxPath, added;
};

// 别名 → 条目；未命中返回 false
bool AliasGet(const std::wstring& alias, AliasEntry* out);
// 增改（同 alias 覆盖；同 boxPath 旧别名被替换——一盒一名语义）
V2Err AliasSet(const std::wstring& alias, const std::wstring& box,
               const std::wstring& boxPath);
V2Err AliasRemove(const std::wstring& alias);            // 不存在 = 成功
V2Err AliasRemoveForBox(const std::wstring& box);        // 盒的全部别名
std::vector<AliasEntry> AliasList();

// ---------------------------------------------------------------------------
// 目标解析（PATH\TO\box 或 *alias）
// ---------------------------------------------------------------------------

struct BoxTarget {
    std::wstring box;        // 盒名（=缓存基名=驱动节名）
    std::wstring boxDir;     // 绝对路径
    std::wstring alias;      // 命中 alias 时非空
    V2Err status;
};

// '*' 前缀 → alias；否则路径（相对 cwd）。校验盒名/目录存在/三件套 sandbox.ini。
BoxTarget ResolveTarget(const std::wstring& arg);

} // namespace sbie::model::v2
