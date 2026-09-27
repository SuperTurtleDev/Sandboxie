// Sandboxie-OSS — SbieCore/Model/Boxes.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 沙箱领域逻辑（契约：04-modules.md §2.4；签名冻结）。
// M1 实现状态：EnumSections / EnumBoxes / GetInfo / ValidateName 已实现
// （纯驱动读路径，可降级直连）；写路径（Create/Rename/Delete/SetEnabled/
// TerminateAll）为占位桩（需 SbieSvc，server 波次接通）。
//
// 行为参考（01-license-map §2 允许范围）：QSbieAPI SbieAPI.cpp ReloadBoxes
// （:1171-1200 节枚举 + IsBox 过滤）、CreateBox（:1459-1477 仅写 Enabled=y，
// 不建目录——04 §8.4）、ValidateName（:1412-1445）。

#pragma once

#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"
#include "../Util/Status.h"

#include <string>
#include <vector>

namespace sbie::model {

struct BoxInfo {
    std::wstring name;
    bool enabled = false;
    bool exists = false;
    std::wstring fileRoot, regRoot, ipcRoot;   // QueryBoxPath 两段式
    bool hasProcesses = false;
};

class BoxRepository {
public:
    explicit BoxRepository(sbie::drv::Api* api, sbie::svc::SvcClient& svc);

    std::vector<std::wstring> EnumSections();  // 含 Template_*/UserSettings_*（枚举节）
    std::vector<BoxInfo> EnumBoxes(bool enabledOnly);  // EnumBoxesEx + IsBoxEnabled
    SbieStatus GetInfo(const std::wstring& name, BoxInfo* out);
    SbieStatus Create(const std::wstring& name);   // ValidateName + SET "Enabled"="y"
    SbieStatus Rename(const std::wstring& oldN, const std::wstring& newN); // 复制节+删旧节（refresh 后）
    SbieStatus Delete(const std::wstring& name, bool delFiles, bool delSection);
    SbieStatus SetEnabled(const std::wstring& name, bool on);
    SbieStatus TerminateAll(const std::wstring& name);
    // 名称校验（对齐 ValidateName，SbieAPI.cpp:1412-1445）：<=38 WCHAR；仅
    // [A-Za-z0-9_]；非 aux/con/nul/prn/com0-9/lpt0-9/clock$；非 GlobalSettings /
    // UserSettings_ 前缀
    static SbieStatus ValidateName(const std::wstring& name);

private:
    sbie::drv::Api* api_;      // 供后续直连 IOCTL 扩展用（当前薄封装自持状态）
    sbie::svc::SvcClient& svc_;
};

// ---------------------------------------------------------------------------
// 触发器执行（波次 A：07-P0-1/07-P0-2，additive——不动上方冻结契约面）。
// 语义（规格 docs/07 §3.1；实现决策记录 docs/04 §16）：
//   * 逐条读取 box 节 <setting>（OnBoxDelete / OnBoxTerminate）的命令值，
//     按 ini 顺序逐条执行（"逐条" = 前一条完成或超时后才启动下一条）；
//   * 值保持原样（不走驱动 %env% 预展开——驱动在 SYSTEM 上下文展开会把
//     %TEMP% 替换成 C:\WINDOWS\TEMP，语义错误）；%SANDBOX%（大小写不敏感）
//     替换为 box 名（07 记录的 SandMan 触发器变量；其余变量未记录 → 不
//     支持，见 04 §16）；%TEMP% 等 Windows 环境变量由子进程（通常 cmd.exe）
//     在继承的宿主用户环境中自行展开；
//   * CreateProcessW（继承环境、不继承句柄、CREATE_NO_WINDOW）宿主执行，
//     每条等待 ≤15s（对齐 07-P1-3 记录的 SandMan 检查器 15s 超时习惯；
//     超时不杀进程、继续下一条——CLI 侧无 SandMan 的"取消"交互面）；
//   * 失败（启动失败 / 非零退出 / 超时）继续执行剩余命令（尽力而为语义，
//     07 未记录 SandMan 的失败中止行为），计数经 out 返回；
//   * 返回值恒 OK（键不存在 = 0 条命令，也是成功）。
// ---------------------------------------------------------------------------
struct TriggerStats {
    unsigned run = 0;      // 成功启动的命令数
    unsigned failed = 0;   // 启动失败 + 非零退出 + 超时
};

SbieStatus RunBoxTriggers(const std::wstring& box, const std::wstring& setting,
                          TriggerStats* out = nullptr);

// %SANDBOX%（大小写不敏感）→ box 名，其余原样（07 未记录的变量不支持，
// 04 §16 决策）。波 B 起为共享助手（Recovery 的 OnFileRecovery 检查器命令
// 与触发器执行器同款展开语义，docs/04 §17）。
std::wstring ExpandSandboxVar(const std::wstring& text, const std::wstring& box);

} // namespace sbie::model
