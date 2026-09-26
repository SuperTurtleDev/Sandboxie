// Sandboxie-OSS — sbie-cli/cli/Commands/maint_cmd.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie maint status|start|stop [--driver|--service|--all]（P1-2，06 缺口表；
// docs/04 §4.8）。
//   * 机器级组件操作（SbieDrv/SbieSvc 全局），不经 sbie-cli server、无 IPC
//     op——client 直接执行（server 会话域 ≠ 组件机器域；且任务边界：server
//     启动路径不做任何组件启停，本命令亦只在显式调用时操作）；
//   * status：SCM 状态（running/stopped/start_pending/.../not_installed）；
//   * start：SbieSvc=进程内 StartService；SbieDrv=KmdUtil.exe start（经
//     SbieDll_RunFromHome 从安装目录拉起）；
//   * stop ：SbieSvc=ControlService(STOP)+等待落定；SbieDrv=KmdUtil stop
//     （KmdUtil 对 SbieDrv 有专门卸载序列）；
//   * --all（缺省）= driver+service；stop 顺序 service→driver（QSbieUtils
//     ::Stop 同序），start 顺序 service→driver。
// 启停需管理员（非提升 rc 6）；未安装组件 rc 5（装卸归 P2-7）。

#include "BoxProcCommands.h"
#include "IpcRoute.h"

#include "Model/Boxes.h"
#include "Model/Maintenance.h"

namespace sbie::cli {

namespace {

enum class Target { Driver, Service, All };

// [--driver|--service|--all] 解析（缺省 All；互斥给出多个 → 取并集语义
// 不做——按 QSbieUtils 位掩码处理：多个旗标等价 --all）
bool ParseTarget(const std::vector<std::wstring>& args, Target* out,
                 std::wstring* err)
{
    bool driver = false, service = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == L"--driver")
            driver = true;
        else if (args[i] == L"--service")
            service = true;
        else if (args[i] == L"--all")
            driver = service = true;
        else {
            *err = args[i];
            return false;
        }
    }
    *out = (driver && service) ? Target::All
         : driver ? Target::Driver
         : service ? Target::Service : Target::All;
    return true;
}

const wchar_t* ComponentLabel(model::Component c)
{
    return c == model::Component::Driver ? L"driver" : L"service";
}

void RenderStatusRow(const GlobalOptions& opts, util::TablePrinter* t,
                     json::JsonValue* rows, model::Component c,
                     const model::ComponentStatus& s, const wchar_t* extra)
{
    if (opts.json) {
        json::JsonValue r = json::JsonValue::Object();
        r.set(L"component", json::JsonValue(ComponentLabel(c)));
        r.set(L"installed", json::JsonValue(s.installed));
        r.set(L"running", json::JsonValue(s.running));
        r.set(L"state", json::JsonValue(s.state));
        rows->pushBack(std::move(r));
        return;
    }
    t->AddRow({ ComponentLabel(c),
                s.running ? L"running" : s.state,
                s.installed ? L"yes" : L"no",
                extra ? extra : L"" });
}

} // namespace

// ---------------------------------------------------------------------------
// sbie maint status [--driver|--service|--all]
// ---------------------------------------------------------------------------

int CmdMaintStatus(const CommandContext& ctx)
{
    Target target = Target::All;
    std::wstring err;
    if (!ParseTarget(ctx.args, &target, &err))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown option: " + err
                         + L"; usage: sbie-cli maint status"
                           L" [--driver|--service|--all]");

    util::TablePrinter t;
    t.AddColumn(L"COMPONENT");
    t.AddColumn(L"STATE");
    t.AddColumn(L"INSTALLED");
    t.AddColumn(L"NOTE");
    json::JsonValue rows = json::JsonValue::Array();

    bool anyError = false;
    auto one = [&](model::Component c, const wchar_t* extra) {
        model::ComponentStatus s;
        SbieStatus st = model::QueryComponent(c, &s);
        if (st != SbieStatus::OK) {
            anyError = true;
            s.state = L"unknown (scm error)";
        }
        RenderStatusRow(ctx.opts, &t, &rows, c, s, extra);
    };

    // 驱动行附 driver-alive 交叉核证（SCM 说 running ≠ 设备必应答）
    if (target != Target::Service) {
        model::ComponentStatus s;
        SbieStatus st = model::QueryComponent(model::Component::Driver, &s);
        if (st != SbieStatus::OK) {
            anyError = true;
            s.state = L"unknown (scm error)";
        }
        if (drv::Loaded() || drv::LoadSbieDll(ctx.opts.sbieDllPath)) {
            const bool alive = drv::DriverAlive();
            if (s.running && !alive)
                s.state += L" (device not responding)";
            RenderStatusRow(ctx.opts, &t, &rows, model::Component::Driver, s,
                            alive ? L"device: alive" : L"device: not open");
        } else {
            RenderStatusRow(ctx.opts, &t, &rows, model::Component::Driver, s,
                            L"device: unknown (no SbieDll)");
        }
    }
    if (target != Target::Driver)
        one(model::Component::Service, L"");

    EmitRows(ctx.opts, t, rows, L"no components");
    return anyError ? ToExitCode(SbieStatus::GENERIC) : 0;
}

// ---------------------------------------------------------------------------
// sbie maint start|stop [--driver|--service|--all]（共用的执行/渲染）
// ---------------------------------------------------------------------------

int RunMaintTransition(const CommandContext& ctx, bool start)
{
    const wchar_t* verb = start ? L"start" : L"stop";
    Target target = Target::All;
    std::wstring err;
    if (!ParseTarget(ctx.args, &target, &err))
        return EmitError(ctx.opts, SbieStatus::USAGE,
                         L"unknown option: " + err
                         + L"; usage: sbie-cli maint " + verb
                         + L" [--driver|--service|--all]");

    // 顺序：stop = service→driver（先停服务再卸驱动）；start = service→driver
    //（QSbieUtils::Start 注：Service 启动会带动驱动；显式双 start 幂等）
    std::vector<model::Component> order;
    if (target != Target::Driver)
        order.push_back(model::Component::Service);
    if (target != Target::Service)
        order.push_back(model::Component::Driver);

    json::JsonValue rows = json::JsonValue::Array();
    util::TablePrinter t;
    t.AddColumn(L"COMPONENT");
    t.AddColumn(L"RESULT");
    t.AddColumn(L"STATE");
    int rc = 0;

    for (model::Component c : order) {
        const wchar_t* name = ComponentLabel(c);
        SbieStatus st = start ? model::StartComponent(c)
                              : model::StopComponent(c);
        // 操作后复核实际状态（报告即事实）
        model::ComponentStatus s;
        (void)model::QueryComponent(c, &s);

        std::wstring result;
        if (st == SbieStatus::OK && s.running == start) {
            result = L"ok";
        } else if (st == SbieStatus::OK) {
            // KmdUtil 路径异步落定或被外部改写：报当前态
            result = s.running ? L"running" : L"stopped";
        } else if (st == SbieStatus::ACCESS_DENIED) {
            result = L"access denied (run as administrator)";
            rc = ToExitCode(st);
        } else if (st == SbieStatus::NOT_FOUND) {
            result = L"not installed";
            rc = ToExitCode(st);
        } else {
            result = L"failed";
            rc = rc ? rc : ToExitCode(st);
        }

        if (ctx.opts.json) {
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"component", json::JsonValue(name));
            r.set(L"result", json::JsonValue(result));
            r.set(L"state", json::JsonValue(s.state));
            r.set(L"running", json::JsonValue(s.running));
            rows.pushBack(std::move(r));
        } else {
            t.AddRow({ name, result, s.state });
        }
    }

    if (ctx.opts.json)
        EmitRows(ctx.opts, t, rows, L"");
    else
        EmitRows(ctx.opts, t, rows, L"no components");
    return rc;
}

int CmdMaintStart(const CommandContext& ctx) { return RunMaintTransition(ctx, true); }
int CmdMaintStop(const CommandContext& ctx)  { return RunMaintTransition(ctx, false); }

void RegisterMaintCommands()
{
    auto& maint = Commands()["maint"];
    maint["status"] = CmdMaintStatus;
    maint["start"] = CmdMaintStart;
    maint["stop"] = CmdMaintStop;
}

} // namespace sbie::cli
