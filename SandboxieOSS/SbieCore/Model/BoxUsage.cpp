// Sandboxie-OSS — SbieCore/Model/BoxUsage.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// box size 波次实现（06 §P0-5）：FindFirstFileW 递归累加。
//   * 长路径：>MAX_PATH-12 时加 \\?\ 前缀（FindFirstFileW 文档口径）；
//   * 重解析点（junction/symlink）：跳过——不递归、不计大小不计数（避免
//     环路与双重计数，与资源管理器"大小"列一致）；
//   * 目录条目本身不计字节（"大小"列口径；"占用空间"列才含簇开销）；
//   * 未初始化的沙箱（FileRoot 目录不存在）= 0/0/0，OK。
// 禁参考 SandMan BoxMonitor（01-license-map §5.2）。

#include "BoxUsage.h"

#include <windows.h>

namespace sbie::model {

namespace {

// 单目录枚举递归统计。path 为普通 DOS 路径；scanRoot 报告进度。
SbieStatus ScanDir(const std::wstring& path, BoxUsageStats* out,
                   const std::function<bool(ULONGLONG)>& progress,
                   unsigned long long* tick)
{
    // 长路径前缀（FindFirstFileW：路径超 MAX_PATH-12 需 \\?\；就地保留
    // 调用方给出的普通形态用于拼接）
    std::wstring findPath = path + L"\\*";
    if (findPath.size() > MAX_PATH - 12)
        findPath = L"\\\\?\\" + findPath;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(findPath.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;

    SbieStatus st = SbieStatus::OK;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;   // 重解析点：跳过（见文件头）

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++out->dirs;
            st = ScanDir(path + L"\\" + name, out, progress, tick);
            if (st != SbieStatus::OK)
                break;
        } else {
            ++out->files;
            out->totalBytes += ((unsigned long long)fd.nFileSizeHigh << 32)
                               | fd.nFileSizeLow;
        }

        // 进度节流：每 256 个条目回调一次（返回 false = 取消）
        if (progress && (++*tick & 0xFF) == 0) {
            if (!progress(out->totalBytes)) {
                st = SbieStatus::GENERIC;
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return st;
}

} // namespace

SbieStatus ScanBoxSize(const std::wstring& fileRoot, BoxUsageStats* out,
                       const std::function<bool(ULONGLONG)>& progress)
{
    if (!out)
        return SbieStatus::GENERIC;
    *out = BoxUsageStats{};
    if (fileRoot.empty())
        return SbieStatus::GENERIC;
    // 根不存在（未初始化沙箱）= 空；非目录 = 参数错
    const DWORD at = GetFileAttributesW(fileRoot.c_str());
    if (at == INVALID_FILE_ATTRIBUTES)
        return SbieStatus::OK;
    if (!(at & FILE_ATTRIBUTE_DIRECTORY))
        return SbieStatus::GENERIC;

    unsigned long long tick = 0;
    SbieStatus st = ScanDir(fileRoot, out, progress, &tick);
    if (st != SbieStatus::OK)
        *out = BoxUsageStats{};   // 取消/失败不回半程数据
    return st;
}

SbieStatus ScanBoxSize(const std::wstring& fileRoot, ULONGLONG* bytes,
                       const std::function<bool(ULONGLONG)>& progress)
{
    BoxUsageStats stats;
    SbieStatus st = ScanBoxSize(fileRoot, &stats, progress);
    if (bytes)
        *bytes = stats.totalBytes;
    return st;
}

} // namespace sbie::model
