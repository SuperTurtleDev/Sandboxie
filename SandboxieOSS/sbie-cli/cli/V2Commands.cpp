// Sandboxie-OSS — sbie-cli/cli/V2Commands.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 五命令实现（docs/10-v2-design.md §7/§9）。
//
// exec 状态机（§7.2/§7.3 全状态迁移）：
//   L=running.lock R=注册可见 P=盒进程数>0
//   S0 无锁未注册：放锁→注册→等R→启动→任务→monitor
//   S2 有锁未注册：等R（他人注册中）；超时 STATE_TIMEOUT
//   S1/S6 无锁已注册：有缓存=S1 接管（放锁直启）；无缓存=S6 注销尾巴（等注册消失）
//   S3/S4 有锁已注册：冲突检查通过后直接启动
//   孤儿锁（无任务无进程）：探测 R 后走 S3 或清锁走 S0（收养）
//
// 启动通道：SbieSvc ProcessServer RunSandboxed（V2 保留的唯一 SbieSvc 运行期
// 依赖，03-svc-protocol §4）。
// exec 退出语义（R1，V1 docs/04 §8.18 同款）：spawn 成功即退（rc=0）；
// 显式 --wait 才等待子进程并透传其退出码；--detach 为历史别名（=默认）。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"
#include "../monitor/MonitorMain.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/V2/V2Cache.h"
#include "../../SbieCore/Model/V2/V2Common.h"
#include "../../SbieCore/Model/V2/V2Registry.h"
#include "../../SbieCore/Model/V2/V2Task.h"
#include "../../SbieCore/Model/V2/V2Template.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace sbie::cli {

using namespace sbie::model::v2;

namespace {

// V2Err → 退出码 + EmitError（模板类错误折叠 11；其余按码）
int Fail(const GlobalOptions& opts, const V2Err& e)
{
    SbieStatus code = e.code;
    if (IsTemplateError(e))
        code = SbieStatus::TEMPLATE_ERROR;
    return EmitError(opts, code, e.msg.empty() ? std::wstring(StatusName(code)) : e.msg);
}

std::wstring CurrentExePath()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)));
    return n ? std::wstring(buf, n) : std::wstring();
}

// 启动后的公共尾巴：任务文件 + 公共 monitor 拉起
V2Err AfterStart(const BoxTarget& t)
{
    TaskEntry task;
    task.box = t.box;
    task.boxPath = t.boxDir;
    task.cache = CachePathFor(t.box);
    task.alias = t.alias;
    task.creatorPid = GetCurrentProcessId();
    task.created = NowIsoTimestamp();
    V2Err e = WriteTask(task);
    if (!e.Ok())
        return e;
    if (!monitor::EnsureMonitorRunning(CurrentExePath()))
        return {SbieStatus::GENERIC,
                L"started, but failed to launch the session monitor "
                L"(box will not auto-unregister when empty)"};
    return {};
}

// exec 失败回收：不留半注册（锁+缓存+reload）
void CleanupAfterSpawnFailure(const BoxTarget& t)
{
    DeleteTask(t.box);
    DeleteLock(t.boxDir);
    UnregisterBox(t.box, false);
}


// ---------------------------------------------------------------------------
// 认证辅助（R2/R3）：--password 旗标 > SBIE_PASS 环境变量 > 交互提示。
// WRONG_PASSWORD 经 FromNtStatus 折叠为 ACCESS_DENIED——据此触发自动交互
// （无 --interactive 参数，用户拍板）：stdin 为 tty 时回显关闭读一次并重试；
// 非 tty（管道/重定向）直接失败并指引 --password/SBIE_PASS。认证只发生在
// spawn 前的注册阶段，不影响 exec 立即退出语义。
// ---------------------------------------------------------------------------

std::wstring EffectivePassword(const GlobalOptions& opts)
{
    if (!opts.password.empty())
        return opts.password;
    wchar_t env[128];
    DWORD n = GetEnvironmentVariableW(L"SBIE_PASS", env,
                                      (DWORD)(sizeof(env) / sizeof(wchar_t)));
    if (n > 0 && n < sizeof(env) / sizeof(wchar_t))
        return std::wstring(env, n);
    return L"";
}

// tty 下回显关闭读一行密码（成功返回 true 并去尾部 CR/LF）
bool ReadPasswordFromTty(std::wstring* out)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn == INVALID_HANDLE_VALUE || !GetConsoleMode(hIn, &mode))
        return false;   // 非 tty（管道/重定向）
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const wchar_t prompt[] = L"config password: ";
    DWORD w = 0;
    WriteConsoleW(hOut, prompt, (DWORD)wcslen(prompt), &w, nullptr);
    SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);
    wchar_t buf[128];
    DWORD got = 0;
    BOOL ok = ReadConsoleW(hIn, buf, 127, &got, nullptr);
    SetConsoleMode(hIn, mode);
    WriteConsoleW(hOut, L"\r\n", 2, &w, nullptr);
    if (!ok || got == 0)
        return false;
    std::wstring pw(buf, got);
    while (!pw.empty() && (pw.back() == L'\r' || pw.back() == L'\n'))
        pw.pop_back();
    if (pw.empty())
        return false;
    *out = std::move(pw);
    return true;
}

// 注册 + 自动交互认证：ACCESS_DENIED 且未提供密码 → tty 提示重试一次。
V2Err RegisterBoxWithAuth(const std::wstring& box, const std::wstring& boxDir,
                          const GlobalOptions& opts,
                          std::vector<std::wstring>* applied = nullptr)
{
    std::wstring pw = EffectivePassword(opts);
    V2Err e = RegisterBox(box, boxDir, applied, pw);
    if (e.Ok() || !pw.empty())
        return e;
    if (e.code != SbieStatus::ACCESS_DENIED)
        return e;
    std::wstring entered;
    if (!ReadPasswordFromTty(&entered))
        return {SbieStatus::ACCESS_DENIED,
                e.msg + L" (no password provided; non-interactive stdin - use"
                        L" --password <pw> or the SBIE_PASS environment"
                        L" variable)"};
    return RegisterBox(box, boxDir, applied, entered);
}

// 命令行 → 单串。两种形态：
//   * 多参数：逐 token 拼接（含空格的 token 单独加引号）——
//     exec box cmd /c "ping -n 45 127.0.0.1"
//   * 单参数且含空格：视为完整命令行原样透传（不加引号，避免把可执行名
//     连同参数整体引起来导致 CreateProcess 找不到程序）——
//     exec box "cmd /c ping -n 45 127.0.0.1"
std::wstring BuildCommandLine(const std::vector<std::wstring>& args)
{
    if (args.size() == 1 && args[0].find(L' ') != std::wstring::npos)
        return args[0];
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            cmd += L" ";
        const std::wstring& a = args[i];
        bool needQuote = a.find(L' ') != std::wstring::npos
                         || a.find(L'\t') != std::wstring::npos;
        if (needQuote && (a.empty() || a.front() != L'"'))
            cmd += L"\"" + a + L"\"";
        else
            cmd += a;
    }
    return cmd;
}

} // namespace

// ---------------------------------------------------------------------------
// exec
// ---------------------------------------------------------------------------

int CmdExec(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    std::vector<std::wstring> rest;   // cmdline
    // --wait 由全局解析器置 opts.execWait（显式分支拦截，不进 positional）
    bool waitChild = opts.execWait;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--wait")
            waitChild = true;   // 冗余防线（直接调用 ctx 的场景）
        else if (ctx.args[i] == L"--detach")
            ;   // 历史别名：立即退出已是默认（R1），解析以兼容旧脚本
        else
            rest.push_back(ctx.args[i]);
    }
    if (ctx.args.size() < 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli exec PATH\\TO\\box|*alias [cmdline] [--wait]");
    const std::wstring target = ctx.args[1];

    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    // ---- 沙盒内执行（矩阵用例：进入沙盒后 `exec ./ cmd.exe`）----
    // 决策（记录）：沙盒内 cwd 是虚拟路径（drive\C\... 视图），宿主侧
    // sandbox.ini 不可见、ReloadConf/注册协议也拒绝沙盒调用者——因此
    // 盒内 exec 不做宿主解析/注册，直接以"自身盒"为目标：驱动
    // QueryProcess(self) 反推盒名，RunSandboxed 由 SbieSvc ProcessServer
    // 按"调用方在盒内 ⇒ 同盒启动"语义（ProcessServer.cpp CallerInSandbox
    // 分支，BoxNameOrModelPid=-pid）执行。锁/任务/monitor 由盒外首个
    // exec 的生命周期管理，盒内仅追加进程。
    if (drv::InSandbox()) {
        drv::ProcQuery self;
        if (drv::QueryProcessById(GetCurrentProcessId(), &self)
            != SbieStatus::OK)
            return EmitError(opts, SbieStatus::ACCESS_DENIED,
                             L"inside sandbox but cannot query own box");
        const std::wstring cmdline2 = rest.empty() ? L"cmd.exe"
                                                   : BuildCommandLine(rest);
        svc::RunResult rr;
        SbieStatus rs = svc::SvcClient::Instance().RunSandboxed(
            self.box, cmdline2, L"", 0, &rr);
        if (rs != SbieStatus::OK)
            return EmitError(opts, SbieStatus::SPAWN_FAILED,
                             L"RunSandboxed failed in box '" + self.box
                                 + L"': " + StatusName(rs) + L" (win32 "
                                 + std::to_wstring(
                                       svc::SvcClient::LastRunSandboxedWin32())
                                 + L")");
        if (opts.json) {
            json::JsonValue data = json::JsonValue::Object();
            data.set(L"box", json::JsonValue(self.box));
            data.set(L"pid", json::JsonValue((long long)rr.pid));
            EmitJsonOk(opts, data);
        } else if (!opts.quiet) {
            util::PrintLineUtf8(util::WideToUtf8(
                L"started pid " + std::to_wstring(rr.pid) + L" in box "
                + self.box + L" (from inside)"));
        }
        if (rr.hProcess)
            CloseHandle(rr.hProcess);
        if (!waitChild)
            return 0;   // R1：spawn 成功即退（rc=0）
        DWORD code = 0;
        // --wait：句柄已在上方关闭，按 pid 重开等待并透传
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, rr.pid);
        if (hProc) {
            WaitForSingleObject(hProc, INFINITE);
            GetExitCodeProcess(hProc, &code);
            CloseHandle(hProc);
        }
        return (int)code;
    }

    BoxTarget t = ResolveTarget(target);
    if (!t.status.Ok())
        return Fail(opts, t.status);

    const std::wstring cmdline = rest.empty() ? L"cmd.exe" : BuildCommandLine(rest);

    // ---- 状态机（§7.3）----
    bool started = false;
    bool s6Healed = false;   // S6 幽灵节 heal（单次 ReloadConf）已做？
    DWORD waited = 0;
    for (;;) {
        RegState st;
        V2Err e = ProbeRegistration(t.box, &st);
        if (!e.Ok())
            return Fail(opts, e);
        const bool registered = st != RegState::Absent;
        const bool lock = LockExists(t.boxDir);

        if (lock) {
            if (!registered) {
                // S2：他人正在注册（或孤儿锁且注册已被清）；等待后重探
            } else {
                std::wstring owner;
                if (CacheBelongsToOtherDir(t.box, t.boxDir, &owner))
                    return Fail(opts,
                                {SbieStatus::INVALID,
                                 L"box name '" + t.box
                                     + L"' already used by another directory ("
                                     + owner + L")"});
                if (!CacheExists(t.box)) {
                    // 注册在但缓存缺失：幂等重生成（等价内联 sync-config）
                    V2Err re = RegisterBoxWithAuth(t.box, t.boxDir, opts);
                    if (!re.Ok())
                        return Fail(opts, re);
                }
                // S3/S4 判定：盒内有活进程 = 真"运行中直接启动"（S4）；无
                // 进程 = 上一代刚结束、monitor teardown 尚未摘锁的濒死窗口
                // ——此时直接 spawn 会落进低级注入结算期静默夭折（实测子进
                // 程退出码 4）。濒死判据：排除盒自举服务（SandboxieRpcSs/
                // DcomLaunch——用户进程全灭后仍存活数秒）后计数为 0。改为
                // 循环重探：或用户进程复起（并发 exec 抢先，走 S4）或锁被
                // teardown 摘除（回 S0，由墓碑机制确定性退避）。
                bool anyUser = false;
                {
                    std::vector<ULONG> pids;
                    if (drv::EnumBoxProcesses(t.box, true, &pids)
                        == SbieStatus::OK) {
                        for (ULONG pid : pids) {
                            drv::ProcQuery pq;
                            if (drv::QueryProcessById(pid, &pq) != SbieStatus::OK)
                                continue;   // 瞬态：保守视为活
                            if (_wcsicmp(pq.image.c_str(), L"SandboxieRpcSs.exe") == 0
                                || _wcsicmp(pq.image.c_str(),
                                            L"SandboxieDcomLaunch.exe") == 0)
                                continue;   // 盒自举服务：不阻直启判定
                            anyUser = true;
                            break;
                        }
                    } else {
                        anyUser = true;   // 枚举失败：保守直启（原行为）
                    }
                }
                if (anyUser) {
                    started = true;
                    break;
                }
                // 濒死窗口：等待重探（teardown 通常 ~1s 内摘锁）
            }
        } else {
            if (registered) {
                if (CacheExists(t.box) && !TaskExists(t.box)) {
                    // S1：静止已注册（register 后未跑 / 长闲）→ 接管：放锁
                    // （幂等刷新缓存）直启。TaskExists 时为 teardown 摘锁后
                    // 尚未删缓存的濒死窗口——不当接管，循环重探走 S6→S0。
                    std::wstring owner;
                    if (CacheBelongsToOtherDir(t.box, t.boxDir, &owner))
                        return Fail(opts,
                                    {SbieStatus::INVALID,
                                     L"box name '" + t.box
                                         + L"' already used by another"
                                           L" directory (" + owner + L")"});
                    V2Err re = RegisterBoxWithAuth(t.box, t.boxDir, opts);
                    if (!re.Ok())
                        return Fail(opts, re);
                    started = true;
                    break;
                }
                // S6：无锁有注册无缓存。两种可能：(a) monitor/手动 unregister
                // 正在途（删缓存→reload→等注册消失）——只需等待；(b) 驱动
                // 内存里的"幽灵节"（缓存文件已删但 reload 未发生，如上次
                // 注册失败残留）——等不来，需主动 heal：触发一次 ReloadConf
                // 重扫缓存目录（目录=注册集合的真理源），幽灵即散。
                if (!s6Healed) {
                    s6Healed = true;
                    V2Err re = ReloadDriverConf();
                    if (!re.Ok())
                        Diag(L"warning: S6 heal reload failed: " + re.msg);
                }
                // 等注册消失后回 S0
            } else {
                // S0：放锁 → 注册 → 启动。
                // R1：不再前置墓碑结算等待（阻塞 exec 返回）——改为 spawn 后
                // 300ms 探活的反应式自愈（见 CmdExec 尾部）。
                // B3：放锁失败（并发者已抢先创建 tmp/锁）不 Fail——落回循环
                // 按 S2/S3 语义等待重探（teardown 窗口 5 并发原仅 1-2 成功）。
                LockInfo li;
                li.box = t.box;
                li.alias = t.alias;
                li.boxPath = t.boxDir;
                li.cache = CachePathFor(t.box);
                li.creatorPid = GetCurrentProcessId();
                li.created = NowIsoTimestamp();
                V2Err le = WriteLock(t.boxDir, li);
                if (!le.Ok()) {
                    Diag(L"lock busy (concurrent registrar); retrying");
                    // 落回循环（消耗 --wait 预算）
                } else {
                    V2Err re = RegisterBoxWithAuth(t.box, t.boxDir, opts);
                    if (!re.Ok()) {
                        DeleteLock(t.boxDir);   // 不留半注册
                        return Fail(opts, re);
                    }
                    started = true;
                    break;
                }
            }
        }
        if (waited >= opts.waitMs)
            return EmitError(opts, SbieStatus::STATE_TIMEOUT,
                             L"box '" + t.box + L"' registration state did not settle within "
                                 + std::to_wstring(opts.waitMs / 1000) + L"s");
        Sleep(200);
        waited += 200;
    }
    (void)started;

    // ---- 预热：任务文件 + 公共 monitor（先于 spawn）----
    // 竞态修复（实测）：盒内进程启动要求本会话存在活的 session leader
    // （SbieDll init → epmapper/actkernel 链依赖），OSS dist 中 leader =
    // 公共 monitor。顺序：先写任务文件（令 monitor 驻留而非空表 1s 退出）
    // → 确保 monitor 在跑（含 leader 获取）→ 最后 RunSandboxed。spawn 失败
    // 由 CleanupAfterSpawnFailure 统一回收任务/锁/注册。
    V2Err ae = AfterStart(t);
    if (!ae.Ok())
        Diag(L"warning: " + ae.msg);

    // ---- 启动（SbieSvc RunSandboxed）----
    svc::RunResult rr;
    SbieStatus rs = svc::SvcClient::Instance().RunSandboxed(
        t.box, cmdline, L"", 0, &rr);
    if (rs != SbieStatus::OK) {
        CleanupAfterSpawnFailure(t);
        return EmitError(opts, SbieStatus::SPAWN_FAILED,
                         L"RunSandboxed failed for '" + t.box + L"': "
                             + StatusName(rs) + L" (win32 "
                             + std::to_wstring(svc::SvcClient::LastRunSandboxedWin32())
                             + L")");
    }

    // 输出与等待
    if (opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"box", json::JsonValue(t.box));
        data.set(L"pid", json::JsonValue((long long)rr.pid));
        data.set(L"cmdline", json::JsonValue(cmdline));
        EmitJsonOk(opts, data);
    } else if (!opts.quiet) {
        util::PrintLineUtf8(util::WideToUtf8(
            L"started pid " + std::to_wstring(rr.pid) + L" in box " + t.box));
    }

    // ---- R1：默认 spawn 成功即退（rc=0）----
    // 反应式结算自愈（替代前置墓碑等待，R1 决策）：spawn 后 300ms 探活，
    // 子进程已死且盒内无用户进程 = 落入上一代注销的注入结算窗口（实测
    // 2-6s 静默夭折，退出码 4/127）→ 结算 4s 后重 spawn 一次。首启/健康
    // 路径仅多 300ms 探活，不阻塞返回语义。
    {
        DWORD early = 0;
        if (rr.hProcess
            && WaitForSingleObject(rr.hProcess, 300) == WAIT_OBJECT_0) {
            GetExitCodeProcess(rr.hProcess, &early);
            V2Err ce;
            size_t user = BoxUserProcessCount(t.box, &ce);
            if ((early == 4 || early == 127) && ce.Ok() && user == 0) {
                Diag(L"early child death (code "
                     + std::to_wstring(early)
                     + L") - injection settle window; respawning once");
                Sleep(4000);
                svc::RunResult rr2;
                SbieStatus rs2 = svc::SvcClient::Instance().RunSandboxed(
                    t.box, cmdline, L"", 0, &rr2);
                if (rs2 == SbieStatus::OK) {
                    if (rr2.hProcess)
                        CloseHandle(rr2.hProcess);
                    if (opts.json) {
                        json::JsonValue data = json::JsonValue::Object();
                        data.set(L"box", json::JsonValue(t.box));
                        data.set(L"pid", json::JsonValue((long long)rr2.pid));
                        data.set(L"respawned", json::JsonValue(true));
                        EmitJsonOk(opts, data);
                    } else if (!opts.quiet) {
                        util::PrintLineUtf8(util::WideToUtf8(
                            L"respawned pid " + std::to_wstring(rr2.pid)
                            + L" in box " + t.box));
                    }
                    if (!waitChild)
                        return 0;
                    DWORD code2 = 0;
                    HANDLE h2 = OpenProcess(
                        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, rr2.pid);
                    if (h2) {
                        WaitForSingleObject(h2, INFINITE);
                        GetExitCodeProcess(h2, &code2);
                        CloseHandle(h2);
                    }
                    return (int)code2;
                }
            }
        }
    }
    if (rr.hProcess)
        CloseHandle(rr.hProcess);
    if (!waitChild)
        return 0;   // R1：spawn 成功即退（rc=0）
    DWORD code = 0;
    {
        HANDLE hProc = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rr.pid);
        if (hProc) {
            WaitForSingleObject(hProc, INFINITE);
            GetExitCodeProcess(hProc, &code);
            CloseHandle(hProc);
        }
    }
    return (int)code;
}

// ---------------------------------------------------------------------------
// register / unregister / sync-config
// ---------------------------------------------------------------------------

int CmdRegister(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (ctx.args.size() < 2 || ctx.args.size() > 3)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli register PATH\\TO\\box [alias]");
    BoxTarget t = ResolveTarget(ctx.args[1]);
    if (!t.status.Ok())
        return Fail(opts, t.status);
    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    std::vector<std::wstring> applied;
    V2Err e = RegisterBoxWithAuth(t.box, t.boxDir, opts, &applied);
    if (!e.Ok())
        return Fail(opts, e);

    std::wstring alias = t.alias;
    if (ctx.args.size() == 3)
        alias = ctx.args[2];
    if (!alias.empty()) {
        e = AliasSet(alias, t.box, t.boxDir);
        if (!e.Ok())
            Diag(L"warning: alias not saved: " + e.msg);
    }
    if (opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"box", json::JsonValue(t.box));
        data.set(L"box_path", json::JsonValue(t.boxDir));
        if (!alias.empty())
            data.set(L"alias", json::JsonValue(alias));
        EmitJsonOk(opts, data);
    } else {
        EmitMessage(opts, L"registered box '" + t.box + L"' (" + t.boxDir + L")"
                    + (alias.empty() ? L"" : L" alias *" + alias));
    }
    return 0;
}

int CmdUnregister(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (ctx.args.size() < 2 || ctx.args.size() > 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli unregister PATH\\TO\\box|*alias");
    BoxTarget t = ResolveTarget(ctx.args[1]);
    if (!t.status.Ok())
        return Fail(opts, t.status);
    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    // §6.2 顺序：任务 → 盒忙检查 → 锁 → 缓存+reload → alias
    DeleteTask(t.box);
    V2Err e = UnregisterBox(t.box, true /*busyCheck*/);
    if (!e.Ok())
        return Fail(opts, e);
    DeleteLock(t.boxDir);
    AliasRemoveForBox(t.box);
    // 墓碑：供下一个 exec 的 S0 路径做结算退避（与 monitor teardown 同款）
    {
        HANDLE h = CreateFileW((MonitorsDir() + L"\\" + t.box + L".dead").c_str(),
                               GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }

    EmitMessage(opts, L"unregistered box '" + t.box + L"'");
    return 0;
}

int CmdSyncConfig(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (ctx.args.size() != 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli sync-config PATH\\TO\\box|*alias");
    BoxTarget t = ResolveTarget(ctx.args[1]);
    if (!t.status.Ok())
        return Fail(opts, t.status);
    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    V2Err ce;
    size_t n = BoxProcessCount(t.box, &ce);
    if (ce.Ok() && n > 0)
        Diag(L"warning: box has " + std::to_wstring(n)
             + L" running process(es); reload affects new processes");
    V2Err e = SyncBoxConfig(t.box, t.boxDir, EffectivePassword(opts));
    if (!e.Ok())
        return Fail(opts, e);
    EmitMessage(opts, L"config re-synced for box '" + t.box + L"'");
    return 0;
}

// ---------------------------------------------------------------------------
// ps
// ---------------------------------------------------------------------------

int CmdPs(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");

    if (ctx.args.size() >= 2) {
        // 单盒进程明细
        BoxTarget t = ResolveTarget(ctx.args[1]);
        if (!t.status.Ok())
            return Fail(opts, t.status);
        std::vector<ULONG> pids;
        SbieStatus s = drv::EnumBoxProcesses(t.box, true, &pids);
        if (s != SbieStatus::OK)
            return EmitError(opts, s, L"cannot enumerate processes of box " + t.box);
        util::TablePrinter tab;
        tab.AddColumn(L"PID", true);
        tab.AddColumn(L"IMAGE");
        tab.AddColumn(L"SESSION", true);
        tab.AddColumn(L"STARTED");
        json::JsonValue rows = json::JsonValue::Array();
        for (ULONG pid : pids) {
            drv::ProcQuery pq;
            if (drv::QueryProcessById(pid, &pq) != SbieStatus::OK)
                continue;
            SYSTEMTIME st;
            FILETIME ft;
            ft.dwLowDateTime = (DWORD)(pq.createTime & 0xFFFFFFFFull);
            ft.dwHighDateTime = (DWORD)(pq.createTime >> 32);
            FileTimeToSystemTime(&ft, &st);
            wchar_t ts[40];
            _snwprintf_s(ts, _TRUNCATE, L"%02u:%02u:%02u", st.wHour, st.wMinute,
                         st.wSecond);
            tab.AddRow({std::to_wstring(pid), pq.image,
                        std::to_wstring(pq.sessionId), ts});
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"pid", json::JsonValue((long long)pid));
            r.set(L"image", json::JsonValue(pq.image));
            r.set(L"session", json::JsonValue((long long)pq.sessionId));
            r.set(L"started", json::JsonValue(ts));
            rows.pushBack(std::move(r));
        }
        EmitRows(opts, tab, rows, L"no processes");
        return 0;
    }

    // 无参：全部 V2 盒总览（boxes 目录 = 注册集合）
    std::vector<AliasEntry> aliases = AliasList();
    util::TablePrinter tab;
    tab.AddColumn(L"BOX");
    tab.AddColumn(L"STATE");
    tab.AddColumn(L"PROCS", true);
    tab.AddColumn(L"LOCK");
    tab.AddColumn(L"TASK");
    tab.AddColumn(L"ALIAS");
    tab.AddColumn(L"DIRECTORY");
    json::JsonValue rows = json::JsonValue::Array();

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((BoxesDir() + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::wstring box(fd.cFileName);
            size_t dot = box.rfind(L'.');
            if (dot != std::wstring::npos)
                box.resize(dot);
            if (!ValidateBoxName(box).Ok())
                continue;

            RegState st = RegState::Absent;
            ProbeRegistration(box, &st);
            const wchar_t* state = st == RegState::Registered       ? L"registered"
                                   : st == RegState::RegisteredDisabled ? L"disabled"
                                                                        : L"absent";
            V2Err ce;
            size_t procs = (st == RegState::Absent) ? 0 : BoxProcessCount(box, &ce);

            // 盒目录：缓存内 FileRootPath（原始未展开值）
            std::wstring dir;
            {
                IniFileData ini;
                if (ParseIniFile(CachePathFor(box), &ini).Ok()) {
                    if (const IniSectionData* sec = ini.Find(box)) {
                        for (const auto& kv : sec->entries)
                            if (_wcsicmp(kv.key.c_str(), L"FileRootPath") == 0)
                                dir = kv.value;
                    }
                }
            }
            bool lock = !dir.empty() && PathExists(dir + L"\\running.lock");
            bool task = TaskExists(box);
            std::wstring alias;
            for (const auto& a : aliases)
                if (_wcsicmp(a.box.c_str(), box.c_str()) == 0)
                    alias = a.alias;

            tab.AddRow({box, state, ce.Ok() ? std::to_wstring(procs) : L"?",
                        lock ? L"yes" : L"-", task ? L"yes" : L"-",
                        alias.empty() ? L"-" : L"*" + alias, dir.empty() ? L"-" : dir});
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"box", json::JsonValue(box));
            r.set(L"state", json::JsonValue(state));
            r.set(L"procs", json::JsonValue((long long)(ce.Ok() ? procs : -1)));
            r.set(L"lock", json::JsonValue(lock));
            r.set(L"task", json::JsonValue(task));
            if (!alias.empty())
                r.set(L"alias", json::JsonValue(alias));
            if (!dir.empty())
                r.set(L"box_path", json::JsonValue(dir));
            rows.pushBack(std::move(r));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    EmitRows(opts, tab, rows, L"no registered v2 boxes");
    return 0;
}

// ---------------------------------------------------------------------------
// kill-box / kill（SvcClient 进程消息族：KillAll / KillOne）
// ---------------------------------------------------------------------------

int CmdKillBox(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (ctx.args.size() != 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli kill-box PATH\\TO\\box|*alias");
    BoxTarget t = ResolveTarget(ctx.args[1]);
    if (!t.status.Ok())
        return Fail(opts, t.status);
    SbieStatus s = svc::SvcClient::Instance().KillAll(t.box, (ULONG)-1);
    if (s == SbieStatus::NOT_FOUND)
        return EmitError(opts, SbieStatus::NOT_FOUND, L"box '" + t.box + L"' has no processes");
    if (s != SbieStatus::OK)
        return EmitError(opts, s, L"KillAll failed for '" + t.box + L"'");
    // 语义提示：杀空进程会触发 monitor 归零边沿 → 自动注销；随后 exec 会
    // 走"等注册消失 → 放锁 → 注册"重注册路径（§7 状态机 S6→S0）。
    EmitMessage(opts, L"killed all processes in box '" + t.box
                    + L"' (monitor will auto-unregister when it observes empty)");
    return 0;
}

int CmdKill(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    if (ctx.args.size() != 2)
        return EmitError(opts, SbieStatus::USAGE, L"usage: sbie-cli kill <PID>");
    const std::wstring& a = ctx.args[1];
    for (wchar_t c : a)
        if (!iswdigit(c))
            return EmitError(opts, SbieStatus::USAGE, L"kill expects a numeric PID");
    ULONG pid = (ULONG)wcstoul(a.c_str(), nullptr, 10);
    if (!pid)
        return EmitError(opts, SbieStatus::USAGE, L"kill expects a numeric PID");

    // PID 须属于某个沙盒（驱动 QueryProcess；非盒内 PID = NOT_FOUND）
    if (!drv::LoadSbieDll(opts.sbieDllPath))
        return EmitError(opts, SbieStatus::DRIVER_UNAVAILABLE, L"SbieDll.dll not available");
    drv::ProcQuery pq;
    if (drv::QueryProcessById(pid, &pq) != SbieStatus::OK)
        return EmitError(opts, SbieStatus::NOT_FOUND,
                         L"pid " + a + L" is not a sandboxed process");
    SbieStatus s = svc::SvcClient::Instance().KillOne(pid);
    if (s != SbieStatus::OK)
        return EmitError(opts, s, L"KillOne failed for pid " + a);
    EmitMessage(opts, L"killed pid " + a + L" (box " + pq.box
                    + L"; killing the last process may trigger auto-unregister)");
    return 0;
}

} // namespace sbie::cli
