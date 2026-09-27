// Sandboxie-OSS — SbieCore/Model/UsbSandbox.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// USB 沙箱接管领域逻辑（波 D2，docs/04 §18；规格锚点 docs/07-deep-gap-analysis.md
// 07-P2-1——SandMan UpdateForceUSB 的 CLI 等价，规格语义以 07 记录为准）。
//
// 规格（07 §3.3 07-P2-1 行 + §2 表）：
//   * 全局键 ForceUsbDrives=y 时：枚举本机 USB 卷（卷序列号十六进制
//     HHHH-LLLL 为标识），未被全局 DisabledForceVolume 列表排除的卷的
//     全部挂载点整体写入 UsbSandbox 箱的 ForceFolder（整表替换）；
//   * 目标箱不存在时自动创建并设初始键组：UseFileDeleteV2=y、
//     UseRegDeleteV2=y、UseVolumeSerialNumbers=y；
//   * UsbSandbox 全局键缺省值 "USB_Box"。
//
// 卷枚举实现决策（04 §18 记录）：
//   * SandMan 侧经 SetupAPI 枚举物理盘的 USBSTOR 枚举器再归并卷——本实现
//     改用卷设备直查：对每个卷打开设备句柄发 IOCTL_STORAGE_QUERY_PROPERTY，
//     BusType == BusTypeUsb(7) 即判 USB。纯 kernel32（FindFirstVolumeW /
//     GetVolumePathNamesForVolumeNameW / GetVolumeInformationW），不引
//     SetupAPI 依赖；语义覆盖面更宽（UASP 等 USB 附加 SCSI 总线盘亦计入，
//     USBSTOR 枚举器只覆盖传统 BOT）。
//   * 总线查询失败的卷（无介质/权限）标 busKnown=false——不计入接管，
//     状态面如实呈现。
#pragma once

#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

struct UsbVolume {
    std::wstring volumeGuid;             // \\?\Volume{...}\（枚举键）
    std::wstring serial;                 // "HHHH-LLLL"（空 = SN 不可得）
    std::wstring label;                  // 卷标（可空）
    std::vector<std::wstring> mountPoints; // "E:\"、挂载文件夹路径（可空）
    bool busKnown = false;               // 总线查询成功
    bool onUsbBus = false;               // BusTypeUsb
};

// 枚举本机全部有 GUID 卷设备的卷（含无挂载点/DVD 等；busKnown/onUsbBus
// 标注）。纯 Win32 读路径，恒成功（空表 = 无卷或权限异常）。
std::vector<UsbVolume> EnumVolumes();

// usb status 的数据面
struct UsbSandboxInfo {
    bool forceUsbDrives = false;         // GlobalSettings\ForceUsbDrives
    std::wstring sandboxName;            // 缺省 USB_Box
    bool sandboxExists = false;
    std::vector<std::wstring> disabledVolumes; // DisabledForceVolume 列表
    std::vector<std::wstring> forceFolders;    // 箱现值（可空）
    std::vector<UsbVolume> volumes;            // EnumVolumes() 结果
    // 计算接管目标：USB 总线卷 & SN 可得 & 未被禁用 & 有挂载点 → 挂载点全集
    std::vector<std::wstring> TargetForceFolders() const;
    // 单卷是否会被接管（SN 空/禁用/非 USB/无挂载点 → false）
    bool VolumeTaken(const UsbVolume& v) const;
};
SbieStatus QueryUsbSandbox(UsbSandboxInfo* out);

// usb sync 的一次性接管（07-P2-1 CLI 形态）。dryRun=true 只计算不落盘。
// ForceUsbDrives=n 时拒绝（对齐守护行为的前置条件；INVALID + 调用方提示）。
struct UsbSyncResult {
    bool boxCreated = false;
    std::vector<std::wstring> written;   // 落盘后的 ForceFolder 值集
    unsigned added = 0;                  // 相对旧值的新增
    unsigned removed = 0;                // 相对旧值的移除
};
SbieStatus SyncUsbSandbox(const std::wstring& password, bool dryRun,
                          UsbSyncResult* out);

} // namespace sbie::model
