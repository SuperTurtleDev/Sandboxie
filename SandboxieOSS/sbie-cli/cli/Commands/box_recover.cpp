// Sandboxie-OSS — sbie-cli/cli/Commands/box_recover.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box recover list|copy|add（04 §4.3；06 §P0-12 文件恢复）。
// 三级命令树：路由层分派到 group=box / sub=recover，第三级 token 在本
// handler 内解析（box_snapshot.cpp 同款）。
//
//   list <name>                     列可恢复文件（# / 沙箱相对路径 / 恢复目标 /
//                                   大小；索引供 copy 用）
//   copy <name> <index|all|path…>   拷出到原位或 --to DIR（拷贝语义、保留
//                                   mtime；目标存在默认报错，--overwrite 覆盖）
//   add <name> <folder>             追加 RecoverFolder 配置值
//
// 架构（docs/04 §14）：真实文件 IO 全部在 server 侧完成（"server 负责所有
// 真实操作"模型；无 server 时 client 直连 Model 等价执行）。copy 的选择子
// （index/path）在 client 侧先经 recover.list 解析为沙箱绝对路径再提交
// recover.copy——索引稳定性两侧同序（同一 List 排序）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/Boxes.h"
#include "Model/ConfigStore.h"
#include "Model/Recovery.h"
#include "Util/TablePrinter.h"

#include <cwchar>

namespace sbie::cli {

namespace {

const wchar_t* kRecoverUsage =
    L"usage: sbie-cli box recover <list|copy|add> <name> [...]";

// 位置参数收集（第三级起）：跳过值取型 flag（--to/--password/…）的跟随
// token（坑 §8.19 同款——否则 "--to D:\out" 会把 D:\out 并进选择子）
std::vector<std::wstring> RecoverPositional(const std::vector<std::wstring>& args)
{
    static const wchar_t* const kValueFlags[] = {
        L"--to", L"--password",
    };
    std::vector<std::wstring> out;
    for (size_t i = 3; i < args.size(); ++i) {
        bool valueFlag = false;
        for (const wchar_t* f : kValueFlags)
            if (args[i] == f) {
                valueFlag = true;
                break;
            }
        if (valueFlag) {
            ++i;
            continue;
        }
        if (!args[i].empty() && args[i][0] != L'-')
            out.push_back(args[i]);
    }
    return out;
}

// recover list 的行（两路径归一：IPC data 行 / Model RecoverEntry）
struct RecoverRow {
    std::wstring sandboxPath, boxPath, targetPath;
    unsigned long long size = 0;
};

int RenderRecoverRows(const GlobalOptions& o, const json::JsonValue& data)
{
    util::TablePrinter t;
    t.AddColumn(L"#", true);
    t.AddColumn(L"PATH");
    t.AddColumn(L"TARGET");
    t.AddColumn(L"SIZE", true);
    size_t i = 0;
    for (const json::JsonValue& row : data.items()) {
        const json::JsonValue* idx = row.find(L"index");
        wchar_t no[16];
        if (idx && idx->isInt())
            swprintf_s(no, L"%lld", idx->asInt());
        else
            swprintf_s(no, L"%llu", (unsigned long long)i);
        std::wstring sizeCell;
        if (const json::JsonValue* h = row.find(L"size_human");
            h && h->isString())
            sizeCell = h->asString();
        else if (const json::JsonValue* s = row.find(L"size"); s && s->isInt())
            sizeCell = boxproc::FormatHumanBytes(
                (unsigned long long)s->asInt());
        t.AddRow({ no,
                   ipcroute::CellOf(row.find(L"box_path")),
                   ipcroute::CellOf(row.find(L"target_path")),
                   sizeCell });
        ++i;
    }
    EmitRows(o, t, data, L"no files to recover");
    return 0;
}

// IPC data（对象数组）→ 行集（copy 的选择子解析用）
std::vector<RecoverRow> ParseRecoverRows(const json::JsonValue& data)
{
    std::vector<RecoverRow> rows;
    if (!data.isArray())
        return rows;
    for (const json::JsonValue& row : data.items()) {
        if (!row.isObject())
            continue;
        RecoverRow r;
        if (const json::JsonValue* v = row.find(L"sandbox_path");
            v && v->isString())
            r.sandboxPath = v->asString();
        if (const json::JsonValue* v = row.find(L"box_path");
            v && v->isString())
            r.boxPath = v->asString();
        if (const json::JsonValue* v = row.find(L"target_path");
            v && v->isString())
            r.targetPath = v->asString();
        if (const json::JsonValue* v = row.find(L"size"); v && v->isInt())
            r.size = (unsigned long long)v->asInt();
        rows.push_back(std::move(r));
    }
    return rows;
}

json::JsonValue RecoverRowsJson(const std::vector<model::RecoverEntry>& es)
{
    json::JsonValue rows = json::JsonValue::Array();
    for (size_t i = 0; i < es.size(); ++i) {
        const model::RecoverEntry& e = es[i];
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"index", json::JsonValue((long long)i));
        r.set(L"sandbox_path", json::JsonValue(e.sandboxPath));
        r.set(L"box_path", json::JsonValue(e.boxPath));
        r.set(L"target_path", json::JsonValue(e.targetPath));
        r.set(L"size", json::JsonValue((long long)e.size));
        r.set(L"size_human",
              json::JsonValue(boxproc::FormatHumanBytes(e.size)));
        rows.pushBack(std::move(r));
    }
    return rows;
}

// copy 成功消息（两路径一致）
std::wstring RecoverCopyMessage(const model::RecoverCopyOutcome& o)
{
    return std::to_wstring(o.copiedFiles) + L" file(s) recovered ("
           + boxproc::FormatHumanBytes(o.copiedBytes) + L")";
}

// copy 失败 → EmitError（目标已存在附 --overwrite 提示；两路径一致）
int EmitRecoverCopyError(const GlobalOptions& opts,
                         const model::RecoverCopyOutcome& o, SbieStatus st)
{
    std::wstring msg = L"recover copy failed at: " + o.failedPath;
    if (o.win32Error == ERROR_FILE_EXISTS)
        msg += L" (target exists; use --overwrite to replace)";
    else if (o.win32Error == ERROR_SHARING_VIOLATION)
        msg += L" (source or target is in use)";
    else if (o.win32Error == ERROR_ACCESS_DENIED)
        msg += L" (access denied)";
    return EmitError(opts, st, msg);
}

} // namespace

// ---------------------------------------------------------------------------
// sbie box recover <verb> <name> [...]
// ---------------------------------------------------------------------------

int CmdBoxRecover(const CommandContext& ctx)
{
    if (!boxproc::DriverAliveOrError(ctx.opts))
        return ToExitCode(SbieStatus::DRIVER_UNAVAILABLE);

    if (ctx.args.size() < 3)
        return EmitError(ctx.opts, SbieStatus::USAGE, kRecoverUsage);
    const std::wstring verb = ctx.args[2];
    if (verb != L"list" && verb != L"copy" && verb != L"add")
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown recover subcommand: " + verb);

    std::vector<std::wstring> pos = RecoverPositional(ctx.args);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE, kRecoverUsage);
    const std::wstring& name = pos[0];

    // ---- list ---------------------------------------------------------------
    if (verb == L"list") {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::Result r = ipcroute::Invoke(ctx.opts, ipc::kOpRecoverList,
                                              params, true, RenderRecoverRows);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;

        srvconn::NoteDegraded();
        model::BoxInfo bi;
        if (model::BoxRepository(nullptr, svc::SvcClient::Instance())
                .GetInfo(name, &bi)
            != SbieStatus::OK)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"box '" + name + L"' not found");
        return RenderRecoverRows(ctx.opts,
                                 RecoverRowsJson(
                                     model::RecoveryManager(bi).List()));
    }

    // ---- add ----------------------------------------------------------------
    if (verb == L"add") {
        if (pos.size() < 2)
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"usage: sbie-cli box recover add <name> "
                             L"<folder>");
        const std::wstring& folder = pos[1];
        const std::wstring pw = boxproc::ResolvePasswordArgs(ctx.opts,
                                                             ctx.args);

        // 写路径（非幂等，retry=false；password = 锁配置写凭据）
        {
            json::JsonValue params = json::JsonValue::Object();
            ipcroute::PSet(&params, L"name", name);
            ipcroute::PSet(&params, L"folder", folder);
            if (!pw.empty())
                ipcroute::PSet(&params, L"password", pw);
            ipcroute::Result r = ipcroute::Invoke(
                ctx.opts, ipc::kOpRecoverAdd, params, false,
                [](const GlobalOptions& o, const json::JsonValue& data) {
                    if (o.json) {
                        EmitJsonOk(o, data);   // {value,message}
                        return 0;
                    }
                    const json::JsonValue* m = data.isObject()
                        ? data.find(L"message") : nullptr;
                    EmitMessage(o, m && m->isString() ? m->asString()
                                                      : std::wstring(
                                                            L"recover folder"
                                                            L" added"));
                    return 0;
                });
            if (r.verdict == ipcroute::Verdict::Handled)
                return r.exitCode;
        }

        // 直连 + 写后回读（§13.1 范式；与 server HRecoverAdd 同款）
        model::BoxInfo bi;
        if (model::BoxRepository(nullptr, svc::SvcClient::Instance())
                .GetInfo(name, &bi)
            != SbieStatus::OK)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"box '" + name + L"' not found");
        model::ConfigStore cfg;
        std::vector<std::wstring> before =
            cfg.GetList(name, L"RecoverFolder", false, false);
        SbieStatus st = model::RecoverAddFolder(name, folder, pw);
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"recover add failed"
                             + std::wstring(boxproc::PasswordHint(st, pw)));
        std::vector<std::wstring> after =
            cfg.GetList(name, L"RecoverFolder", false, false);
        if (after.size() <= before.size())
            return EmitError(ctx.opts, SbieStatus::GENERIC,
                             L"recover add verification failed: RecoverFolder"
                             L" count did not increase after write");
        EmitMessage(ctx.opts,
                    L"recover folder added: " + after.back()
                        + L" (" + folder + L")");
        return 0;
    }

    // ---- copy ---------------------------------------------------------------
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box recover copy <name> "
                         L"<index|all|path...> [--to <dir>] [--overwrite]");

    // 行集获取（IPC 优先；降级直连）。copy 语义需要 index/path 解析，两路径
    // 都先取列表（IPC 模式 = recover.list 读 op）
    std::vector<RecoverRow> rows;
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpRecoverList, params, true,
            [&rows](const GlobalOptions&, const json::JsonValue& data) {
                rows = ParseRecoverRows(data);
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled) {
            if (r.exitCode != 0)
                return r.exitCode;   // 错误已渲染（box 不存在等）
        } else {
            srvconn::NoteDegraded();
            model::BoxInfo bi;
            if (model::BoxRepository(nullptr, svc::SvcClient::Instance())
                    .GetInfo(name, &bi)
                != SbieStatus::OK)
                return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                                 L"box '" + name + L"' not found");
            for (const model::RecoverEntry& e :
                 model::RecoveryManager(bi).List())
                rows.push_back(
                    { e.sandboxPath, e.boxPath, e.targetPath, e.size });
        }
    }
    if (rows.empty())
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"no recoverable files in box '" + name + L"'");

    // 选择子解析 → 沙箱绝对路径集
    std::vector<std::wstring> paths;
    bool badSelector = false;
    std::wstring badWhat;
    for (size_t i = 1; i < pos.size() && !badSelector; ++i) {
        const std::wstring& sel = pos[i];
        if (_wcsicmp(sel.c_str(), L"all") == 0) {
            for (const RecoverRow& r : rows)
                paths.push_back(r.sandboxPath);
            continue;
        }
        ULONG idx = 0;
        if (boxproc::ParseUlong(sel, &idx)) {
            if (idx >= rows.size()) {
                badSelector = true;
                badWhat = L"index " + sel + L" out of range ("
                          + std::to_wstring(rows.size()) + L" file(s))";
                break;
            }
            paths.push_back(rows[idx].sandboxPath);
            continue;
        }
        // 路径选择子：对沙箱相对/绝对路径的后缀匹配（\ 归一、大小写不敏感）
        std::wstring norm = sel;
        for (wchar_t& c : norm)
            if (c == L'/')
                c = L'\\';
        size_t hits = 0;
        for (const RecoverRow& r : rows) {
            const bool hitAbs = r.sandboxPath.size() >= norm.size()
                && _wcsicmp(r.sandboxPath.c_str()
                                + (r.sandboxPath.size() - norm.size()),
                            norm.c_str())
                       == 0
                && (norm.empty() || norm[0] == L'\\'
                    || r.sandboxPath.size() == norm.size()
                    || r.sandboxPath[r.sandboxPath.size() - norm.size() - 1]
                           == L'\\');
            const bool hitRel = !hitAbs && r.boxPath.size() >= norm.size()
                && _wcsicmp(r.boxPath.c_str()
                                + (r.boxPath.size() - norm.size()),
                            norm.c_str())
                       == 0
                && (norm.empty() || norm[0] == L'\\'
                    || r.boxPath.size() == norm.size()
                    || r.boxPath[r.boxPath.size() - norm.size() - 1]
                           == L'\\');
            if (hitAbs || hitRel) {
                paths.push_back(r.sandboxPath);
                ++hits;
            }
        }
        if (hits == 0) {
            badSelector = true;
            badWhat = L"no recoverable file matches: " + sel;
        }
    }
    if (badSelector)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND, badWhat);
    if (paths.empty())
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"nothing selected (use <index|all|path...>)");

    const std::wstring toDir = boxproc::OptionValue(ctx.args, L"--to");
    const bool overwrite = boxproc::HasFlag(ctx.args, L"--overwrite");

    // 写路径（非幂等，retry=false；真实文件 IO 在 server 侧——docs/04 §14）
    {
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        json::JsonValue arr = json::JsonValue::Array();
        for (const std::wstring& p : paths)
            arr.pushBack(json::JsonValue(p));
        params.set(L"paths", std::move(arr));
        if (!toDir.empty())
            ipcroute::PSet(&params, L"to", toDir);
        ipcroute::PSet(&params, L"overwrite", overwrite);
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, ipc::kOpRecoverCopy, params, false,
            [](const GlobalOptions& o, const json::JsonValue& data) {
                if (o.json) {
                    EmitJsonOk(o, data);   // {copied,bytes,message}
                    return 0;
                }
                const json::JsonValue* m = data.isObject()
                    ? data.find(L"message") : nullptr;
                EmitMessage(o, m && m->isString()
                              ? m->asString()
                              : std::wstring(L"recovered"));
                return 0;
            });
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    // 直连：Model 等价执行
    model::BoxInfo bi;
    if (model::BoxRepository(nullptr, svc::SvcClient::Instance())
            .GetInfo(name, &bi)
        != SbieStatus::OK)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"box '" + name + L"' not found");
    model::RecoveryManager rm(bi);
    model::RecoverCopyOutcome outcome;
    SbieStatus st = rm.Copy(paths, toDir, overwrite, &outcome);
    if (st != SbieStatus::OK)
        return EmitRecoverCopyError(ctx.opts, outcome, st);

    if (ctx.opts.json) {
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"copied", json::JsonValue((long long)outcome.copiedFiles));
        d.set(L"bytes", json::JsonValue((long long)outcome.copiedBytes));
        d.set(L"message", json::JsonValue(RecoverCopyMessage(outcome)));
        EmitJsonOk(ctx.opts, d);
    } else {
        EmitMessage(ctx.opts, RecoverCopyMessage(outcome));
    }
    return 0;
}

void RegisterBoxRecoverCommands()
{
    Commands()["box"]["recover"] = CmdBoxRecover;
}

} // namespace sbie::cli
