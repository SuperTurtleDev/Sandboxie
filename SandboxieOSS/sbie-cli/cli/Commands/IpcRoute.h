// Sandboxie-OSS — sbie-cli/cli/Commands/IpcRoute.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 接线波次共享助手：命令 handler 的 "IPC 优先 → 降级直连" 路由
//（00-architecture §5/§7 + server agent 接线说明）。
//
// 语义（核实 ServerConnect::Call 后固化；server 写路径波次更新，04 §12）：
//   * Run() 已在路由前 EnsureConnected——正常路径下连接就绪；
//   * srvconn::Call(op, params, retry)：读命令 retry=true（server 死自动
//     重拉一次；再失败=请求从未执行，降级直连安全）；写命令 retry=false
//     （00 §7 非幂等不自动重试；请求已送出后断裂 → 报 RETRY_SUGGESTED=8，
//     由用户手动重试，绝不降级直连以免双写）；
//   * server 写路径 op 已实现（box.create/set/delete/rename/setEnabled/
//     clean/size、box.snap.*、proc.start/kill/killAll/suspend/resume、
//     cfg.set/unset/lock/unlock、tpl.apply/revoke、recover.list/copy/add
//     ——参数对拍表见 04 §12/§13/§14）；仍未实现的 op（tpl.list/info/check）
//     回 ERR_NOT_IMPLEMENTED=103——该错误码表示"请求绝未执行"，一律降级
//     client 直连。
//   * server 明确回复的业务错误（NOT_FOUND/BOX_BUSY/ACCESS_DENIED…）是
//     权威结论：直接报错退出，不再降级（降级重跑只会得到同错或更差）。
//   * 错误信封：server 侧 error.code = SbieStatus 业务码原样（103 精确可
//     辨；旧 server 兼容保留消息标记第二判据）。
//   * --show-transport（诊断选项）：各判定点 stderr 报告 ipc/direct。
//
// 退出约定：
//   Verdict::Handled  — IPC 已出结果（成功已渲染 / 失败已 EmitError），
//                       handler 返回 result.exitCode；
//   Verdict::Fallback — 走本文件内既有的直连降级路径（调用方在降级路径里
//                       保留 srvconn::NoteDegraded() 提示）。

#pragma once

#include "../Cli.h"
#include "../Output.h"
#include "../ServerConnect.h"

#include "../../../SbieCore/Util/Json.h"
#include "../../../SbieCore/Util/Status.h"
#include "../../../SbieCore/Util/Utf8.h"

#include <cwchar>
#include <functional>
#include <string>
#include <vector>

namespace sbie::cli::ipcroute {

enum class Verdict { Handled, Fallback };

// op 成功时把 data 渲染成表格/JSON（04 §7 双轨）；返回退出码（一般 0）。
using Renderer = std::function<int(const GlobalOptions&, const json::JsonValue&)>;

struct Result {
    Verdict verdict = Verdict::Fallback;
    int exitCode = 0;
};

// 参数便捷组装（params 恒为 JSON 对象）
inline void PSet(json::JsonValue* o, const wchar_t* key, const std::wstring& v)
{
    o->set(key, json::JsonValue(v));
}
inline void PSet(json::JsonValue* o, const wchar_t* key, bool v)
{
    o->set(key, json::JsonValue(v));
}
inline void PSet(json::JsonValue* o, const wchar_t* key, long long v)
{
    o->set(key, json::JsonValue(v));
}

// 核心路由。render 为空 = 通用成功渲染（data.message 文本 / data 原样 JSON）。
Result Invoke(const GlobalOptions& opts, const char* op,
              const json::JsonValue& params, bool retry,
              const Renderer& render);

// ---- 渲染小工具（行集类 op：data = 对象数组） ------------------------------

// 列描述：header = 表头，key = 行对象字段名（snake_case，与 --json 一致）
struct Col {
    const wchar_t* header;
    const wchar_t* key;
    bool rightAlign = false;   // 数字列右对齐（04 §7.1）
};

// JSON 值 → 表格单元格文本：bool → yes/no；int → 十进制；string 原样；
// 缺失/其余 → "-"
inline std::wstring CellOf(const json::JsonValue* v)
{
    if (!v)
        return L"-";
    switch (v->type()) {
    case json::JsonValue::Type::Bool:
        return v->asBool() ? L"yes" : L"no";
    case json::JsonValue::Type::Int: {
        wchar_t buf[24];
        swprintf_s(buf, L"%lld", v->asInt());
        return buf;
    }
    case json::JsonValue::Type::String:
        return v->asString();
    default:
        return L"-";
    }
}

// 行集渲染：表格按 cols 建列（cells 可对个别列做派生，如 image 取基名），
// --json 原样输出 data 数组（字段即 server op 契约字段）。
int RenderRows(const GlobalOptions& opts, const std::vector<Col>& cols,
               const json::JsonValue& data, const wchar_t* emptyText,
               const std::function<std::vector<std::wstring>(
                   const json::JsonValue& row)>& cells = nullptr);

// 键值行渲染（box info / proc info 类）：按 keys 顺序取 data 字段。
int RenderKv(const GlobalOptions& opts,
             const std::vector<std::pair<std::wstring, const wchar_t*>>& keys,
             const json::JsonValue& data);

} // namespace sbie::cli::ipcroute
