// Sandboxie-OSS — SbieCore/Model/Processes.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// M2 登记（坑 docs/04-modules.md §8.17）：Start 的 RunSandboxed 不走
// SvcClient::RunSandboxed 便捷层，而是本文件经 SvcClient::Call 原始组包，
// 两个实测必要条件（任一不满足 → 服务端 CreateProcessAsUser 失败
// ERROR_INVALID_NAME(123)）：
//   1. env 必须是完整环境块（GetEnvironmentStringsW，双 NUL 结尾）；
//   2. dir 必须非空——空串经服务端变成 lpCurrentDirectory=L"" 在 SbieSvc
//      上下文（SetThreadToken 伪装下）必 123；dir 空时以调用方当前目录
//      代入（等价 CreateProcess 的继承 cwd 语义）。
// （本地未伪装复现时空 dir 可被容忍——此差异即首查误导源。）

#include "Processes.h"
#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"

// vendor 协议头（MSGID_PROCESS_RUN_SANDBOXED / PROCESS_RUN_SANDBOXED_REQ）
#include "ProcessWire.h"
#include "msgids.h"

#include <cstdlib>
#include <cstring>

namespace sbie::model {

ProcessRepository::ProcessRepository(sbie::drv::Api* api,
                                     sbie::svc::SvcClient& svc)
    : api_(api), svc_(svc)
{
}

std::vector<ProcEntry> ProcessRepository::Enum(bool allSessions,
                                               const std::wstring& box)
{
    std::vector<ProcEntry> out;
    std::vector<ULONG> pids;
    if (drv::EnumBoxProcesses(box, allSessions, &pids) != SbieStatus::OK)
        return out;
    out.reserve(pids.size());
    for (ULONG pid : pids) {
        drv::ProcQuery q;
        if (drv::QueryProcessById(pid, &q) != SbieStatus::OK)
            continue; // 竞态：进程刚退出
        ProcEntry e;
        e.pid = pid;
        e.box = std::move(q.box);
        e.image = std::move(q.image);
        e.sessionId = q.sessionId;
        e.createTime = q.createTime;
        // SBIE_FLAG_* 位（02 §3.3；QueryProcessInfo 失败=0，与 flags 恰为 0
        // 不可区分——04 §2.4 已注，proc info 命令配合 Ex2 存在性判断）
        if (drv::Loaded() && drv::ApiP()->SbieApi_QueryProcessInfo)
            e.flags = (ULONG)drv::ApiP()->SbieApi_QueryProcessInfo(
                (HANDLE)(ULONG_PTR)pid, 0);
        out.push_back(std::move(e));
    }
    return out;
}

SbieStatus ProcessRepository::Info(ULONG pid, svc::ProcInfo* out)
{
    return svc_.GetProcInfo(pid, 7 /*1|2|4 全量*/, out);
}

SbieStatus ProcessRepository::Kill(ULONG pid)
{
    return svc_.KillOne(pid);
}

SbieStatus ProcessRepository::KillBox(const std::wstring& box)
{
    return svc_.KillAll(box, (ULONG)-1);
}

SbieStatus ProcessRepository::Suspend(ULONG pid)
{
    return svc_.SuspendResume(pid, true);
}

SbieStatus ProcessRepository::Resume(ULONG pid)
{
    return svc_.SuspendResume(pid, false);
}

namespace {

// 完整环境块（双 NUL 结尾；WCHAR 计数含结尾）
std::vector<WCHAR> BuildEnvBlock()
{
    std::vector<WCHAR> out;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        LPWCH p = env;
        while (*p) {
            while (*p)
                out.push_back(*p++);
            out.push_back(L'\0');   // 各串结尾 NUL
            ++p;
        }
        out.push_back(L'\0');       // 块结尾第二 NUL
        FreeEnvironmentStringsW(env);
    }
    if (out.empty())
        out.push_back(L'\0'), out.push_back(L'\0');
    return out;
}

} // namespace

SbieStatus ProcessRepository::Start(const std::wstring& box,
                                    const std::wstring& cmd,
                                    const std::wstring& dir, bool elevated,
                                    svc::RunResult* out)
{
    if (elevated) {
        // 降级路径：SbieDll_RunStartExe /elevated（04 §4.4；02 §3.7）
        if (!drv::Loaded() && !drv::LoadSbieDll())
            return SbieStatus::ERR_SBIEDLL;
        std::wstring full = L"/box:" + box + L" /elevated " + cmd;
        if (!drv::ApiP()->SbieDll_RunStartExe(full.c_str(), box.c_str()))
            return SbieStatus::GENERIC;
        return SbieStatus::OK; // 无句柄可回（RunStartExe 不外露）
    }

    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;

    // 变长区布局（对齐 SbieDll_RunSandboxed，core\dll\callsvc.c:1012-1055）：
    // ofs = 字节偏移（相对结构体起点），len = WCHAR 数（不含结尾 NUL）；
    // 每段后随 NUL。env 为完整环境块；dir 非空（空 → 调用方当前目录）。
    std::vector<WCHAR> env = BuildEnvBlock();
    std::wstring useDir = dir;
    if (useDir.empty()) {
        wchar_t cwd[MAX_PATH + 1] = L"";
        if (GetCurrentDirectoryW(MAX_PATH + 1, cwd))
            useDir = cwd;
        else
            useDir = L"C:\\";
    }
    const size_t cmdChars = cmd.size();
    const size_t dirChars = useDir.size();
    const size_t envChars = env.size() ? env.size() - 1 : 0; // 尾 NUL 单写
    const size_t fixedLen = sizeof(PROCESS_RUN_SANDBOXED_REQ);
    const size_t cmdOfs = fixedLen;
    const size_t dirOfs = cmdOfs + (cmdChars + 1) * sizeof(WCHAR);
    const size_t envOfs = dirOfs + (dirChars + 1) * sizeof(WCHAR);
    const size_t reqLen = envOfs + (envChars + 1) * sizeof(WCHAR);

    PROCESS_RUN_SANDBOXED_REQ* req =
        (PROCESS_RUN_SANDBOXED_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = MSGID_PROCESS_RUN_SANDBOXED;
    req->h.length = (ULONG)reqLen;
    wcsncpy_s(req->boxname, box.c_str(), _TRUNCATE);
    req->cmd_ofs = (ULONG)cmdOfs;
    req->cmd_len = (ULONG)cmdChars;
    req->dir_ofs = (ULONG)dirOfs;
    req->dir_len = (ULONG)dirChars;
    req->env_ofs = (ULONG)envOfs;
    req->env_len = (ULONG)envChars;
    if (cmdChars)
        memcpy((UCHAR*)req + cmdOfs, cmd.c_str(), cmdChars * sizeof(WCHAR));
    memcpy((UCHAR*)req + dirOfs, useDir.c_str(), dirChars * sizeof(WCHAR));
    if (envChars)
        memcpy((UCHAR*)req + envOfs, env.data(), envChars * sizeof(WCHAR));

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = svc_.Call(req, reqLen, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    PROCESS_RUN_SANDBOXED_RPL* r = (PROCESS_RUN_SANDBOXED_RPL*)rpl;
    if (r->h.status != 0) {
        // 该消息的 status 为 win32 错误（03 §4）；折叠为 GENERIC（不外泄裸码）
        free(rpl);
        return SbieStatus::GENERIC;
    }
    out->hProcess = (HANDLE)(ULONG_PTR)r->hProcess;
    out->pid = r->dwProcessId;
    if (r->hThread)
        CloseHandle((HANDLE)(ULONG_PTR)r->hThread); // hThread 本项目暂不留用
    free(rpl);
    return SbieStatus::OK;
}

} // namespace sbie::model
