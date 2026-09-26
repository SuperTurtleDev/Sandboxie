// Sandboxie-OSS — sbie-cli/server/Dispatcher.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// op → Model 调用 → JSON 的分派框架（契约：04-modules.md §2.6）。
// **每个 op 的 handler 独立函数**，注册表 std::map<std::string, Handler>，
// 新命令只加注册项（Dispatcher.cpp 的 RegisterBuiltinOps）。
//
// server 波次（M2）实现状态：
//   * 读路径 op 已转发 SbieCore：status/version/box.list/box.info/box.get/
//     box.listSetting/proc.list/proc.info/cfg.get/cfg.listSetting/cfg.path/
//     cfg.reload/log.dump/log.watch；
//   * 写路径 op 已实现（server 写路径波次，04 §12）：box.create/set/delete/
//     rename、box.snap.list/take/remove/select/setInfo、proc.start/kill、
//     cfg.set、tpl.apply/revoke——参数名与 client IpcRoute 预接线对拍，
//     SbieSvc 调用一律经 SvcProxy 专职线程（03 §1 线程亲和）；
//   * 补缺波次（06 缺口表收口）：proc.killAll/suspend/resume、
//     box.setEnabled/clean、cfg.unset/lock/unlock 已实现（含写 op 的
//     password 参数接线，P0-11）；
//   * box size / recover 波次（06 §P0-5/P0-12，docs/04 §14）：box.size、
//     recover.list/copy（纯文件，worker 线程）、recover.add（SvcProxy）已
//     实现——recover 的真实文件 IO 在 server 侧（架构决策见 docs/04 §14）；
//   * 仍未实现（占位桩）：tpl.list/info/check（client 直连实现运行良好，
//     audit 判降级可接受，06 §5）——后续波次在 RegisterBuiltinOps 换成真
//     handler 即可；
//   * server.shutdown 在 ServerMain 工作线程内特判（调用方会话校验），
//     不走注册表。

#pragma once

#include "ServerState.h"
#include "../../SbieCore/Util/Json.h"
#include "../../SbieCore/Util/Status.h"

#include <functional>
#include <map>
#include <string>

namespace sbie::server {

// 单个 op 的执行结果（由 ServerMain 工作线程包装为回复帧 envelope）
struct OpResult {
    bool ok = false;
    SbieStatus status = SbieStatus::OK;  // ok=false 时的错误语义
    std::wstring message;                // ok=false 诊断文本
    unsigned long ntstatus = 0;          // 可选原始 NTSTATUS（hex 入 JSON）
    json::JsonValue data = json::JsonValue::Object(); // ok=true 数据（行集类
                                                      // op 为数组，其余对象）

    static OpResult Succeed(json::JsonValue d)
    {
        OpResult r;
        r.ok = true;
        r.data = std::move(d);
        return r;
    }
    static OpResult Fail(SbieStatus s, const std::wstring& msg,
                         unsigned long nt = 0)
    {
        OpResult r;
        r.ok = false;
        r.status = s;
        r.message = msg;
        r.ntstatus = nt;
        return r;
    }
};

// params：请求 JSON 的 "params" 对象（缺省为空对象）；conn：发起连接
//（log.watch 需要登记订阅）。
using Handler = std::function<OpResult(const json::JsonValue& params,
                                       const std::shared_ptr<Connection>& conn)>;

// handler 注册表（进程级）。key = OpName（ipcc/SbieIpc.h 的 kOp* 常量串）。
std::map<std::string, Handler>& Registry();

// 进程启动时调一次（幂等）：登记全部内置 op。
void RegisterBuiltinOps();

// 分派：未知 op → ERR_NOT_IMPLEMENTED。
OpResult Dispatch(const std::string& op, const json::JsonValue& params,
                  const std::shared_ptr<Connection>& conn);

} // namespace sbie::server
