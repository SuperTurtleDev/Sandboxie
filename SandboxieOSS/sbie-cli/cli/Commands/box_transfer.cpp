// Sandboxie-OSS — sbie-cli/cli/Commands/box_transfer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box copy|export|import（波次 B：07-P1-2/07-P1-4，docs/04 §17）。
//
//   copy <src> <dst> [--content]      整节复制（含 Template= 引用）；
//                                     --content 叠加 FileRoot 目录树拷贝
//   export <name> --to <dir|file.sbx> 导出 = ini 节（box.ini）+ 内容树
//                                     （content\）；--to 以 .sbx/.zip 结尾
//                                     = 自研 zip 归档（store），否则目录形态
//   import <path> --name <name>       导入 = 建节 + 还原内容树；path 是目录
//                                     （目录形态包）或归档文件（自动识别）
//
// 退出码：5=源不存在/目标已存在/包不存在；7=名/包非法；6=锁配置；1=文件错。
// 架构：IPC 优先（box.copy/box.export/box.import），server 缺席降级 client
// 直连 Model 等价执行（§13/§14 范式）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/BoxTransfer.h"
#include "Model/Boxes.h"

#include <cwchar>

namespace sbie::cli {

namespace {

bool EndsWithArchiveExt(const std::wstring& path)
{
    const size_t n = path.size();
    auto eq = [&](const wchar_t* ext) {
        const size_t m = wcslen(ext);
        return n >= m + 1 && path[n - m - 1] == L'.'
               && _wcsicmp(path.c_str() + (n - m), ext) == 0;
    };
    return eq(L"sbx") || eq(L"zip");
}

// 路径是已有目录？（导入侧形态识别：目录 = 目录形态包，文件 = zip 归档）
bool PathIsDirectory(const std::wstring& path)
{
    const DWORD at = GetFileAttributesW(path.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// transfer data（files/bytes）→ 表格行（export/import 成功面）
int RenderTransferResult(const GlobalOptions& o, const json::JsonValue& data)
{
    if (o.json) {
        EmitJsonOk(o, data);
        return 0;
    }
    const json::JsonValue* m = data.isObject() ? data.find(L"message")
                                               : nullptr;
    EmitMessage(o, m && m->isString() ? m->asString()
                                      : std::wstring(L"done"));
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// sbie box copy <src> <dst> [--content]
// ---------------------------------------------------------------------------

int CmdBoxCopy(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box copy <src> <dst> [--content]");
    const std::wstring src = pos[0];
    const std::wstring dst = pos[1];
    const bool content = boxproc::HasFlag(ctx.args, L"--content");

    if (src == dst || _wcsicmp(src.c_str(), dst.c_str()) == 0)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"source and destination are the same");
    if (model::BoxRepository::ValidateName(dst) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid box name '" + dst + L"' (max 38 chars of "
                         L"A-Z a-z 0-9 _; reserved words excluded)");

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    // 写路径（非幂等，retry=false；box.copy）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"src", src);
        ipcroute::PSet(&params, L"dst", dst);
        ipcroute::PSet(&params, L"content", content);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxCopy, params, false,
            RenderTransferResult);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware）：与 server HBoxCopy 同序
    model::TransferStats stats;
    SbieStatus st = model::CopyBox(src, dst, content, pw, &stats);
    if (st == SbieStatus::NOT_FOUND)
        return EmitError(ctx.opts, st,
                         L"box '" + src + L"' not found, or '" + dst
                         + L"' already exists");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box copy failed"
                         + std::wstring(boxproc::PasswordHint(st, pw)));

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        if (content) {
            d.set(L"files", json::JsonValue((long long)stats.files));
            d.set(L"bytes", json::JsonValue((long long)stats.bytes));
        }
        d.set(L"message",
              json::JsonValue(L"box '" + src + L"' copied to '" + dst + L"'"
                              + (content
                                     ? L" (" + std::to_wstring(stats.files)
                                           + L" file(s), "
                                           + boxproc::FormatHumanBytes(
                                                 stats.bytes)
                                           + L")"
                                     : L"")));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts,
                    L"box '" + src + L"' copied to '" + dst + L"'"
                        + (content
                               ? L" (" + std::to_wstring(stats.files)
                                     + L" file(s), "
                                     + boxproc::FormatHumanBytes(stats.bytes)
                                     + L")"
                               : L""));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box export <name> --to <dir|file.sbx>
// ---------------------------------------------------------------------------

int CmdBoxExport(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box export <name> "
                         L"--to <dir|file.sbx>");
    const std::wstring name = pos[0];
    const std::wstring to = boxproc::OptionValue(ctx.args, L"--to");
    if (to.empty())
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"--to <dir|file.sbx> is required");
    const bool archive = EndsWithArchiveExt(to);

    // 读侧语义但产物落盘（导出幂等覆盖——retry=true 与 box.size 同类；
    // 产物重复生成无破坏语义）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"to", to);
        ipcroute::PSet(&params, L"archive", archive);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxExport, params, true,
            RenderTransferResult);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连
    model::TransferStats stats;
    std::wstring detail;
    SbieStatus st = model::ExportBox(name, to, archive, &stats, &detail);
    if (st == SbieStatus::NOT_FOUND)
        return EmitError(ctx.opts, st, L"box '" + name + L"' not found");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box export failed"
                             + (detail.empty() ? L"" : L": " + detail));

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"files", json::JsonValue((long long)stats.files));
        d.set(L"dirs", json::JsonValue((long long)stats.dirs));
        d.set(L"bytes", json::JsonValue((long long)stats.bytes));
        d.set(L"archive", json::JsonValue(archive));
        d.set(L"message",
              json::JsonValue(L"box '" + name + L"' exported to " + to
                              + L" (" + std::to_wstring(stats.files)
                              + L" file(s), "
                              + boxproc::FormatHumanBytes(stats.bytes)
                              + L")"));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts,
                    L"box '" + name + L"' exported to " + to + L" ("
                        + std::to_wstring(stats.files) + L" file(s), "
                        + boxproc::FormatHumanBytes(stats.bytes) + L")");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sbie box import <path> --name <name>
// ---------------------------------------------------------------------------

int CmdBoxImport(const CommandContext& ctx)
{
    if (!boxproc::LoadDriverOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    std::vector<std::wstring> pos = boxproc::Positional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box import <path> --name <name>");
    const std::wstring path = pos[0];
    const std::wstring name = boxproc::OptionValue(ctx.args, L"--name");
    if (name.empty())
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"--name <new-box-name> is required");
    if (model::BoxRepository::ValidateName(name) != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::INVALID,
                         L"invalid box name '" + name + L"' (max 38 chars of "
                         L"A-Z a-z 0-9 _; reserved words excluded)");
    // 形态识别：既有目录 = 目录形态包；否则按归档文件打开（打不开报坏包）
    const bool archive = !PathIsDirectory(path);

    const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts, ctx.args);
    // 写路径（非幂等，retry=false；box.import）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"path", path);
        ipcroute::PSet(&params, L"name", name);
        ipcroute::PSet(&params, L"archive", archive);
        if (!pw.empty())
            ipcroute::PSet(&params, L"password", pw);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpBoxImport, params, false,
            RenderTransferResult);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连（pw-aware）：与 server HBoxImport 同序
    model::TransferStats stats;
    std::wstring detail;
    SbieStatus st = model::ImportBox(path, name, archive, pw, &stats,
                                     &detail);
    if (st == SbieStatus::NOT_FOUND)
        return EmitError(ctx.opts, st,
                         L"box '" + name + L"' already exists, or package"
                         L" not found: " + path);
    if (st == SbieStatus::INVALID)
        return EmitError(ctx.opts, st,
                         detail.empty() ? L"bad package: " + path : detail);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st,
                         L"box import failed"
                             + (detail.empty() ? L"" : L": " + detail)
                             + boxproc::PasswordHint(st, pw));

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"files", json::JsonValue((long long)stats.files));
        d.set(L"dirs", json::JsonValue((long long)stats.dirs));
        d.set(L"bytes", json::JsonValue((long long)stats.bytes));
        d.set(L"message",
              json::JsonValue(L"box '" + name + L"' imported from " + path
                              + L" (" + std::to_wstring(stats.files)
                              + L" file(s), "
                              + boxproc::FormatHumanBytes(stats.bytes)
                              + L")"));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts,
                    L"box '" + name + L"' imported from " + path + L" ("
                        + std::to_wstring(stats.files) + L" file(s), "
                        + boxproc::FormatHumanBytes(stats.bytes) + L")");
    }
    return 0;
}

void RegisterBoxTransferCommands()
{
    auto& box = Commands()["box"];
    box["copy"] = CmdBoxCopy;
    box["export"] = CmdBoxExport;
    box["import"] = CmdBoxImport;
}

} // namespace sbie::cli
