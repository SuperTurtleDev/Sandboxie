// Sandboxie-OSS — sbie-cli/cli/Output.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "Output.h"
#include "Util/Utf8.h"

#include <cstdio>

namespace sbie::cli {

using sbie::util::WideToUtf8;

void EmitRows(const GlobalOptions& opts, const util::TablePrinter& table,
              const json::JsonValue& rows, const wchar_t* emptyText)
{
    if (opts.json) {
        json::JsonValue top = json::JsonValue::Object();
        top.set(L"ok", json::JsonValue(true));
        top.set(L"data", rows);
        util::PrintLineUtf8(json::SerializeUtf8(top));
        return;
    }
    if (table.Empty()) {
        if (!opts.quiet)
            util::PrintLineUtf8(WideToUtf8(emptyText ? emptyText : L"(no rows)"));
        return;
    }
    util::PrintUtf8(WideToUtf8(table.Render(!opts.quiet)));
}

void EmitKv(const GlobalOptions& opts,
            const std::vector<std::pair<std::wstring, std::wstring>>& pairs,
            const json::JsonValue& object)
{
    if (opts.json) {
        EmitJsonOk(opts, object);
        return;
    }
    util::TablePrinter t;
    t.AddColumn(L"NAME");
    t.AddColumn(L"VALUE");
    for (auto& kv : pairs)
        t.AddRow({ kv.first, kv.second });
    util::PrintUtf8(WideToUtf8(t.Render(!opts.quiet)));
}

void EmitValue(const GlobalOptions& opts, const std::wstring& text,
               const json::JsonValue& value)
{
    if (opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"value", value);
        EmitJsonOk(opts, data);
        return;
    }
    util::PrintLineUtf8(WideToUtf8(text));
}

void EmitMessage(const GlobalOptions& opts, const std::wstring& text)
{
    if (opts.json) {
        json::JsonValue data = json::JsonValue::Object();
        data.set(L"message", json::JsonValue(text));
        EmitJsonOk(opts, data);
        return;
    }
    util::PrintLineUtf8(WideToUtf8(text));
}

int EmitError(const GlobalOptions& opts, SbieStatus code,
              const std::wstring& message, const wchar_t* ntstatusHex)
{
    int exitCode = ToExitCode(code);
    if (opts.json) {
        json::JsonValue err = json::JsonValue::Object();
        err.set(L"code", json::JsonValue((long long)exitCode));
        err.set(L"message", json::JsonValue(message));
        if (ntstatusHex)
            err.set(L"ntstatus", json::JsonValue(ntstatusHex));
        json::JsonValue top = json::JsonValue::Object();
        top.set(L"ok", json::JsonValue(false));
        top.set(L"error", std::move(err));
        util::PrintLineUtf8(json::SerializeUtf8(top));
    } else {
        std::wstring line = L"sbie-cli: " + message;
        if (!opts.quiet)
            line += std::wstring(L" (") + StatusName(code) + L")";
        util::PrintErrLineUtf8(util::WideToUtf8(line));
    }
    return exitCode;
}

void EmitJsonOk(const GlobalOptions& opts, const json::JsonValue& data)
{
    (void)opts;   // 调用点保证 opts.json == true
    json::JsonValue top = json::JsonValue::Object();
    top.set(L"ok", json::JsonValue(true));
    top.set(L"data", data);
    util::PrintLineUtf8(json::SerializeUtf8(top));
}

void Diag(const std::wstring& message)
{
    util::PrintErrLineUtf8(util::WideToUtf8(message));
}

} // namespace sbie::cli
