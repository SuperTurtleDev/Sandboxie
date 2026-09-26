// Sandboxie-OSS — sbie-cli/cli/Commands/IpcRoute.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// IpcRoute.h 的非内联实现（接线波次）。

#include "IpcRoute.h"

#include "../../ipcc/SbieIpc.h"

#include <cwchar>

namespace sbie::cli::ipcroute {

namespace {

// 通用成功渲染（写路径 op 的过渡渲染：server 补齐 op 后其 data 约定携带
// message 字段；没有时 --json 原样透传 data、文本模式输出 "ok"）
int RenderGenericOk(const GlobalOptions& opts, const json::JsonValue& data)
{
    const json::JsonValue* m = data.isObject() ? data.find(L"message") : nullptr;
    if (opts.json) {
        EmitJsonOk(opts, data);
        return 0;
    }
    if (m && m->isString() && !m->asString().empty())
        util::PrintLineUtf8(util::WideToUtf8(m->asString()));
    else
        util::PrintLineUtf8("ok");
    return 0;
}

} // namespace

Result Invoke(const GlobalOptions& opts, const char* op,
              const json::JsonValue& params, bool retry,
              const Renderer& render)
{
    Result r;
    // 无连接（--no-server / 预拉起失败）：请求从未送出，直连降级安全
    if (!srvconn::HasServer()) {
        if (opts.showTransport)
            Diag(L"transport: direct (server not connected)");
        return r;
    }

    srvconn::IpcOutcome o = srvconn::Call(op, params, retry);
    if (o.transport && o.ok) {
        if (opts.showTransport)
            Diag(L"transport: ipc");
        r.verdict = Verdict::Handled;
        r.exitCode = render ? render(opts, o.data)
                            : RenderGenericOk(opts, o.data);
        return r;
    }
    if (o.transport && !o.ok) {
        // server 已实现该 op 且给出业务错误 → 权威结论；ERR_NOT_IMPLEMENTED
        // = 占位桩 → 降级直连。
        // 错误信封（server 写路径波次，04 §12）：server 侧 error.code 现为
        // SbieStatus 业务码原样（不再经 ToExitCode 折叠 103→1），故按码值
        // 精确识别；消息标记（"not implemented in this server build" /
        // "unknown op"）保留为旧 server 兼容的第二判据。
        const bool notImplemented =
            o.code == (int)SbieStatus::ERR_NOT_IMPLEMENTED
            || (o.code == (int)SbieStatus::GENERIC
                && (o.message.find(L"not implemented") != std::wstring::npos
                    || o.message.find(L"unknown op") != std::wstring::npos));
        if (notImplemented) {
            if (opts.showTransport)
                Diag(L"transport: direct (server op not implemented)");
            return r;
        }
        if (opts.showTransport)
            Diag(L"transport: ipc");
        r.verdict = Verdict::Handled;
        r.exitCode = EmitError(opts, static_cast<SbieStatus>(o.code),
                               o.message.empty()
                                   ? util::Utf8ToWide(op) + L" failed"
                                   : o.message);
        return r;
    }
    // 传输不可恢复（server 死且重拉失败 / 请求已送出后断裂）
    if (retry) {
        if (opts.showTransport)
            Diag(L"transport: direct (server connection lost)");
        return r;   // 幂等读：请求无副作用，降级直连
    }
    // 非幂等写：请求可能已部分执行（00 §7）——报 8 提示手动重试，不降级
    if (opts.showTransport)
        Diag(L"transport: ipc (connection lost mid-command)");
    r.verdict = Verdict::Handled;
    r.exitCode = EmitError(opts,
                           o.code == (int)SbieStatus::RETRY_SUGGESTED
                               ? SbieStatus::RETRY_SUGGESTED
                               : SbieStatus::SERVER_UNAVAILABLE,
                           o.message);
    return r;
}

int RenderRows(const GlobalOptions& opts, const std::vector<Col>& cols,
               const json::JsonValue& data, const wchar_t* emptyText,
               const std::function<std::vector<std::wstring>(
                   const json::JsonValue& row)>& cells)
{
    util::TablePrinter t;
    for (const Col& c : cols)
        t.AddColumn(c.header, c.rightAlign);
    for (const json::JsonValue& row : data.items()) {
        if (cells) {
            t.AddRow(cells(row));
            continue;
        }
        std::vector<std::wstring> line;
        line.reserve(cols.size());
        for (const Col& c : cols)
            line.push_back(CellOf(row.isObject() ? row.find(c.key) : nullptr));
        t.AddRow(std::move(line));
    }
    EmitRows(opts, t, data.isArray() ? data : json::JsonValue::Array(),
             emptyText);
    return 0;
}

int RenderKv(const GlobalOptions& opts,
             const std::vector<std::pair<std::wstring, const wchar_t*>>& keys,
             const json::JsonValue& data)
{
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    for (const auto& k : keys)
        kv.emplace_back(k.first,
                        CellOf(data.isObject() ? data.find(k.second) : nullptr));
    EmitKv(opts, kv, data.isObject() ? data : json::JsonValue::Object());
    return 0;
}

} // namespace sbie::cli::ipcroute
