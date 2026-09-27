// Sandboxie-OSS — SbieCore/Model/DiskImage.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 磁盘映像沙箱 / RAM 磁盘领域逻辑（波 D2，docs/04 §18；规格锚点
// docs/07-deep-gap-analysis.md §2 表 N-A-7/N-A-8 与 06 N-A-1 合并行）。
//
// 执行者定位（可行性核实结论，docs/07 §2 旁注）：
//   * UseFileImage=y / UseRamDisk=y 的挂载执行者是 **SbieSvc core**（GPLv3，
//     core/svc/MountManager.cpp）：每个沙箱进程注入路径
//     DriverAssistInject.cpp:160 调 AcquireBoxRoot——读箱键、经 ImBox.exe
//     （SandboxieTools 运行时，SbieSvc 以子进程拉起）挂载 ImDisk 设备、把
//     FileRoot 变为指向盘内目录的 junction。**写键即生效**（下一次箱内首
//     进程启动时自动挂载），CLI 无需也不应在启动路径上做任何事。
//   * 显式挂载/卸载/枚举/查询在 SbieSvc 侧有完备的 pipe 面
//     （MSGID_IMBOX_CREATE/MOUNT/UNMOUNT/ENUM/QUERY，core/svc/MountManager.cpp
//     Handler）——本模块经 SvcClient::ImBox* 直达，等价于 QSbieAPI
//     ImBoxCreate/ImBoxMount/ImBoxUnmount/ImBoxEnum（LGPL 行为参考）。
//   * EnableEFS：纯键驱动（SbieDll 代理 EFS 文件打开 → SbieSvc UserServer
//     ::OpenFile 查键，core/svc/UserServer.cpp:642；core/dll/file.c
//     File_NtCreateFileProxy）——本模块只做状态呈现，无触发面。
//   * ImDisk 驱动/ImBox.exe 属 SandboxieTools（无许可证，01 §1）——本模块
//     不含其代码也不直调其 exe；运行时缺席时 SbieSvc 回
//     ERROR_DEVICE_NOT_AVAILABLE → DRIVER_UNAVAILABLE（状态面呈现
//     "unknown (ImDisk driver not available)"，属正确答案而非错误）。
//
// 键面（docs/07 §1.3 General/Security 页）：
//   箱级：UseFileImage、UseRamDisk、ConfidentialBox/LessConfidentialBox、
//         EnableEFS、ForceProtectionOnMount、ProtectAdminOnly；
//   全局：RamDiskSizeKb（SbieSvc 下限 100MB，MountManager.cpp:1134-1136）、
//         RamDiskLetter（空 = 自动分配盘符）。
// 镜像文件名：<FileRootPath DOS>.box（服务端 GetImageFileName 语义）。

#pragma once

#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"

#include <optional>
#include <string>
#include <vector>

namespace sbie::model {

// IMBOX_QUERY 的挂载状态聚合（known=false = 查询不可用：SbieSvc 断连或
// ImDisk 驱动缺席——两种情况 CLI 都只能如实报告"未知"）
struct ImMountState {
    bool known = false;
    bool mounted = false;
    std::wstring diskRoot;               // \Device\ImDiskN
    unsigned long long diskSize = 0;     // 字节
    unsigned long long usedSize = 0;     // 字节
};

// 单箱映像/加密状态（img status 的数据面）
struct BoxImageInfo {
    std::wstring box;
    bool boxExists = false;
    // 键面
    bool useFileImage = false;
    bool useRamDisk = false;
    bool confidential = false;           // ConfidentialBox=y
    bool lessConfidential = false;       // LessConfidentialBox=y
    bool enableEfs = false;              // EnableEFS=y
    bool forceProtectionOnMount = false; // ForceProtectionOnMount=y
    bool protectAdminOnly = true;        // ProtectAdminOnly（缺省 y，QSbieAPI 语义）
    // 路径与文件
    std::wstring fileRootDos;            // DOS 形（空 = 驱动不可用）
    std::wstring regRootNt;              // NT 注册表根（挂载键）
    std::wstring imageFile;              // <fileRootDos>.box
    bool imageExists = false;
    unsigned long long imageBytes = 0;   // 磁盘实占（压缩/稀疏口径）
    // 挂载状态
    ImMountState mount;
};

// 单箱聚合查询（box 不存在 → NOT_FOUND；SbieSvc 断连不影响键面，只影响
// mount.known）
SbieStatus QueryBoxImage(const std::wstring& box, BoxImageInfo* out);

// 全箱枚举（含无相关键的箱——调用方自行过滤/呈现；驱动不可用 → 空表）
std::vector<BoxImageInfo> EnumBoxImages();

// 显式创建镜像文件（MSGID_IMBOX_CREATE：SbieSvc 挂载→格式化→卸载，产物
// <fileRoot>.box）。sizeKb 下限 256*1024（BoxImageWindow 规格，docs/07 N-A-8）。
SbieStatus CreateBoxImage(const std::wstring& box, unsigned long long sizeKb,
                          const std::wstring& password);

// 显式挂载（MSGID_IMBOX_MOUNT）。protect 缺省 = ForceProtectionOnMount 键；
// adminOnly 仅在 protect 生效（API_PROTECT_ROOT 参数），缺省 = ProtectAdminOnly
// 键；autoUnmount = 末进程退出后自动卸载（MountHandler req->auto_unmount）。
SbieStatus MountBoxImage(const std::wstring& box, const std::wstring& password,
                         std::optional<bool> protect,
                         std::optional<bool> adminOnly, bool autoUnmount);

// 显式卸载（MSGID_IMBOX_UNMOUNT；根在用/未挂载均回 NOT_FOUND 语义错误）
SbieStatus UnmountBoxImage(const std::wstring& box);

// 已挂载根枚举（IMBOX_ENUM；ImDisk 缺席 → DRIVER_UNAVAILABLE）
SbieStatus EnumMountedRoots(std::vector<std::wstring>* regRoots);

// RAM 磁盘聚合（ramdisk status 的数据面；共享单实例——所有 UseRamDisk 箱
// 挂同一盘内各自的 <boxname> 目录，MountManager m_RamDisk 语义）
struct RamDiskInfo {
    unsigned long long sizeKb = 0;       // 0 = 未配置
    bool sizeBelowMinimum = false;       // < 100MB：SbieSvc 拒绝挂载（msg 2238）
    std::wstring letter;                 // 空 = 自动分配
    std::vector<std::wstring> boxes;     // UseRamDisk=y 的箱
    ImMountState mount;                  // 空 reg_root 的 IMBOX_QUERY
};
SbieStatus QueryRamDisk(RamDiskInfo* out);

} // namespace sbie::model
