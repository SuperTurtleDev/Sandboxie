// Sandboxie-OSS — sbie-cli/cli/Output.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 输出格式化（契约：04-modules.md §2.6/§7）：TablePrinter（默认人类可读）
// 与 --json 二选一；JSON 由 Util/Json 生成。stdout 只放数据/JSON，
// 诊断一律 stderr（§7.2）。

#pragma once

#include "Cli.h"
#include "Util/Json.h"
#include "Util/TablePrinter.h"
#include "Util/Status.h"

#include <string>
#include <vector>

namespace sbie::cli {

// 表格/JSON 双轨输出：rows 为与表列同序的 JSON 对象数组（snake_case 字段名）。
// 空结果：默认输出 "（no rows）"（或 emptyText），--json 输出 []。退出码由
// 调用方决定（一般 0）。
void EmitRows(const GlobalOptions& opts, const util::TablePrinter& table,
              const json::JsonValue& rows, const wchar_t* emptyText);

// 键值行（box info/proc info 类）：pairs 表（NAME VALUE）或 JSON 对象
void EmitKv(const GlobalOptions& opts, const std::vector<std::pair<std::wstring,
                  std::wstring>>& pairs, const json::JsonValue& object);

// 单值文本（box get/cfg get 类）：默认原样一行；--json 输出 {"value": …}
void EmitValue(const GlobalOptions& opts, const std::wstring& text,
               const json::JsonValue& value);

// 成功消息（"box 'X' created" 类）；--json 输出 {"ok":true,"data":{"message":…}}
void EmitMessage(const GlobalOptions& opts, const std::wstring& text);

// 失败：--json → stdout {"ok":false,"error":{code,message[,ntstatus]}}；
// 默认 → stderr "sbie-cli: <message>"。返回应设置的退出码（= code）。
int EmitError(const GlobalOptions& opts, SbieStatus code,
              const std::wstring& message, const wchar_t* ntstatusHex = nullptr);

// 顶层成功 JSON（status/version 这类非行集命令自己组装 data 后调用）
void EmitJsonOk(const GlobalOptions& opts, const json::JsonValue& data);

// stderr 诊断行（不影响 stdout 纯净性）
void Diag(const std::wstring& message);

} // namespace sbie::cli
