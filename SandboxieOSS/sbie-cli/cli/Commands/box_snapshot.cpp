// Sandboxie-OSS — sbie-cli/cli/Commands/box_snapshot.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie box snapshot list|take|remove|select|set-info <name> …（04 §4.3）。
// 三级命令树：路由层只分派到 group=box / sub=snapshot，第三级 token 在本
// handler 内解析。全部为纯文件操作（04 §8.3：不经驱动/SbieSvc 写路径；
// take/remove/select 只读驱动查活动进程）。
//
// 退出码：0；5=box/snapshot 不存在；9=box 有活动进程；1=box 未初始化等。

#include "BoxProcCommands.h"
#include "IpcRoute.h"
#include "../../ipcc/SbieIpc.h"

#include "Model/Snapshots.h"
#include "Util/TablePrinter.h"

namespace sbie::cli {

namespace {

const wchar_t* kSnapUsage =
    L"usage: sbie-cli box snapshot <list|take|remove|select|set-info> <name> ...";

// 组 SnapshotManager：先 GetInfo（顺带校验 box 存在 → 5）
SbieStatus MakeSnapMgr(const std::wstring& name, model::SnapshotManager* out)
{
    model::BoxRepository repo(nullptr, svc::SvcClient::Instance());
    model::BoxInfo bi;
    SbieStatus st = repo.GetInfo(name, &bi);
    if (st != SbieStatus::OK)
        return st;
    *out = model::SnapshotManager(bi);
    return SbieStatus::OK;
}

} // namespace

int CmdBoxSnapshot(const CommandContext& ctx)
{
    // args: [box, snapshot, <verb>, <name>, ...]
    if (ctx.args.size() < 3)
        return EmitError(ctx.opts, SbieStatus::USAGE, kSnapUsage);
    const std::wstring verb = ctx.args[2];
    if (verb != L"list" && verb != L"take" && verb != L"remove"
        && verb != L"select" && verb != L"set-info")
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown snapshot subcommand: " + verb);
    std::vector<std::wstring> pos;
    for (size_t i = 3; i < ctx.args.size(); ++i)
        if (ctx.args[i].empty() || ctx.args[i][0] != L'-')
            pos.push_back(ctx.args[i]);
    if (pos.size() < 1)
        return EmitError(ctx.opts, SbieStatus::USAGE, kSnapUsage);

    const std::wstring& name = pos[0];

    // IPC 路由（box.snap.*；本构建 server op 均占位 → 自动降级直连）。
    // list 读路径 retry=true；take/remove/select/set-info 非幂等 retry=false
    //（00 §7）。params schema 按 04 §4.3 命令参数（server 实现该族 op 后
    // 生效）。list 无第二位置参数；其余 verb 的 <snap-name>/<id> 缺失由
    // 各自直连分支报 USAGE（IPC 分支带空 id 时 server 侧自会拒）。
    {
        const bool isList = verb == L"list";
        const bool isTake = verb == L"take";
        const char* op = isList      ? ipc::kOpBoxSnapList
                       : isTake      ? ipc::kOpBoxSnapTake
                       : verb == L"remove"  ? ipc::kOpBoxSnapRemove
                       : verb == L"select"  ? ipc::kOpBoxSnapSelect
                                            : ipc::kOpBoxSnapSetInfo;
        json::JsonValue params = json::JsonValue::Object();
        ipcroute::PSet(&params, L"name", name);
        if (pos.size() >= 2) {
            if (isTake)
                ipcroute::PSet(&params, L"snap_name", pos[1]);
            else
                ipcroute::PSet(&params, L"id", pos[1]);
        }
        if (isTake) {
            const std::wstring info = boxproc::OptionValue(ctx.args, L"--info");
            if (!info.empty())
                ipcroute::PSet(&params, L"info", info);
        }
        if (verb == L"set-info") {
            if (boxproc::HasFlag(ctx.args, L"--name")) {
                const std::wstring v = boxproc::OptionValue(ctx.args, L"--name");
                if (!v.empty())
                    params.set(L"new_name", json::JsonValue(v));
            }
            if (boxproc::HasFlag(ctx.args, L"--info")) {
                const std::wstring v = boxproc::OptionValue(ctx.args, L"--info");
                if (!v.empty())
                    params.set(L"new_info", json::JsonValue(v));
            }
        }
        ipcroute::Result r = ipcroute::Invoke(
            ctx.opts, op, params, isList,
            isList
                ? ipcroute::Renderer([](const GlobalOptions& o,
                                        const json::JsonValue& data) {
                      return ipcroute::RenderRows(
                          o,
                          { { L"ID", L"id", true },
                            { L"NAME", L"name" },
                            { L"DATE", L"date" },
                            { L"CURRENT", L"current" },
                            { L"DEFAULT", L"default" } },
                          data, L"no snapshots");
                  })
                : nullptr);
        if (r.verdict == ipcroute::Verdict::Handled)
            return r.exitCode;
    }

    model::SnapshotManager sm(model::BoxInfo{});
    SbieStatus st = MakeSnapMgr(name, &sm);
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"box '" + name + L"' not found");

    if (verb == L"list") {
        std::wstring cur, def;
        std::vector<model::SnapshotInfo> snaps = sm.List(&cur, &def);
        util::TablePrinter t;
        t.AddColumn(L"ID", true);
        t.AddColumn(L"NAME");
        t.AddColumn(L"DATE");
        t.AddColumn(L"CURRENT");
        t.AddColumn(L"DEFAULT");
        json::JsonValue rows = json::JsonValue::Array();
        for (const model::SnapshotInfo& s : snaps) {
            bool isCur = _wcsicmp(s.id.c_str(), cur.c_str()) == 0;
            bool isDef = _wcsicmp(s.id.c_str(), def.c_str()) == 0;
            t.AddRow({ s.id, s.name, boxproc::FormatUnixSeconds(s.date),
                       isCur ? L"*" : L"", isDef ? L"d" : L"" });
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"id", json::JsonValue(s.id));
            r.set(L"name", json::JsonValue(s.name));
            r.set(L"date", json::JsonValue(boxproc::FormatUnixSeconds(s.date)));
            r.set(L"current", json::JsonValue(isCur));
            r.set(L"default", json::JsonValue(isDef));
            r.set(L"parent_id", json::JsonValue(s.parentId));
            r.set(L"info", json::JsonValue(s.info));
            rows.pushBack(std::move(r));
        }
        EmitRows(ctx.opts, t, rows, L"no snapshots");
        return 0;
    }

    if (verb == L"take") {
        if (pos.size() < 2)
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"usage: sbie-cli box snapshot take <name> "
                             L"<snap-name> [--info <text>]");
        st = sm.Take(pos[1]);
        if (st == SbieStatus::BOX_BUSY)
            return EmitError(ctx.opts, SbieStatus::BOX_BUSY,
                             L"box '" + name + L"' has running processes");
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"cannot take snapshot (box not initialized "
                             L"or file error)");
        // Take 契约不回 ID；读回 Current/Snapshot（即新快照）
        std::wstring cur;
        sm.List(&cur, nullptr);
        // --info <text>：take 后补写 Description（契约无 info 形参）
        std::wstring info = boxproc::OptionValue(ctx.args, L"--info");
        if (!info.empty())
            (void)sm.SetInfo(cur, std::nullopt, info);
        EmitMessage(ctx.opts, L"snapshot #" + cur + L" taken");
        return 0;
    }

    if (verb == L"remove") {
        if (pos.size() < 2)
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"usage: sbie-cli box snapshot remove <name> <id>");
        st = sm.Remove(pos[1]);
        if (st == SbieStatus::BOX_BUSY)
            return EmitError(ctx.opts, SbieStatus::BOX_BUSY,
                             L"box '" + name + L"' has running processes");
        if (st == SbieStatus::NOT_FOUND)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"snapshot '" + pos[1] + L"' not found");
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st,
                             L"cannot remove snapshot (shared parent or "
                             L"file error)");
        EmitMessage(ctx.opts, L"snapshot #" + pos[1] + L" removed");
        return 0;
    }

    if (verb == L"select") {
        if (pos.size() < 2)
            return EmitError(ctx.opts, SbieStatus::USAGE,
                             L"usage: sbie-cli box snapshot select <name> <id>");
        st = sm.Select(pos[1]);
        if (st == SbieStatus::BOX_BUSY)
            return EmitError(ctx.opts, SbieStatus::BOX_BUSY,
                             L"box '" + name + L"' has running processes");
        if (st == SbieStatus::NOT_FOUND)
            return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                             L"snapshot '" + pos[1] + L"' not found");
        if (st != SbieStatus::OK)
            return EmitError(ctx.opts, st, L"snapshot select failed");
        EmitMessage(ctx.opts, L"switched to snapshot #" + pos[1]);
        return 0;
    }

    // set-info
    if (pos.size() < 2)
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"usage: sbie-cli box snapshot set-info <name> <id> "
                         L"[--name <n>] [--info <text>]");
    std::optional<std::wstring> newName, newInfo;
    if (boxproc::HasFlag(ctx.args, L"--name"))
        newName = boxproc::OptionValue(ctx.args, L"--name");
    if (boxproc::HasFlag(ctx.args, L"--info"))
        newInfo = boxproc::OptionValue(ctx.args, L"--info");
    st = sm.SetInfo(pos[1], newName, newInfo);
    if (st == SbieStatus::NOT_FOUND)
        return EmitError(ctx.opts, SbieStatus::NOT_FOUND,
                         L"snapshot '" + pos[1] + L"' not found");
    if (st != SbieStatus::OK)
        return EmitError(ctx.opts, st, L"snapshot set-info failed");
    boxproc::EmitSilentOk(ctx.opts, L"updated");
    return 0;
}

void RegisterBoxSnapshotCommands()
{
    Commands()["box"]["snapshot"] = CmdBoxSnapshot;
}

} // namespace sbie::cli
