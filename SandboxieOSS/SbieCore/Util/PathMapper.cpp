// Sandboxie-OSS — SbieCore/Util/PathMapper.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（非复制）：QSbieAPI\SbieAPI.cpp 的 Nt2DosPath（LGPL-2.1）。

#include "PathMapper.h"
#include "DriverApi/DriverApi.h"

#include <windows.h>
#include <cwchar>

namespace sbie::util {

void NtToDosPath(std::wstring* path)
{
    if (!path || path->empty())
        return;

    // 1) SbieDll 的符号链接解析（\Device\HarddiskVolumeX\… → X:\…）
    if (drv::Loaded() && drv::ApiP()->SbieDll_TranslateNtToDosPath) {
        // 该导出原地改写，缓冲区需留余量
        std::wstring buf = *path;
        buf.resize(buf.size() + 64, L'\0');
        if (drv::ApiP()->SbieDll_TranslateNtToDosPath(buf.data())) {
            // 截断到首个 NUL
            size_t n = wcsnlen(buf.c_str(), buf.size());
            std::wstring out(buf.c_str(), n);
            if (!out.empty() && out[1] == L':' /* 形如 X:\ */) {
                *path = std::move(out);
                return;
            }
        }
    }

    // 2) `\??\C:\...` 前缀剥离
    static const wchar_t kNtPrefix[] = L"\\??\\";
    if (path->compare(0, 4, kNtPrefix) == 0) {
        *path = path->substr(4);
        return;
    }

    // 3) \Device\HarddiskVolumeN\… → 逐盘符 QueryDosDeviceW 匹配
    //    （设备名 "C:" 形态——同 DosToNtPath 的尾斜杠坑，04 §15）
    if (path->compare(0, 8, L"\\Device\\") == 0) {
        wchar_t root[3] = L"C:";
        wchar_t target[512];
        for (wchar_t drvLetter = L'A'; drvLetter <= L'Z'; ++drvLetter) {
            root[0] = drvLetter;
            DWORD n = QueryDosDeviceW(root, target, (DWORD)(std::size(target)));
            if (n > 0 && target[0]) {
                size_t len = wcslen(target);
                if (path->compare(0, len, target) == 0
                    && (*path)[len] == L'\\') {
                    *path = std::wstring(root, 2) + L"\\"
                          + path->substr(len + 1);
                    return;
                }
            }
        }
    }
    // 其余（如 \RPC Control\…、\Device\… 未能映射）原样保留
}

void DosToNtPath(std::wstring* path)
{
    if (!path || path->size() < 2 || (*path)[1] != L':')
        return;
    // QueryDosDeviceW 的设备名须为 "C:" 形态——带尾反斜杠的 "C:\" 实测
    // n=0 不转换（04 §15 附带修复；QSbieAPI SbieAPI.cpp:963 同款调用形态）
    wchar_t root[3] = { (*path)[0], L':', L'\0' };
    wchar_t target[512];
    DWORD n = QueryDosDeviceW(root, target, (DWORD)(std::size(target)));
    if (n > 0 && target[0]) {
        // C:\foo → \Device\HarddiskVolume4\foo（path+2 保留 "\foo" 部分）
        *path = std::wstring(target) + path->substr(2);
    }
}

} // namespace sbie::util
