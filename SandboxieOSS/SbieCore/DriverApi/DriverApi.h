// Sandboxie-OSS — SbieCore/DriverApi/DriverApi.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// SbieDll.dll 动态绑定 + 驱动调用封装（契约：04-modules.md §2.1；绑定规格：
// 02-driver-api.md §1/§3）。
//
// ── 契约（冻结）───────────────────────────────────────────────────────────
//   LoadSbieDll / Loaded / GetVersion / ApiP / InSandbox / DriverAlive 的
//   签名与语义按 04 §2.1，不得变更。
// ── 本文件对契约的补充（additive，已登记 docs\04-modules.md §8）──────────
//   * 绑定表成员：02 §1 规范的 SbieApiBindings 机器可读形式（struct Api）。
//   * 薄封装组：QueryDriverInfo / EnumBoxes / IsBoxEnabled / QueryBoxPath /
//     EnumBoxProcesses / QueryProcessById / QueryConfText / QueryConfList /
//     EnumConfSections / ReloadConf / GetHomePath / LastNtStatus（M1 交付，
//     04 §2.1 之外按"02 文档分组所需的全部绑定与薄封装"要求补充）。
//   * LastLoadError / LoadedFrom / AbiMatches：诊断辅助。
//
// 原型对齐 Sandboxie\core\dll\sbieapi.h（GPLv3，vendored 参考；导出均为
// extern "C"，x64 单一调用约定，统一声明为默认 cdecl，CALLBACK 展开为空、
// 保留以自文档——02 §1）。

#pragma once

#include "../Util/Status.h"

#include <windows.h>
#include <string>
#include <vector>

namespace sbie::drv {

// ---------------------------------------------------------------------------
// 契约区（04-modules.md §2.1）
// ---------------------------------------------------------------------------

// 进程启动时调用一次（幂等；失败后可换目录重试，全部 Api() 调用返回 ERR_SBIEDLL）。
// 搜索顺序（02 §1 + 任务书）：explicitDir → 本 exe 同目录 →
// C:\Program Files\Sandboxie-Plus\ → 注册表 SbieSvc 服务的 ImagePath 目录 →
// 默认 DLL 搜索（PATH）。
bool LoadSbieDll(const std::wstring& explicitDir = L"");

// 已成功加载且全部导出绑定完成
bool Loaded();

struct VersionInfo {
    std::wstring version;   // "5.73.5"
    ULONG abi = 0;          // 0x57230
};

// SbieApi_GetVersionEx（失败/未加载：version=L"unknown"/abi=0）
VersionInfo GetVersion();

// 绑定表（02 §1 规范）。所有驱动经由此结构调用，禁止散落 GetProcAddress。
struct Api;
Api* ApiP();

// SbieApi_QueryProcess(self) 成功 => 在沙箱内 => 全部命令拒绝（02 §6）
bool InSandbox();

// 尝试打开 \Device\SandboxieDriverApi；false = 未装/未跑
bool DriverAlive();

// ---------------------------------------------------------------------------
// 绑定表：函数指针类型（02 §1/§3，与 sbieapi.h 原型逐参一致）
// ---------------------------------------------------------------------------

using P_SbieApi_GetVersionEx    = LONG (CALLBACK*)(WCHAR* version_string /*[16]*/,
                                                   ULONG* abi_version);
using P_SbieApi_Call            = LONG (CALLBACK*)(ULONG api_code, LONG arg_num, ...);
using P_SbieApi_Ioctl           = LONG (CALLBACK*)(ULONG64* parms);
using P_SbieApi_QueryDrvInfo    = LONG (CALLBACK*)(ULONG info_class, VOID* info_data,
                                                   ULONG info_size);
using P_SbieApi_GetHomePath     = LONG (CALLBACK*)(WCHAR* NtPath, ULONG NtPathMaxLen,
                                                   WCHAR* DosPath, ULONG DosPathMaxLen);

using P_SbieApi_EnumBoxesEx     = LONG (CALLBACK*)(LONG index /*init -1*/,
                                                   WCHAR* box_name /*BOXNAME_COUNT*/,
                                                   BOOLEAN return_all_sections);
using P_SbieApi_IsBoxEnabled    = LONG (CALLBACK*)(const WCHAR* box_name /*[40]*/);
using P_SbieApi_QueryBoxPath    = LONG (CALLBACK*)(const WCHAR* box_name,
                                                   WCHAR* out_file_path,
                                                   WCHAR* out_key_path,
                                                   WCHAR* out_ipc_path,
                                                   ULONG* inout_file_path_len,
                                                   ULONG* inout_key_path_len,
                                                   ULONG* inout_ipc_path_len);
using P_SbieApi_QueryProcessPath= LONG (CALLBACK*)(HANDLE ProcessId,
                                                   WCHAR* out_file_path,
                                                   WCHAR* out_key_path,
                                                   WCHAR* out_ipc_path,
                                                   ULONG* inout_file_path_len,
                                                   ULONG* inout_key_path_len,
                                                   ULONG* inout_ipc_path_len);

using P_SbieApi_QueryProcess    = LONG (CALLBACK*)(HANDLE ProcessId,
                                                   WCHAR* out_box_name /*[40]*/,
                                                   WCHAR* out_image_name /*[96]*/,
                                                   WCHAR* out_sid /*[96]*/,
                                                   ULONG* out_session_id);
using P_SbieApi_QueryProcessEx2 = LONG (CALLBACK*)(HANDLE ProcessId,
                                                   ULONG image_name_len_in_wchars,
                                                   WCHAR* out_box_name /*[40]*/,
                                                   WCHAR* out_image_name,
                                                   WCHAR* out_sid /*[96]*/,
                                                   ULONG* out_session_id,
                                                   ULONG64* out_create_time);
using P_SbieApi_QueryProcessInfo= ULONG64 (CALLBACK*)(HANDLE ProcessId, ULONG info_type);
using P_SbieApi_QueryProcessInfoStr = LONG (CALLBACK*)(HANDLE ProcessId, ULONG info_type,
                                                       WCHAR* out_str, ULONG* inout_str_len);
using P_SbieApi_EnumProcessEx   = LONG (CALLBACK*)(const WCHAR* box_name /*[40]*/,
                                                   BOOLEAN all_sessions,
                                                   ULONG which_session /*-1=当前*/,
                                                   ULONG* boxed_pids /*[512]*/,
                                                   ULONG* boxed_count);
using P_SbieApi_OpenProcess     = LONG (CALLBACK*)(HANDLE* ProcessHandle,
                                                   HANDLE ProcessId);

using P_SbieApi_QueryConf       = LONG (CALLBACK*)(const WCHAR* section /*[66]*/,
                                                   const WCHAR* setting /*[66]*/,
                                                   ULONG setting_index,
                                                   WCHAR* out_buffer,
                                                   ULONG buffer_len);
using P_SbieApi_QueryConfBool   = BOOLEAN (CALLBACK*)(const WCHAR* section,
                                                      const WCHAR* setting,
                                                      BOOLEAN def);
using P_SbieApi_QueryConfNumber = ULONG (CALLBACK*)(const WCHAR* section,
                                                    const WCHAR* setting,
                                                    ULONG def);
using P_SbieApi_QueryConfNumber64 = ULONG64 (CALLBACK*)(const WCHAR* section,
                                                        const WCHAR* setting,
                                                        ULONG64 def);
using P_SbieApi_UpdateConf      = ULONG (CALLBACK*)(ULONG op,
                                                    const WCHAR* section,
                                                    const WCHAR* setting,
                                                    const WCHAR* value);
using P_SbieApi_ReloadConf      = LONG (CALLBACK*)(ULONG session_id, ULONG flags);

using P_SbieApi_SessionLeader   = LONG (CALLBACK*)(ULONG session_id, HANDLE* ProcessId);
using P_SbieApi_GetMessage      = ULONG (CALLBACK*)(ULONG* MessageNum, ULONG SessionId,
                                                    ULONG* MessageId, ULONG* Pid,
                                                    wchar_t* Buffer, ULONG Length);

using P_SbieApi_MonitorControl  = LONG (CALLBACK*)(ULONG* NewState, ULONG* OldState);
using P_SbieApi_MonitorGetEx    = LONG (CALLBACK*)(ULONG* Type, ULONG* Pid, ULONG* Tid,
                                                   WCHAR* Name /*[256]*/);

// SbieDll_*（02 §3.7）
using P_SbieDll_PortName        = const WCHAR* (CALLBACK*)();
using P_SbieDll_RunStartExe     = BOOL (CALLBACK*)(const WCHAR* cmd, const wchar_t* boxname);
using P_SbieDll_RunFromHome     = BOOLEAN (CALLBACK*)(const WCHAR* pgmName,
                                                      const WCHAR* pgmArgs,
                                                      STARTUPINFOW* si,
                                                      PROCESS_INFORMATION* pi);
using P_SbieDll_FormatMessage0  = WCHAR* (CALLBACK*)(ULONG code);
using P_SbieDll_FormatMessage1  = WCHAR* (CALLBACK*)(ULONG code, const WCHAR* ins1);
using P_SbieDll_FormatMessage2  = WCHAR* (CALLBACK*)(ULONG code, const WCHAR* ins1,
                                                     const WCHAR* ins2);
using P_SbieDll_TranslateNtToDosPath = BOOLEAN (CALLBACK*)(WCHAR* path);

// 绑定表实现：成员名与导出名一致（02 §1）。任一导出缺失（老版本 DLL）时
// LoadSbieDll 失败并经 LastLoadError() 报名。
struct Api {
    // 版本与驱动状态（02 §3.1）
    P_SbieApi_GetVersionEx      SbieApi_GetVersionEx;
    P_SbieApi_QueryDrvInfo      SbieApi_QueryDrvInfo;
    P_SbieApi_GetHomePath       SbieApi_GetHomePath;

    // 沙箱枚举与路径（02 §3.2）
    P_SbieApi_EnumBoxesEx       SbieApi_EnumBoxesEx;
    P_SbieApi_IsBoxEnabled      SbieApi_IsBoxEnabled;
    P_SbieApi_QueryBoxPath      SbieApi_QueryBoxPath;
    P_SbieApi_QueryProcessPath  SbieApi_QueryProcessPath;

    // 进程（02 §3.3）
    P_SbieApi_QueryProcess      SbieApi_QueryProcess;
    P_SbieApi_QueryProcessEx2   SbieApi_QueryProcessEx2;
    P_SbieApi_QueryProcessInfo  SbieApi_QueryProcessInfo;
    P_SbieApi_QueryProcessInfoStr SbieApi_QueryProcessInfoStr;
    P_SbieApi_EnumProcessEx     SbieApi_EnumProcessEx;
    P_SbieApi_OpenProcess       SbieApi_OpenProcess;

    // 配置（02 §3.4）
    P_SbieApi_QueryConf         SbieApi_QueryConf;
    P_SbieApi_QueryConfBool     SbieApi_QueryConfBool;
    P_SbieApi_QueryConfNumber   SbieApi_QueryConfNumber;
    P_SbieApi_QueryConfNumber64 SbieApi_QueryConfNumber64;
    P_SbieApi_UpdateConf        SbieApi_UpdateConf;
    P_SbieApi_ReloadConf        SbieApi_ReloadConf;

    // 会话与日志（02 §3.5）
    P_SbieApi_SessionLeader     SbieApi_SessionLeader;
    P_SbieApi_GetMessage        SbieApi_GetMessage;

    // 监控/trace（02 §3.6；MONITOR_GET2 无导出，走 SbieApi_Ioctl/SbieApi_Call）
    P_SbieApi_MonitorControl    SbieApi_MonitorControl;
    P_SbieApi_MonitorGetEx      SbieApi_MonitorGetEx;

    // 传输与杂项（02 §1/§3.7）
    P_SbieApi_Call              SbieApi_Call;
    P_SbieApi_Ioctl             SbieApi_Ioctl;
    P_SbieDll_PortName          SbieDll_PortName;
    P_SbieDll_RunStartExe       SbieDll_RunStartExe;
    P_SbieDll_RunFromHome       SbieDll_RunFromHome;
    P_SbieDll_FormatMessage0    SbieDll_FormatMessage0;
    P_SbieDll_FormatMessage1    SbieDll_FormatMessage1;
    P_SbieDll_FormatMessage2    SbieDll_FormatMessage2;
    P_SbieDll_TranslateNtToDosPath SbieDll_TranslateNtToDosPath;
};

// ---------------------------------------------------------------------------
// 薄封装组（M1；NTSTATUS 一律折叠为 SbieStatus，原始值可读 LastNtStatus()）
// ---------------------------------------------------------------------------

// 最近一次薄封装调用的原始 NTSTATUS（诊断用；非线程安全——本 CLI 串行调用）
LONG LastNtStatus();

// SbieApi_QueryDrvInfo：info_class=0 → SBIE_FEATURE_FLAG_* 特征位
SbieStatus QueryDriverInfo(ULONG infoClass, void* data, ULONG size);
inline SbieStatus QueryFeatureFlags(unsigned long* flags)
{
    return QueryDriverInfo(0, flags, sizeof(*flags));
}

// 枚举 box：allSections=false 只回启用中的 box（SbieApi_EnumBoxesEx 内部
// IsBoxEnabled 过滤）；true 回全部节（含 disabled 与 Template_* 等，调用方
// 需自行甄别——见 Model::BoxRepository）。
SbieStatus EnumBoxes(std::vector<std::wstring>* boxes, bool allSections);

// STATUS_SUCCESS=启用；STATUS_ACCOUNT_RESTRICTION=存在但未启用；其余=不存在
SbieStatus IsBoxEnabled(const std::wstring& box, bool* enabled, bool* exists);

// 两段式 QueryBoxPath；出参为 NT 路径（DOS 化见 Util::NtToDosPath）
SbieStatus QueryBoxPath(const std::wstring& box,
                        std::wstring* fileRoot, std::wstring* regRoot,
                        std::wstring* ipcRoot);

// 枚举沙箱内进程 PID（box 空串 = 全部 box；两段式：先取 count 再取 pids，
// 预留 +128 余量，QSbieAPI 同款行为）
SbieStatus EnumBoxProcesses(const std::wstring& box, bool allSessions,
                            std::vector<ULONG>* pids);

struct ProcQuery {
    std::wstring box, image, sid;
    ULONG sessionId = 0;
    ULONG64 createTime = 0;  // PsGetProcessCreateTimeQuadPart：100ns 单位，1601 纪元
};

// SbieApi_QueryProcessEx2（image 缓冲 MAX_PATH）
SbieStatus QueryProcessById(ULONG pid, ProcQuery* out);

// 驱动缓存读配置（04 §2.4 ConfigStore 语义的最小直连版）。
// section/setting 名超 64 WCHAR 会被驱动截断（x_section[66]）——调用方保证。
SbieStatus QueryConfText(const std::wstring& section, const std::wstring& setting,
                         ULONG index, bool noExpand, bool noTemplates,
                         std::wstring* value);
// index 递增到不存在，收集全部值
SbieStatus QueryConfList(const std::wstring& section, const std::wstring& setting,
                         bool noExpand, bool noTemplates,
                         std::vector<std::wstring>* values);
// 枚举节名（section=setting=NULL | CONF_GET_NO_TEMPLS|CONF_GET_NO_EXPAND）
SbieStatus EnumConfSections(std::vector<std::wstring>* sections);

// SbieApi_ReloadConf（session_id=-1 当前会话；flags 见 SBIE_CONF_FLAG_*）
SbieStatus ReloadConf(unsigned long flags, bool reconfigure);

// API_DISABLE_FORCE_PROCESS（02 §2 表 +12；P1-1）。参数编排对齐
// sbieapi.c SbieApi_DisableForceProcess（:916-940）：set/get 各可空，同帧可
// 先读旧值再写新值。语义（drv session.c Session_Api_DisableForce）：
//   * set 非 0 = 自此刻起禁用强制沙箱 ForceDisableSeconds 秒（配置键
//     GlobalSettings\ForceDisableSeconds，缺省 10；=0 永不允许禁用；
//     ForceDisableAdminOnly=y 时需管理员——STATUS_ACCESS_DENIED）；
//   * set = 0 = 恢复（清除时间戳）；
//   * get 回填 Session_IsForceDisabled（BOOLEAN，非剩余秒数——剩余时间由
//     调用方按 ForceDisableSeconds 与禁用时刻自行推算）。
// 失败时 oldState 被 SbieDll 置 FALSE（sbieapi.c 同款）。
SbieStatus DisableForceProcess(ULONG* newState, ULONG* oldState);

// ---------------------------------------------------------------------------
// 监控/trace（波 D1，docs/04 §20；02 §3.6）
// ---------------------------------------------------------------------------

// API_MONITOR_GET2 的取回结果分类（驱动端 session.c Session_Api_MonitorGet2）
enum class MonitorFetch {
    Ok,         // 条目已写入缓冲（bufferLen>0；moreEntries=true 时积压未尽）
    Empty,      // STATUS_NO_MORE_ENTRIES：会话监控环空
    NotEnabled, // STATUS_DEVICE_NOT_READY：本会话 monitor_log 未分配（未开监控）
    Error,      // 其他失败（沙箱内调用 NOT_IMPLEMENTED / 驱动消失…）
};

// API_MONITOR_CONTROL：本会话监控总开关（非沙箱限定；MonitorAdminOnly=y 时
// set=1 需管理员）。newState/oldState 各可空；get 回填 monitor_log 是否在场。
// 开启时驱动按 GlobalSettings\TraceBufferPages（缺省 256 页）分配环形缓冲。
SbieStatus MonitorControl(ULONG* newState, ULONG* oldState);

// API_MONITOR_GET2 批量拉取本会话监控环（02 §7 坑 1：SbieDll 无包装导出，
// 经 SbieApi_Ioctl 直投 API_MONITOR_GET2_ARGS{buffer_ptr, buffer_len}）。
//   * 请求 *bufferLen = 缓冲容量（字节），返回 *bufferLen = 实际写入量；
//   * 缓冲布局（QSbieAPI GetMonitor 同款，SbieAPI.cpp:3075-3121）：
//     连续记录 [ULONG size][记录体 size 字节]…以 [ULONG 0] 结尾；记录体 =
//     [LONGLONG 时间戳 8][ULONG type 4][ULONG pid 4][ULONG tid 4]
//     [若干 \0 结尾 WCHAR 串][可选 0xFFFF 栈标签区]——解码见 Model/Monitor.h；
//   * 缓冲必须 8 字节对齐（建议 256*4096，QSbieAPI 同容量）；
//   * 调用方须非沙箱（02 §6）。原始 NTSTATUS 可读 LastNtStatus()。
MonitorFetch MonitorGet2(void* buffer8Aligned, ULONG* bufferLen, bool* moreEntries);

// Sandboxie 安装目录（NT/DOS 两式；缓冲 512）
SbieStatus GetHomePath(std::wstring* ntPath, std::wstring* dosPath);

// ---------------------------------------------------------------------------
// 诊断辅助（additive）
// ---------------------------------------------------------------------------

// LoadSbieDll 失败原因（含缺失导出名/尝试过的路径）；成功为空
std::wstring LastLoadError();
// 实际加载的 DLL 全路径
std::wstring LoadedFrom();
// abi == kExpectedAbi（0x57230）；不匹配时管理类命令应拒绝（02 §1），
// 本 M1 仅只读查询，调用方在 stderr 提示
bool AbiMatches();

} // namespace sbie::drv
