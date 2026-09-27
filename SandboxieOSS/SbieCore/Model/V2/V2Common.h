// Sandboxie-OSS — SbieCore/Model/V2/V2Common.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 公共底座：运行区路径布局、盒名校验、通用 ini 解析、时间戳。
//
// V2 模型（docs/10-v2-design.md，冻结稿）：
//   * 盒目录（用户任意位置）= 真理源：sandbox.ini + drive\ + user\ + running.lock
//   * %LOCALAPPDATA%\SandboxieOSS\ = 可再生运行件：
//       boxes\<box>.ini    展开缓存（ImportBox 通配目录 = 注册工件）
//       monitors\<box>.task monitor 任务文件
//       aliases.json       alias 索引
//   * 主 ini 只有一条部署期写死的 [GlobalSettings] ImportBox=<boxes 目录>\
//     （尾随反斜杠触发驱动目录通配，conf.c Conf_Import_AllIncludes）
//
// 关键驱动行为锚点（详见 docs/10 §4.3/§6）：
//   * 缓存必须单节且节名==文件基名（conf.c:1037-1046）
//   * 文件名（含 .ini）≤44 WCHAR、≥5（conf.c:924，BOXNAME_COUNT=40）
//   * 驱动会在导入节尾部自动注入 FileRootPath=缓存路径剥扩展名（conf.c:1080-1089），
//     但按名取值走 map 桶序=插入序（map.h:53 append=TRUE → 桶尾），Box_InitPaths
//     读 index0（box.c:299）⇒ 缓存里显式写的 FileRootPath 真实生效
//   * 盒可用要求节内 Enabled=y（conf_user.c:381-388 Conf_IsEnabled）

#pragma once

#include "../../Util/Status.h"

#include <windows.h>
#include <string>
#include <vector>

namespace sbie::model::v2 {

// ---------------------------------------------------------------------------
// 错误信封：SbieStatus 码 + 人类可读消息（cli 层直接透传给 EmitError）
// ---------------------------------------------------------------------------

struct V2Err {
    SbieStatus code = SbieStatus::OK;
    std::wstring msg;           // 诊断细节（可空）
    bool Ok() const { return code == SbieStatus::OK; }
};

// ---------------------------------------------------------------------------
// 运行区路径（全部可再生；目录惰性创建）
// ---------------------------------------------------------------------------

// %LOCALAPPDATA%\SandboxieOSS（无 LOCALAPPDATA 时回退 %APPDATA%）
std::wstring AppDataRoot();
std::wstring BoxesDir();            // <root>\boxes        （ImportBox 通配目录）
std::wstring MonitorsDir();         // <root>\monitors     （monitor 任务目录）
std::wstring AliasIndexPath();      // <root>\aliases.json
std::wstring MonitorLogPath();      // <root>\monitor.log
std::wstring CachePathFor(const std::wstring& box);        // boxes\<box>.ini
std::wstring TaskPathFor(const std::wstring& box);         // monitors\<box>.task
// 创建全部运行目录（幂等）
bool EnsureRuntimeDirs();

// ---------------------------------------------------------------------------
// 盒名（= 盒目录基名 = 驱动节名 = 缓存文件基名）
// ---------------------------------------------------------------------------

// 规则：1..38 WCHAR；[A-Za-z0-9_]；不得含 '.'（ImportBox 注入值按文件名首个
// '.' 截断，conf.c:1002）；非保留节名。失败时 err 填原因。
V2Err ValidateBoxName(const std::wstring& name);

// 路径 → 绝对规范化（展开相对路径、剥尾随 '\'）；失败返回空串
std::wstring NormalizeDirPath(const std::wstring& path);
// 盒目录绝对路径 → 盒名（基名）
std::wstring BoxNameFromPath(const std::wstring& boxDir);

// ---------------------------------------------------------------------------
// 通用 ini 解析（sandbox.ini / 模板文件 / running.lock 共用）
// ---------------------------------------------------------------------------

struct IniKeyValue {
    std::wstring key;
    std::wstring value;
};

struct IniSectionData {
    std::wstring name;                       // '[name]'；节前散行归入 name 为空的节
    std::vector<IniKeyValue> entries;        // 保序、保重复
};

struct IniFileData {
    std::vector<IniSectionData> sections;
    const IniSectionData* Find(const std::wstring& name) const;   // 大小写不敏感
    IniSectionData* Find(const std::wstring& name);
};

// 读 + 解析。支持 UTF-8（带/不带 BOM）与 UTF-16LE（BOM）。
// 语法：'[' 节头；'#' 注释行（驱动语义，conf.c:834——';' 不是注释符）；
// 'key=value'（键去尾空白、值去首尾空白；'=' 缺失 = 错误）。
V2Err ParseIniFile(const std::wstring& path, IniFileData* out);

// UTF-8（带 BOM）原子写：先写 <path>.tmp 再 MoveFileExW(REPLACE_EXISTING)。
V2Err WriteTextFileAtomic(const std::wstring& path, const std::string& utf8);

// 小工具
std::wstring NowIsoTimestamp();              // 2026-09-27T12:00:03Z
bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
std::wstring FileReadAll(const std::wstring& path, V2Err* err);   // 空=失败/空文件
// 盒内进程数（EnumBoxProcesses 全会话；失败 err 置码，返回 SIZE_MAX）
size_t BoxProcessCount(const std::wstring& box, V2Err* err);

// 盒内"用户进程"数：排除 V2/官方自举服务镜像（SandboxieRpcSs/DcomLaunch/
// BITS/WUAU/Crypto—— RpcSs 的 linger 自杀是事件驱动，上游竞态下可永久滞留，
// apps/com/RpcSs/linger.c:387-602；docs/11 blocker-1）。monitor 归零判定与
// exec 濒死分支统一用本口径（单一判据，消灭双判据死锁）。枚举失败返回
// SIZE_MAX（调用方保守处理）。
size_t BoxUserProcessCount(const std::wstring& box, V2Err* err);

} // namespace sbie::model::v2
