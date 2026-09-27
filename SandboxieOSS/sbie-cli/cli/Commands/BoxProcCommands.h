// Sandboxie-OSS — sbie-cli/cli/Commands/BoxProcCommands.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// box/proc/snapshot/cfg 命令集（第二波：SbieSvc 写路径 + 快照文件操作；
// 规格 docs/04-modules.md §4.3/§4.4/§4.5）。
//
// 注册方式：M1 框架的注册表在 Commands.cpp::RegisterCommands() 于 Run() 时
// 统一赋值（会覆盖静态自注册），故本波次各文件导出 Register* 函数、由接线
// agent 在 RegisterCommands() 之后调用（覆盖同名桩）。各文件不修改任何
// 已存在的注册表文件。
//
// 本文件同时提供本波次命令的共享小工具（header-only）。

#pragma once

#include "../Cli.h"
#include "../Output.h"
#include "../ServerConnect.h"

#include "DriverApi/DriverApi.h"
#include "Model/Boxes.h"
#include "Model/Processes.h"
#include "Util/Status.h"
#include "Util/Utf8.h"

#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace sbie::cli {

// 注册函数清单（接线 agent 在 RegisterCommands() 之后依序调用）
void RegisterBoxCreateCommands();     // box_create.cpp   → box create
                                      //    (--type 预设) + box types
void RegisterBoxManageCommands();     // box_manage.cpp   → box info/get/set/
                                      //    list-setting/rename/delete/
                                      //    enable/disable/clean/size
void RegisterBoxTransferCommands();   // box_transfer.cpp → box copy/export/
                                      //    import（07-P1-2/4）
void RegisterBoxSnapshotCommands();   // box_snapshot.cpp → box snapshot
                                      //    list/take/remove/select/set-info
void RegisterBoxRecoverCommands();    // box_recover.cpp  → box recover
                                      //    list/copy/add（P0-12）
void RegisterProcCommands();          // proc_cmd.cpp     → proc info/start/kill/
                                      //    kill-all(--all)/suspend/resume
void RegisterCfgSetCommands();        // cfg_set.cpp      → cfg set/unset/lock/
                                      //    unlock
void RegisterForceCommands();         // force_cmd.cpp    → force on/off/status
                                      //    （P1-1）
void RegisterMaintCommands();         // maint_cmd.cpp    → maint status/start/
                                      //    stop（P1-2；无 IPC op，机器级）

namespace boxproc {

// 密码解析：--password 选项 > SBIE_PASS 环境变量（04 §3）。
// 组名前缀的 --password 由 Run() 解析进 opts.password；组名之后（命令专属
// 区）的 --password <pw> 由本族命令自行吸收（Positional 跳过其值）。
inline std::wstring ResolvePassword(const GlobalOptions& opts)
{
    if (!opts.password.empty())
        return opts.password;
    wchar_t buf[256];
    size_t n = 0;
    if (_wgetenv_s(&n, buf, L"SBIE_PASS") == 0 && n > 0)
        return std::wstring(buf);
    return std::wstring();
}

// 写命令的 WRONG_PASSWORD 提示（P0-8 连接级缓存语义：unlock 仅验证、
// 密码不缓存——后续写必须逐次携带 --password/SBIE_PASS，06 §P0-8 建议）
inline const wchar_t* PasswordHint(SbieStatus st, const std::wstring& pw)
{
    return (st == SbieStatus::ACCESS_DENIED && pw.empty())
        ? L" (config is locked: pass --password <pw> or set SBIE_PASS)"
        : L"";
}

// 确保驱动绑定可用；失败时输出退出码 3 诊断并返回 false
inline bool LoadDriverOrError(const GlobalOptions& opts)
{
    if (drv::Loaded() || drv::LoadSbieDll(opts.sbieDllPath))
        return true;
    EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE,
              L"SbieDll.dll not available: " + drv::LastLoadError());
    return false;
}

// 驱动在场检查（读类命令的 3 号退出码前提）
inline bool DriverAliveOrError(const GlobalOptions& opts)
{
    if (!LoadDriverOrError(opts))
        return false;
    if (!drv::DriverAlive())
        EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE,
                  L"Sandboxie driver is not running");
    return drv::DriverAlive();
}

// 数字参数解析（完整消费才有效；否则 USAGE）
inline bool ParseUlong(const std::wstring& s, ULONG* out)
{
    if (s.empty())
        return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 10);
    if (!end || *end != L'\0')
        return false;
    *out = (ULONG)v;
    return true;
}

// 位置参数收集：跳过前两个 token（group/sub）后，非 "-" 开头即位置参数；
// 值取型 flag（--index/--dir/--section/…）的跟随 token 不是位置参数
// （坑 §8.19：否则 "cfg set K V --section S" 会把 S 并进 value）
inline std::vector<std::wstring> Positional(const std::vector<std::wstring>& args)
{
    static const wchar_t* const kValueFlags[] = {
        L"--index", L"--dir", L"--info", L"--name", L"--section",
        L"--template", L"--password", L"--sbie-dll-path", L"--to", L"--type",
    };
    std::vector<std::wstring> out;
    for (size_t i = 2; i < args.size(); ++i) {
        bool valueFlag = false;
        for (const wchar_t* f : kValueFlags)
            if (args[i] == f) { valueFlag = true; break; }
        if (valueFlag) {
            ++i;   // 跳过 flag 及其值
            continue;
        }
        if (args[i].empty() || args[i][0] != L'-')
            out.push_back(args[i]);
    }
    return out;
}

// 取值选项：--name <value>（在 args 全体中查找首个；找不到返回 L""）
inline std::wstring OptionValue(const std::vector<std::wstring>& args,
                                const wchar_t* name)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name)
            return args[i + 1];
    return L"";
}

// ResolvePassword 的完整版：优先取命令专属区的尾置 --password <pw>
// （P0-11：boxproc 风格命令此前静默丢弃该形态——直连与 IPC 双路径统一），
// 其次 opts.password（组名前 --password），最后 SBIE_PASS。
inline std::wstring ResolvePasswordArgs(const GlobalOptions& opts,
                                        const std::vector<std::wstring>& args)
{
    const std::wstring trailing = OptionValue(args, L"--password");
    if (!trailing.empty())
        return trailing;
    return ResolvePassword(opts);
}

inline bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* name)
{
    for (const std::wstring& a : args)
        if (a == name)
            return true;
    return false;
}

// unix 秒 → ISO-8601 本地时间（04 §7.2：2026-09-27T12:34:56）
inline std::wstring FormatUnixSeconds(ULONGLONG secs)
{
    if (secs == 0)
        return L"-";
    ULARGE_INTEGER u{};
    u.QuadPart = secs * 10000000ull + 116444736000000000ull;
    FILETIME utc{ u.LowPart, u.HighPart };
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local)
        || !FileTimeToSystemTime(&local, &st))
        return L"-";
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 100ns(1601 纪元) → 本地 "YYYY-MM-DD HH:MM:SS"（proc list 同款）
inline std::wstring FormatCreateTime(ULONG64 t)
{
    if (t == 0)
        return L"-";
    FILETIME utc{ (DWORD)(t & 0xFFFFFFFF), (DWORD)(t >> 32) };
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local)
        || !FileTimeToSystemTime(&local, &st))
        return L"-";
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 静默成功（默认输出 "—" 的命令）：JSON 仍发 ok 信封
inline void EmitSilentOk(const GlobalOptions& opts, const wchar_t* what)
{
    if (!opts.json)
        return;
    json::JsonValue d = json::JsonValue::Object();
    d.set(L"message", json::JsonValue(what));
    EmitJsonOk(opts, d);
}

// 字节数人性化（box size / recover 的 SIZE 列；1024 基，B/KB/MB/GB/TB，
// <10 两位小数、<100 一位、其余取整——资源管理器"大小"列风格）。
// server 侧 Dispatcher.cpp 有同构副本（cli/server 模块隔离，04 §1）
inline std::wstring FormatHumanBytes(unsigned long long bytes)
{
    if (bytes < 1024)
        return std::to_wstring(bytes) + L" B";
    static const wchar_t* const units[] = { L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    size_t u = 0;
    do {
        v /= 1024.0;
        ++u;
    } while (v >= 1024.0 && u < 4);
    wchar_t buf[32];
    if (v < 10.0)
        swprintf_s(buf, L"%.2f %s", v, units[u - 1]);
    else if (v < 100.0)
        swprintf_s(buf, L"%.1f %s", v, units[u - 1]);
    else
        swprintf_s(buf, L"%.0f %s", v, units[u - 1]);
    return buf;
}

} // namespace boxproc
} // namespace sbie::cli
