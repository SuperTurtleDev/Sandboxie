// Sandboxie-OSS — SbieCore/Util/Utf8.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// UTF-16（内部 wstring）↔ UTF-8（CLI 输出 / JSON / IPC payload）转换。

#pragma once

#include <string>
#include <string_view>

namespace sbie::util {

std::string  WideToUtf8(const std::wstring& s);
std::wstring Utf8ToWide(const std::string& s);

// 控制台 UTF-8 输出初始化：SetConsoleOutputCP(CP_UTF8) + stdout/stderr 置 binary
// （04-modules.md §7.1）。幂等，wmain 开头调用一次。
void InitUtf8Console();

// fwrite UTF-8 到 FILE*（binary 模式下直接写字节）
void PrintUtf8(const char* s);
void PrintUtf8(const std::string& s);
void PrintLineUtf8(const std::string& s);

// stderr 诊断：UTF-8 字节写（stderr 已置 binary，勿用 fwprintf——会写裸 UTF-16）
void PrintErrUtf8(const std::string& s);
void PrintErrLineUtf8(const std::string& s);

} // namespace sbie::util
