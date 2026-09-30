// Sandboxie-OSS — SbieCore/Util/Utf8.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "Utf8.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <fcntl.h>

namespace sbie::util {

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        // 含孤立代理时退化为替换输出，保证 CLI 永不崩
        n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
    }
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), nullptr, 0);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0)
            return {};
    }
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

void InitUtf8Console()
{
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

void PrintUtf8(const char* s)
{
    if (s && *s)
        fwrite(s, 1, strlen(s), stdout);
}

void PrintUtf8(const std::string& s)
{
    if (!s.empty())
        fwrite(s.data(), 1, s.size(), stdout);
}

void PrintLineUtf8(const std::string& s)
{
    fwrite(s.data(), 1, s.size(), stdout);
    fputc('\n', stdout);
}

void PrintErrUtf8(const std::string& s)
{
    if (!s.empty())
        fwrite(s.data(), 1, s.size(), stderr);
}

void PrintErrLineUtf8(const std::string& s)
{
    fwrite(s.data(), 1, s.size(), stderr);
    fputc('\n', stderr);
}

} // namespace sbie::util
