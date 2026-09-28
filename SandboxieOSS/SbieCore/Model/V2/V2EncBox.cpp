// Sandboxie-OSS — SbieCore/Model/V2/V2EncBox.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2EncBox.h 实现。

#include "V2EncBox.h"
#include "../../DriverApi/DriverApi.h"
#include "../../SvcClient/SvcClient.h"
#include "../../Util/Utf8.h"

#include <windows.h>

namespace sbie::model::v2 {

bool IsEncryptedBox(const std::wstring& boxDir)
{
    IniFileData ini;
    if (!ParseIniFile(boxDir + L"\\sandbox.ini", &ini).Ok())
        return false;
    const IniSectionData* sec = ini.Find(BoxNameFromPath(boxDir));
    if (!sec)
        sec = ini.Find(L"");   // 单节省头形态
    if (!sec)
        return false;
    for (const auto& kv : sec->entries)
        if (_wcsicmp(kv.key.c_str(), L"UseFileImage") == 0
            && (kv.value[0] == L'y' || kv.value[0] == L'Y'))
            return true;
    return false;
}

std::wstring EncBoxFileRoot(const std::wstring& boxDir)
{
    return boxDir + L"\\data";
}

std::wstring EncBoxImageFile(const std::wstring& boxDir)
{
    return EncBoxFileRoot(boxDir) + L".box";
}

unsigned long long ParseSizeToKb(const std::wstring& size)
{
    if (size.empty())
        return 0;
    WCHAR unit = towupper(size.back());
    std::wstring num = size;
    bool hasUnit = (unit == L'M' || unit == L'G' || unit == L'K');
    if (hasUnit)
        num.pop_back();
    if (num.empty())
        return 0;
    for (wchar_t c : num)
        if (!iswdigit(c))
            return 0;
    unsigned long long v = wcstoull(num.c_str(), nullptr, 10);
    if (v == 0)
        return 0;
    // 返回单位 = KB（wire image_size 语义）。纯数字按 M（用户规格）。
    switch (unit) {
    case L'K': return v;
    case L'G': return v * 1024ull * 1024ull;
    default:   return v * 1024ull;   // 'M' 或纯数字
    }
}

V2Err CreateEncryptedBox(const std::wstring& boxDir, unsigned long long sizeKb,
                         const std::wstring& password)
{
    V2Err ne = ValidateBoxName(BoxNameFromPath(boxDir));
    if (!ne.Ok())
        return ne;
    if (password.empty())
        return {SbieStatus::INVALID, L"encrypted box requires a password"};
    if (sizeKb == 0)
        sizeKb = 1024ull * 1024ull;   // 默认 1G
    if (sizeKb < 256ull * 1024ull / 1024ull)   // ImDisk/格式化下限保护
        { /* 允许小盒（自测用 256M），不设更高门槛 */ }
    if (sizeKb < 16)
        return {SbieStatus::INVALID, L"size too small (min 16M)"};

    if (!CreateDirectoryW(boxDir.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS)
        return {SbieStatus::GENERIC, L"cannot create " + boxDir};
    if (PathExists(boxDir + L"\\sandbox.ini"))
        return {SbieStatus::INVALID,
                boxDir + L"\\sandbox.ini already exists (not an empty dir)"};

    // 明文配置：FileRootPath=<dir>\data（镜像名约定 → <dir>\data.box）
    const std::wstring box = BoxNameFromPath(boxDir);
    std::wstring ini;
    ini += L"# v2 encrypted box (created by sbie-cli create-encbox)\n";
    ini += L"# container: data.box (DiskCryptor via SbieSvc MountManager/ImBox)\n";
    ini += L"[" + box + L"]\n";
    ini += L"Enabled=y\n";
    ini += L"UseFileImage=y\n";
    ini += L"FileRootPath=" + EncBoxFileRoot(boxDir) + L"\n";
    ini += L"Template=BoxTypes\\Standard\n";
    V2Err we = WriteTextFileAtomic(boxDir + L"\\sandbox.ini",
                                   util::WideToUtf8(ini));
    if (!we.Ok())
        return we;

    // IMBOX_CREATE：服务端建卷+格式化+卸载（CreateHandler 全流程同步）
    SbieStatus s = svc::SvcClient::Instance().ImBoxCreate(
        EncBoxFileRoot(boxDir), sizeKb, password);
    if (s != SbieStatus::OK) {
        DeleteFileW((boxDir + L"\\sandbox.ini").c_str());
        return {s, L"ImBoxCreate failed ("
                      + std::wstring(StatusName(s))
                      + L"); is the SandboxieTools ImDisk runtime installed?"};
    }
    return {};
}

V2Err MountEncBox(const std::wstring& box, const std::wstring& boxDir,
                  const std::wstring& regRootNt, const std::wstring& password)
{
    SbieStatus s = svc::SvcClient::Instance().ImBoxMount(
        regRootNt, EncBoxFileRoot(boxDir), password,
        false /*protectRoot*/, false /*adminOnly*/, true /*autoUnmount*/);
    if (s == SbieStatus::OK)
        return {};
    // 已挂载（AcquireBoxRoot 复用路径）→ 幂等通过
    svc::SvcClient::ImDiskMount q;
    if (svc::SvcClient::Instance().ImBoxQuery(regRootNt, &q)
            == SbieStatus::OK
        && q.mounted)
        return {};
    return {s, L"mount data.box failed for '" + box + L"' ("
                  + StatusName(s) + L")"};
}

V2Err UnmountEncBox(const std::wstring& regRootNt)
{
    SbieStatus s = svc::SvcClient::Instance().ImBoxUnmount(regRootNt);
    if (s == SbieStatus::OK || s == SbieStatus::NOT_FOUND)
        return {};   // 未挂载=幂等成功
    return {s, L"unmount failed (" + std::wstring(StatusName(s)) + L")"};
}

} // namespace sbie::model::v2
