// Sandboxie-OSS — SbieCore/Util/Status.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 跨模块统一状态码（04-modules.md §1/§6 契约）：
//   0..9 与 CLI 退出码语义一致（稳定契约，脚本依赖）；NTSTATUS/win32 错误经
//   FromNtStatus() 折叠进本枚举，原始 NTSTATUS 由调用方自行保留时用 NtStatusText()。
//   >100 为内部扩展码（ERR_JSON/ERR_SBIEDLL/ERR_SVC_TRANSPORT 等，见各模块契约引用），
//   到 cli 层经 ToExitCode() 折叠为 1（GENERIC）或语义码。

#pragma once

#include <windows.h>
#include <string>

namespace sbie {

enum class SbieStatus : long {
    OK = 0,                 // 成功
    GENERIC = 1,            // 未分类失败
    USAGE = 2,              // 命令行语法错误
    DRIVER_UNAVAILABLE = 3, // 驱动未运行/未安装（STATUS_SERVER_DISABLED 等）
    SERVER_UNAVAILABLE = 4, // server 拉起失败/不可降级
    NOT_FOUND = 5,          // 目标（box/pid/setting/…）不存在
    ACCESS_DENIED = 6,      // 权限不足/密码错/在沙箱内运行
    INVALID = 7,            // 名字/参数值非法
    RETRY_SUGGESTED = 8,    // （V1 遗留语义位；V2 无 server，不再产生）
    BOX_BUSY = 9,           // 沙箱内有活动进程

    // ---- V2 扩展（docs/10-v2-design.md §9.2；仍为进程退出码）----
    STATE_TIMEOUT = 10,     // 等注册可见/消失超时（--wait）
    TEMPLATE_ERROR = 11,    // 模板缺失/环/歧义/变量缺失/类目不符
    CACHE_INVALID = 12,     // 缓存自检失败 / 注册 reload 失败
    SPAWN_FAILED = 13,      // RunSandboxed/进程启动失败

    // ---- 内部扩展码（不直接作为进程退出码）----
    ERR_JSON = 100,          // JSON 解析/序列化失败（04 §7.3）
    ERR_SBIEDLL = 101,       // SbieDll.dll 加载/绑定失败（04 §2.1）
    ERR_SVC_TRANSPORT = 102, // SbieSvc LPC 传输失败（04 §2.2）
    ERR_NOT_IMPLEMENTED = 103 // 该路径在本构建中尚未实现（占位桩）
};

// NTSTATUS → SbieStatus 错误码翻译表（02-driver-api.md §5）。
// 未列出的失败值一律折叠为 GENERIC；STATUS_SUCCESS 之外的成功值（告警）按 OK 处理。
SbieStatus FromNtStatus(LONG ntstatus);

// SbieStatus → CLI 退出码（04 §6）：0..9 原样；扩展码折叠。
int ToExitCode(SbieStatus s);

// 状态码短名（"OK"/"DRIVER_UNAVAILABLE"/…），诊断输出用。
const wchar_t* StatusName(SbieStatus s);

// NTSTATUS 的人类可读文案：优先 ntdll.dll 的消息表（QSbieAPI CSbieAPI__FormatNtStatus
// 同款做法，负值先经 RtlNtStatusToDosError，LGPL 行为参考），失败时回退 "0xC000xxxx"。
std::wstring NtStatusText(LONG ntstatus);

// 便捷谓词
inline bool Ok(SbieStatus s) { return s == SbieStatus::OK; }

} // namespace sbie
