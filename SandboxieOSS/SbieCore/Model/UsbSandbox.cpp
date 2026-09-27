// Sandboxie-OSS — SbieCore/Model/UsbSandbox.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// USB 沙箱接管实现（规格锚点 docs/07 07-P2-1；卷枚举决策见 UsbSandbox.h 头注）。

#include "UsbSandbox.h"
#include "Boxes.h"
#include "ConfigStore.h"
#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"

#include <windows.h>
#include <winioctl.h>

#include <cwchar>

namespace sbie::model {

namespace {

// 卷序列号 DWORD → "HHHH-LLLL"（大写、4-4 零填充，vol 命令同格式）
std::wstring SerialText(DWORD sn)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%04X-%04X", HIWORD(sn), LOWORD(sn));
    return buf;
}

// 卷设备总线查询：IOCTL_STORAGE_QUERY_PROPERTY → BusType。
// 尝试两种打开形态：挂载点（\\.\X:）与卷 GUID 设备（\\?\Volume{…} 去尾斜杠）。
bool QueryBusIsUsb(const std::wstring& volumeGuid,
                   const std::vector<std::wstring>& mountPoints, bool* known)
{
    *known = false;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    // 足量缓冲：定长头 + 变长尾（vendor/product/serial 字符串）
    unsigned char buf[1024];
    STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buf;

    auto tryOpen = [&](const wchar_t* path) -> bool {
        HANDLE h = CreateFileW(path, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query,
                                  sizeof(query), buf, sizeof(buf), &ret, nullptr);
        CloseHandle(h);
        if (!ok)
            return false;
        *known = true;
        return desc->BusType == 7;   // BusTypeUsb
    };

    // 形态 1：有盘符的卷开 \\.\X:（去尾反斜杠）
    for (const std::wstring& mp : mountPoints) {
        if (mp.size() >= 2 && mp[1] == L':') {
            std::wstring dev = L"\\\\.\\" + mp.substr(0, 2);
            if (tryOpen(dev.c_str()))
                return true;
            if (*known)
                return false;   // 查询成功但非 USB——结论已定
        }
    }
    // 形态 2：卷 GUID 设备（\\?\Volume{…}，去尾反斜杠）
    if (volumeGuid.size() > 1) {
        std::wstring dev = volumeGuid;
        if (dev.back() == L'\\')
            dev.pop_back();
        if (tryOpen(dev.c_str()))
            return true;
    }
    return false;
}

} // namespace

std::vector<UsbVolume> EnumVolumes()
{
    std::vector<UsbVolume> out;
    WCHAR guid[64];
    const DWORD guidLen = (DWORD)(sizeof(guid) / sizeof(guid[0]));
    HANDLE it = FindFirstVolumeW(guid, guidLen);
    if (it == INVALID_HANDLE_VALUE)
        return out;
    do {
        // 只取卷设备（\\?\Volume{GUID}\）；跳过挂载文件夹形式的返回项
        if (wcsncmp(guid, L"\\\\?\\Volume{", 11) != 0)
            continue;
        UsbVolume v;
        v.volumeGuid = guid;

        // 全部路径名（盘符 + 挂载文件夹；多串缓冲）。
        // lpcchReturnLength 不可为 NULL（win32 API 契约）——坑记录 04 §18
        DWORD n = 0;
        WCHAR paths[2048];
        const DWORD pathsCap = (DWORD)(sizeof(paths) / sizeof(paths[0]));
        if (GetVolumePathNamesForVolumeNameW(guid, paths, pathsCap, &n)
            && n && n < pathsCap) {
            const WCHAR* p = paths;
            while (*p) {
                v.mountPoints.push_back(p);
                p += wcslen(p) + 1;
            }
        }

        // 序列号 + 卷标：任一挂载点即可
        if (!v.mountPoints.empty()) {
            DWORD serial = 0;
            WCHAR label[MAX_PATH + 1];
            if (GetVolumeInformationW(v.mountPoints[0].c_str(), label,
                                      (DWORD)(sizeof(label) / sizeof(label[0])),
                                      &serial, nullptr, nullptr, nullptr, 0)) {
                v.serial = SerialText(serial);
                v.label = label;
            }
        }

        v.onUsbBus = QueryBusIsUsb(v.volumeGuid, v.mountPoints, &v.busKnown);
        out.push_back(std::move(v));
    } while (FindNextVolumeW(it, guid, guidLen));
    FindVolumeClose(it);
    return out;
}

bool UsbSandboxInfo::VolumeTaken(const UsbVolume& v) const
{
    if (!v.busKnown || !v.onUsbBus || v.serial.empty() || v.mountPoints.empty())
        return false;
    for (const std::wstring& d : disabledVolumes)
        if (_wcsicmp(d.c_str(), v.serial.c_str()) == 0)
            return false;
    return true;
}

std::vector<std::wstring> UsbSandboxInfo::TargetForceFolders() const
{
    std::vector<std::wstring> out;
    for (const UsbVolume& v : volumes) {
        if (!VolumeTaken(v))
            continue;
        for (const std::wstring& mp : v.mountPoints)
            out.push_back(mp);
    }
    return out;
}

SbieStatus QueryUsbSandbox(UsbSandboxInfo* out)
{
    auto flag = ConfigStore().Get(L"GlobalSettings", L"ForceUsbDrives", 0, true,
                                  true);
    out->forceUsbDrives = flag.has_value() && *flag == L"y";

    auto name = ConfigStore().Get(L"GlobalSettings", L"UsbSandbox", 0, true, true);
    out->sandboxName = name.has_value() && !name->empty() ? *name
                                                          : std::wstring(L"USB_Box");

    bool enabled = false, exists = false;
    SbieStatus st = drv::IsBoxEnabled(out->sandboxName, &enabled, &exists);
    if (st != SbieStatus::OK)
        return st;
    out->sandboxExists = exists;

    out->disabledVolumes = ConfigStore().GetList(L"GlobalSettings",
                                                 L"DisabledForceVolume");
    out->forceFolders = exists
        ? ConfigStore().GetList(out->sandboxName, L"ForceFolder") 
        : std::vector<std::wstring>();
    out->volumes = EnumVolumes();
    return SbieStatus::OK;
}

SbieStatus SyncUsbSandbox(const std::wstring& password, bool dryRun,
                          UsbSyncResult* out)
{
    UsbSandboxInfo info;
    SbieStatus st = QueryUsbSandbox(&info);
    if (st != SbieStatus::OK)
        return st;
    if (!info.forceUsbDrives)
        return SbieStatus::INVALID;   // 调用方给提示（对齐守护前置条件）

    const std::vector<std::wstring> targets = info.TargetForceFolders();
    out->written = targets;

    // 与现值差分（呈现用）
    auto contains = [](const std::vector<std::wstring>& list,
                       const std::wstring& v) {
        for (const std::wstring& e : list)
            if (_wcsicmp(e.c_str(), v.c_str()) == 0)
                return true;
        return false;
    };
    for (const std::wstring& t : targets)
        if (!contains(info.forceFolders, t))
            ++out->added;
    for (const std::wstring& old : info.forceFolders)
        if (!contains(targets, old))
            ++out->removed;

    if (dryRun)
        return SbieStatus::OK;

    // 组装写序列（落盘提交语义：SbieSvc 的 refresh=false 只改服务进程内存树，
    // 落盘 + 驱动重载由 refresh=true 的那次写完成——docs/04 §8.4 坑记录；
    // 故全部 refresh=false、仅末条 refresh=true 提交整棵内存树）。
    struct Write {
        const wchar_t* setting;
        std::wstring value;
        bool append;    // false = SET（整表替换/首值），true = Append
    };
    std::vector<Write> writes;

    // 箱不存在：创建（Enabled=y）
    if (!info.sandboxExists) {
        st = BoxRepository::ValidateName(info.sandboxName);
        if (st != SbieStatus::OK)
            return st;
        writes.push_back({ L"Enabled", L"y", false });
        out->boxCreated = true;
    }
    // 初始键组（07-P2-1 规格三键；幂等——已显式配置者不覆盖，07 未记录
    // SandMan 重写既有值的行为，取保守语义）
    const wchar_t* initKeys[] = { L"UseFileDeleteV2", L"UseRegDeleteV2",
                                  L"UseVolumeSerialNumbers" };
    for (const wchar_t* key : initKeys) {
        auto cur = ConfigStore().Get(info.sandboxName, key, 0, true, true);
        if (cur.has_value() && !cur->empty())
            continue;
        writes.push_back({ key, L"y", false });
    }
    // ForceFolder 整表替换：SET（CIniFile::SetValue 先清后置首值）+ 其余 Append；
    // targets 空 = SET 空值（删除整个设置）
    if (targets.empty()) {
        writes.push_back({ L"ForceFolder", L"", false });
    } else {
        writes.push_back({ L"ForceFolder", targets[0], false });
        for (size_t i = 1; i < targets.size(); ++i)
            writes.push_back({ L"ForceFolder", targets[i], true });
    }

    for (size_t i = 0; i < writes.size(); ++i) {
        const bool commit = (i + 1 == writes.size());   // 末条落盘 + 热重载
        st = writes[i].append
            ? ConfigStore().SetAppend(info.sandboxName, writes[i].setting,
                                      writes[i].value, commit, password)
            : ConfigStore().Set(info.sandboxName, writes[i].setting,
                                writes[i].value, commit, password);
        if (st != SbieStatus::OK)
            return st;
    }
    return SbieStatus::OK;
}

} // namespace sbie::model
