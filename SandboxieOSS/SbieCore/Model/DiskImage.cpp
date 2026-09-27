// Sandboxie-OSS — SbieCore/Model/DiskImage.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 磁盘映像沙箱 / RAM 磁盘领域逻辑实现（契约：本文件 + DiskImage.h；
// 执行者定位见头注；行为参考范围 01-license-map §2——core 为 GPLv3 实读，
// QSbieAPI ImBox* 组包形态仅参考）。

#include "DiskImage.h"
#include "Boxes.h"
#include "ConfigStore.h"
#include "../DriverApi/DriverApi.h"
#include "../Util/PathMapper.h"

#include <windows.h>

namespace sbie::model {

namespace {

bool KeyIsY(const std::wstring& section, const std::wstring& setting)
{
    auto v = ConfigStore().Get(section, setting, 0, true, true);
    return v.has_value() && *v == L"y";
}

// 键读取（缺省值语义对齐 QSbieAPI GetBool：ProtectAdminOnly 缺省 y）
std::wstring KeyText(const std::wstring& section, const std::wstring& setting)
{
    auto v = ConfigStore().Get(section, setting, 0, true, true);
    return v.has_value() ? *v : std::wstring();
}

// IMBOX_QUERY 包装：失败原因折叠进 ImMountState（known=false），不外泄错误
void QueryMountInto(const std::wstring& regRootNt, ImMountState* out)
{
    svc::SvcClient::ImDiskMount m;
    SbieStatus st = svc::SvcClient::Instance().ImBoxQuery(regRootNt, &m);
    if (st == SbieStatus::OK) {
        out->known = true;
        out->mounted = true;
        out->diskRoot = std::move(m.diskRoot);
        out->diskSize = m.diskSize;
        out->usedSize = m.usedSize;
    } else if (st == SbieStatus::NOT_FOUND) {
        out->known = true;
        out->mounted = false;
        out->diskRoot.clear();
        out->diskSize = out->usedSize = 0;
    }
    // 其余（DRIVER_UNAVAILABLE / ERR_SVC_TRANSPORT / …）：known 保持 false
}

} // namespace

SbieStatus QueryBoxImage(const std::wstring& box, BoxImageInfo* out)
{
    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    BoxInfo bi;
    SbieStatus st = repo.GetInfo(box, &bi);
    if (st != SbieStatus::OK)
        return st;

    out->box = box;
    out->boxExists = true;
    out->fileRootDos = bi.fileRoot;
    out->regRootNt = bi.regRoot;
    out->useFileImage = KeyIsY(box, L"UseFileImage");
    out->useRamDisk = KeyIsY(box, L"UseRamDisk");
    out->confidential = KeyIsY(box, L"ConfidentialBox");
    out->lessConfidential = KeyIsY(box, L"LessConfidentialBox");
    out->enableEfs = KeyIsY(box, L"EnableEFS");
    out->forceProtectionOnMount = KeyIsY(box, L"ForceProtectionOnMount");
    auto pao = ConfigStore().Get(box, L"ProtectAdminOnly", 0, true, true);
    out->protectAdminOnly = !pao.has_value() || *pao == L"y";

    // 镜像文件 = <FileRoot DOS>.box（服务端 GetImageFileName 同名规则）
    if (!bi.fileRoot.empty()) {
        out->imageFile = bi.fileRoot + L".box";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(out->imageFile.c_str(), GetFileExInfoStandard,
                                 &fad)) {
            out->imageExists = true;
            out->imageBytes = ((unsigned long long)fad.nFileSizeHigh << 32)
                              | fad.nFileSizeLow;
        }
    }

    // 挂载状态（UseRamDisk 箱的挂载键是共享 RAM 盘——空 reg_root 查询；
    // UseFileImage 箱按本箱 reg_root 查询；两者皆非则仍按本箱查，回未挂载）
    if (!bi.regRoot.empty())
        QueryMountInto(out->useRamDisk ? std::wstring() : bi.regRoot,
                       &out->mount);
    return SbieStatus::OK;
}

std::vector<BoxImageInfo> EnumBoxImages()
{
    std::vector<BoxImageInfo> out;
    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    for (const BoxInfo& bi : repo.EnumBoxes(false)) {
        BoxImageInfo ii;
        if (QueryBoxImage(bi.name, &ii) == SbieStatus::OK)
            out.push_back(std::move(ii));
    }
    return out;
}

SbieStatus CreateBoxImage(const std::wstring& box, unsigned long long sizeKb,
                          const std::wstring& password)
{
    if (sizeKb < 256ull * 1024)
        return SbieStatus::INVALID;
    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    BoxInfo bi;
    SbieStatus st = repo.GetInfo(box, &bi);
    if (st != SbieStatus::OK)
        return st;
    if (bi.fileRoot.empty())
        return SbieStatus::GENERIC;   // 驱动不可答路径（box info 已挡 3 号码）
    return svc::SvcClient::Instance().ImBoxCreate(bi.fileRoot, sizeKb, password);
}

SbieStatus MountBoxImage(const std::wstring& box, const std::wstring& password,
                         std::optional<bool> protect,
                         std::optional<bool> adminOnly, bool autoUnmount)
{
    BoxImageInfo info;
    SbieStatus st = QueryBoxImage(box, &info);
    if (st != SbieStatus::OK)
        return st;
    if (info.fileRootDos.empty() || info.regRootNt.empty())
        return SbieStatus::GENERIC;
    const bool doProtect = protect.has_value() ? *protect
                                               : info.forceProtectionOnMount;
    const bool doAdminOnly = adminOnly.has_value() ? *adminOnly
                                                   : info.protectAdminOnly;
    return svc::SvcClient::Instance().ImBoxMount(
        info.regRootNt, info.fileRootDos, password, doProtect, doAdminOnly,
        autoUnmount);
}

SbieStatus UnmountBoxImage(const std::wstring& box)
{
    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    BoxInfo bi;
    SbieStatus st = repo.GetInfo(box, &bi);
    if (st != SbieStatus::OK)
        return st;
    if (bi.regRoot.empty())
        return SbieStatus::GENERIC;
    return svc::SvcClient::Instance().ImBoxUnmount(bi.regRoot);
}

SbieStatus EnumMountedRoots(std::vector<std::wstring>* regRoots)
{
    return svc::SvcClient::Instance().ImBoxEnum(regRoots);
}

SbieStatus QueryRamDisk(RamDiskInfo* out)
{
    const std::wstring sizeText = KeyText(L"GlobalSettings", L"RamDiskSizeKb");
    if (!sizeText.empty())
        out->sizeKb = _wcstoui64(sizeText.c_str(), nullptr, 10);
    out->sizeBelowMinimum =
        out->sizeKb != 0 && out->sizeKb < 100ull * 1024;
    out->letter = KeyText(L"GlobalSettings", L"RamDiskLetter");

    BoxRepository repo(nullptr, svc::SvcClient::Instance());
    for (const BoxInfo& bi : repo.EnumBoxes(false)) {
        if (KeyIsY(bi.name, L"UseRamDisk"))
            out->boxes.push_back(bi.name);
    }

    // 空 reg_root = 共享 RAM 盘查询（MountManager QueryHandler 约定）
    QueryMountInto(std::wstring(), &out->mount);
    return SbieStatus::OK;
}

} // namespace sbie::model
