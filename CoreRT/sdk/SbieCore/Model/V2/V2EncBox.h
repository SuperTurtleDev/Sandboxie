// Sandboxie-OSS — SbieCore/Model/V2/V2EncBox.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 加密盒（create-encbox / UseFileImage=y）支撑（docs/10 §6.5）：
//   * 目录布局（用户规格 + 冻结服务端约定兼容）：
//       <dir>\sandbox.ini    明文配置（FileRootPath=<dir>\data + UseFileImage=y）
//       <dir>\data           盒根（junction 目标；挂载后指向卷内 SbieDisk）
//       <dir>\data.box       DiskCryptor 加密容器 = <file_root>.box
//     服务端镜像名约定 = file_root + ".box"（MountManager.cpp
//     GetImageFileName）——取 FileRootPath=<dir>\data 使镜像恰为 <dir>\data.box
//     （满足用户布局），而非默认的 <盒目录>.box（兄弟文件）。
//   * 挂载链（全在冻结 SbieSvc，零驱动/服务改动）：IMBOX_MOUNT(密码)
//     → ImDisk 挂载 + 建 junction；UseFileImage=y 的盒启动时
//     DriverAssistInject 会找现有挂载复用（AcquireBoxRoot）；autoUnmount=1
//     时盒终止通知自动摘 junction+卸载（DriverAssist.cpp:841）。
//   * 密码：--mount-password / --password 旗标，缺省 R3 自动交互（区别于
//     ini EditPassword）。

#pragma once

#include "V2Common.h"

namespace sbie::model::v2 {

// sandbox.ini 是否声明 UseFileImage=y（读盒目录明文配置）
bool IsEncryptedBox(const std::wstring& boxDir);

// 加密盒的盒根（file_root）与镜像文件路径
std::wstring EncBoxFileRoot(const std::wstring& boxDir);   // <dir>\data
std::wstring EncBoxImageFile(const std::wstring& boxDir);  // <dir>\data.box

// 创建加密容器：写 sandbox.ini（Enabled/UseFileImage/FileRootPath/
// Template=BoxTypes\Standard）→ IMBOX_CREATE（服务端建 <dir>\data.box 并
// 格式化后卸载）。sizeKb 为 0 时用默认 1GB。密码必填（空=INVALID——加密
// 容器无密码无意义；调用方先行 R3 交互获取）。
V2Err CreateEncryptedBox(const std::wstring& boxDir, unsigned long long sizeKb,
                         const std::wstring& password);

// 挂载（exec 前置）：regRootNt 来自驱动 QueryBoxPath（注册后可查）。
// autoUnmount=true——盒终止时 SbieSvc 自动卸载。幂等容忍"已挂载"。
V2Err MountEncBox(const std::wstring& box, const std::wstring& boxDir,
                  const std::wstring& regRootNt, const std::wstring& password);

// 卸载（monitor teardown 兜底；AutoUnmount 语义下通常已卸，幂等容忍）。
V2Err UnmountEncBox(const std::wstring& regRootNt);

// 解析 --size 形态（"1024M"/"1G"/"500M"/纯数字=M）→ KB；非法返回 0
unsigned long long ParseSizeToKb(const std::wstring& size);

} // namespace sbie::model::v2
