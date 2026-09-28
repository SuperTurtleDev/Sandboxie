// Sandboxie-OSS — sbie-cli/cli/BoxCommands.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// `sbie-cli create-box <dir> [--type <t>]` — 目录化建盒（V2 语义：mkdir +
// sandbox.ini；V1 的 box create --type 等价面）。六型对齐 docs/04 §12 类型
// 映射：hardening / hardened-plus / standard / standard-plus / app / app-plus。
//
// `sbie-cli create-encbox <dir> [--size 1024M|1G|500M] [--password <pw>]`
// — 加密盒：sandbox.ini(UseFileImage=y, FileRootPath=<dir>\data) +
// data.box 容器（SbieSvc IMBOX_CREATE）。密码缺省 R3 自动交互（非 tty 直接
// 失败并指引 --password）。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"
#include "../monitor/MonitorMain.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/V2/V2Common.h"
#include "../../SbieCore/Model/V2/V2EncBox.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"

#include <windows.h>

namespace sbie::cli {

using namespace sbie::model::v2;

namespace {

struct TypeSpec {
    const wchar_t* name;      // --type 值
    const wchar_t* file;      // templates\BoxTypes\<file>
};

// 六型映射（docs/04 §12；standard=仅基础集；键序 hardened-plus 先
// UsePrivacyMode 后 UseSecurityMode（向导落键序，无语义差））
const TypeSpec kTypes[] = {
    {L"hardening",     L"Hardened.ini"},
    {L"hardened-plus", L"HardenedPlus.ini"},
    {L"standard",      L"Standard.ini"},
    {L"standard-plus", L"StandardPlus.ini"},
    {L"app",           L"AppBox.ini"},
    {L"app-plus",      L"AppBoxPlus.ini"},
};

bool ReadPasswordPrompt(const wchar_t* what, std::wstring* out)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn == INVALID_HANDLE_VALUE || !GetConsoleMode(hIn, &mode))
        return false;   // 非 tty
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w = 0;
    WriteConsoleW(hOut, what, (DWORD)wcslen(what), &w, nullptr);
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

} // namespace

int CmdCreateBox(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    std::wstring dir, type = L"standard";
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--type" && i + 1 < ctx.args.size())
            type = ctx.args[++i];
        else
            return EmitError(opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
    }
    if (ctx.args.size() < 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli create-box PATH\\TO\\dir [--type "
                         L"hardening|hardened-plus|standard|standard-plus|"
                         L"app|app-plus]");
    dir = NormalizeDirPath(ctx.args[1]);
    if (!dir.empty() && !PathExists(dir)) {
        // 允许末段不存在（要创建）；父目录必须存在
    } else if (dir.empty()) {
        // 相对路径不存在：父目录判定
        dir = ctx.args[1];
        size_t cut = dir.find_last_of(L'\\');
        std::wstring parent = cut == std::wstring::npos ? L"."
                                                        : dir.substr(0, cut);
        std::wstring base = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
        if (base.empty() || !IsDirectory(NormalizeDirPath(parent)))
            return EmitError(opts, SbieStatus::NOT_FOUND,
                             L"parent directory not found: " + parent);
        dir = NormalizeDirPath(parent) + L"\\" + base;
        if (PathExists(dir + L"\\sandbox.ini"))
            return EmitError(opts, SbieStatus::INVALID,
                             dir + L"\\sandbox.ini already exists");
    }
    const TypeSpec* spec = nullptr;
    for (const auto& t : kTypes)
        if (type == t.name)
            spec = &t;
    if (!spec)
        return EmitError(opts, SbieStatus::USAGE,
                         L"unknown --type: " + type);
    const std::wstring box = BoxNameFromPath(dir);
    V2Err ne = ValidateBoxName(box);
    if (!ne.Ok())
        return EmitError(opts, ne.code, ne.msg);
    if (!CreateDirectoryW(dir.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS)
        return EmitError(opts, SbieStatus::GENERIC, L"cannot create " + dir);
    if (PathExists(dir + L"\\sandbox.ini"))
        return EmitError(opts, SbieStatus::INVALID,
                         dir + L"\\sandbox.ini already exists");
    std::wstring ini;
    ini += L"# v2 box (created by sbie-cli create-box)\n";
    ini += L"[" + box + L"]\n";
    ini += L"Enabled=y\n";
    ini += L"Template=BoxTypes\\" + std::wstring(spec->file).substr(
        0, std::wstring(spec->file).size() - 4) + L"\n";
    V2Err we = WriteTextFileAtomic(dir + L"\\sandbox.ini",
                                   util::WideToUtf8(ini));
    if (!we.Ok())
        return EmitError(opts, SbieStatus::GENERIC, we.msg);
    EmitMessage(opts, L"created box '" + box + L"' (" + dir
                    + L", type " + type + L")");
    return 0;
}

int CmdCreateEncBox(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    std::wstring size = L"1G", password = opts.password;
    bool havePw = !password.empty();
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (ctx.args[i] == L"--size" && i + 1 < ctx.args.size())
            size = ctx.args[++i];
        else if (ctx.args[i] == L"--password" && i + 1 < ctx.args.size()) {
            password = ctx.args[++i];
            havePw = true;
        } else
            return EmitError(opts, SbieStatus::USAGE,
                             L"unknown option: " + ctx.args[i]);
    }
    if (ctx.args.size() < 2)
        return EmitError(opts, SbieStatus::USAGE,
                         L"usage: sbie-cli create-encbox PATH\\TO\\dir "
                         L"[--size 1024M|1G|500M] [--password <pw>]");
    // 密码链（与 exec 挂载统一）：--password > SBIE_BOX_PASSWORD > R3 tty 交互
    //（非 tty 失败）；SBIE_PASS 属 ini EditPassword 体系，不参与
    if (!havePw) {
        wchar_t env[128];
        DWORD n = GetEnvironmentVariableW(L"SBIE_BOX_PASSWORD", env, 128);
        if (n > 0 && n < 128) {
            password = std::wstring(env, n);
            havePw = true;
        }
    }
    if (!havePw) {
        if (!ReadPasswordPrompt(L"box image password: ", &password))
            return EmitError(opts, SbieStatus::ACCESS_DENIED,
                             L"encrypted box requires a password; "
                             L"non-interactive stdin - use --password <pw> or "
                             L"the SBIE_BOX_PASSWORD environment variable");
    }

    const unsigned long long sizeKb = ParseSizeToKb(size);
    if (sizeKb == 0)
        return EmitError(opts, SbieStatus::USAGE,
                         L"bad --size (examples: 500M, 1024M, 1G)");

    // 路径解析：目标目录可能尚不存在（本命令要创建它）——先按已存在
    // 规范化；失败则规范化其父目录再拼末段（与 create-box 同法）。绝不把
    // 相对路径传给 ImBox LPC（服务端 TranslateNtToDos 对 "\??\enc6\data"
    // 类相对 NT 路径失败 → PATH_NOT_FOUND，实测三连复现的根因）。
    std::wstring dir = ctx.args[1];
    {
        std::wstring norm = NormalizeDirPath(dir);
        if (norm.empty()) {
            std::wstring d = dir;
            size_t cut = d.find_last_of(L'\\');
            std::wstring parent = cut == std::wstring::npos ? L"."
                                                            : d.substr(0, cut);
            std::wstring base = cut == std::wstring::npos ? d
                                                          : d.substr(cut + 1);
            std::wstring pn = NormalizeDirPath(parent);
            if (!pn.empty() && !base.empty())
                norm = pn + L"\\" + base;
        }
        if (norm.empty())
            return EmitError(opts, SbieStatus::NOT_FOUND,
                             L"parent directory not found: " + dir);
        dir = norm;
    }
    V2Err e = CreateEncryptedBox(dir, sizeKb, password);
    if (!e.Ok())
        return EmitError(opts, e.code, e.msg);
    EmitMessage(opts, L"created encrypted box '" + BoxNameFromPath(dir)
                    + L"' (" + dir + L"): sandbox.ini + data.box ("
                    + std::to_wstring(sizeKb / 1024) + L"M container)");
    return 0;
}

} // namespace sbie::cli
