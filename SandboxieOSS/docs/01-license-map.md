# 01 — 许可证映射

本文回答一个问题：**Sandboxie-OSS 的每一部分，允许参考/复制仓库内哪些旧代码。**

## 0. 总原则

1. 本项目整体 **GPLv3**。GPLv3 代码可以**直接复制**进本项目（保留原版权头 + GPLv3 头），
   也可以按行为重写（重写后无传染义务的引用关系，但本项目本身已是 GPLv3，无实际差别）。
2. LGPL 部分**不得整文件复制**进本项目（静态链接进闭源才有意义；本项目开源，复制反而引入
   许可证混杂），但**允许且鼓励阅读其实现来理解协议/算法**，并按行为重写。重写时禁止
   逐行翻译式抄袭。
3. custom license（非 OSI）部分：**禁止阅读后重写其实现细节之外的一切复用**。对 SandMan
   的使用边界 = 仅允许"阅读其头文件与公开功能描述来确认功能面清单"，**任何 .cpp 实现文件
   不得作为参考**；功能行为以 `QSbieAPI`（LGPL）的公共方法签名与本文档其他各篇的 core
   (GPL) 源码实读为准。
4. 无许可证部分：**不使用、不参考**。

## 1. 许可证事实（实读核实）

来源：仓库根 `LICENSE.Plus`（"Further licensing information" 节，第 54-64 行）与各目录内许可证文件。

| 组件 | 路径 | 许可证 | 本项目可否复制源码 | 本项目可否参考实现 |
|---|---|---|---|---|
| Sandboxie core | `Sandboxie\`（`LICENSE.Classic` = GPLv3） | GPLv3 | **可以**（保留版权头） | 可以 |
| QSbieAPI | `SandboxiePlus\QSbieAPI\`（含 `LICENSE` 文件，LGPL-3.0） | LGPL-3.0 | 不复制整文件 | **可以**（协议层的主要参考） |
| SandMan | `SandboxiePlus\SandMan\`（整目录） | **custom license（非 OSI）** | **禁止** | **禁止**（仅可读 .h 确认功能面，见 §0.3） |
| MiscHelpers | `SandboxiePlus\MiscHelpers\` | LGPL（Qt 系） | 否（不用 Qt） | 否（无对应需求） |
| UGlobalHotkey | `SandboxiePlus\UGlobalHotkey\` | Public Domain（Qt 系） | 否 | 否 |
| QtSingleApp | `SandboxiePlus\QtSingleApp\` | BSD（Qt 系） | 否 | 否（单实例方案见 00-architecture §4） |
| SandboxieTools | `SandboxieTools\` | **无许可证文件** | **禁止** | **禁止**（含 ImBox/UpdUtil/MiniDump） |
| Qt 6 | 运行库 | LGPL | 不适用（本项目不用 Qt） | 不适用 |

核实记录（2026-09-27 实读）：

- `LICENSE.Plus:58` — "SandMan is the primary Sandboxie-Plus UI component, provided under a custom license."
- `LICENSE.Plus:59` — "QSbieAPI is a standalone reimplementation of Sandboxie's API using IPC mechanisms…, licensed under the LGPL."
- `LICENSE.Plus:60` — "Sandboxie core components, licensed under the GPL v3."
- `SandboxiePlus\QSbieAPI\LICENSE` 存在（LGPL-3.0 全文：Version 3, 29 June 2007。
  初稿曾误记 LGPL-2.1——波 C 随拷核实 LICENSE 为 V3，波 D1 复核确认并订正本表）。
- `SandboxieTools\` 目录无 LICENSE/README 许可证声明（仅源码文件）→ 按不可用处理。
- `SandboxiePlus\SandMan\LICENSE` 存在，内容为上述 custom license 全文。

## 2. 新项目各模块 → 允许参考的旧代码路径清单

| 新模块（见 04-modules.md） | 允许**复制**（GPLv3） | 允许**参考重写**（LGPL/行为） | 禁止 |
|---|---|---|---|
| `SbieCore/DriverApi`（SbieDll 动态绑定 + IOCTL 调用） | `Sandboxie\core\dll\sbieapi.h`、`sbieapi.c`、`Sandboxie\core\drv\api_defs.h`、`api_flags.h`、`Sandboxie\common\defines.h`（BOXNAME_COUNT 等）、`Sandboxie\common\win32_ntddk.h`（UNICODE_STRING64 等） | `QSbieAPI\SbieAPI.cpp` 的 `IoControl()` 用法（`SbieAPI.cpp:79-83`、`Connect()` `SbieAPI.cpp:319-339`） | — |
| `SbieCore/SvcClient`（LPC 端口 + MSG_HEADER 分块协议） | `Sandboxie\core\svc\msgids.h`、`sbieiniwire.h`、`ProcessWire.h`、`queuewire.h`、`InteractiveWire.h`、`GuiWire.h`、`MountManagerWire.h`、`Sandboxie\core\svc\PipeServer.cpp/.h`（服务端语义参考） | `QSbieAPI\SbieAPI.cpp:447-585`（`CSbieAPI__ConnectPort` / `CSbieAPI__CallServer`：分块、序号、重连）；`QSbieAPI\SbieDefs.h:8`（`SBIESVC_PORT`） | — |
| `SbieCore/Models`（Box/Process/Snapshot/Template/Ini 值语义） | `Sandboxie\core\svcc\…` 无；值语义来自 `Sandboxie\install\Templates.ini`（GPLv3 安装脚本资产，可读） | `QSbieAPI\Sandboxie\SandBox.h/.cpp`（快照文件布局，`SandBox.cpp:353-505`）、`SbieIni.h/.cpp`（ESetMode 语义）、`SbieTemplates.cpp`（模板检查）；`QSbieAPI\SbieAPI.cpp` 的 `ValidateName`（`SbieAPI.cpp:1412-1445`）、`CreateBox`（`SbieAPI.cpp:1459-1477`） | `SandboxiePlus\SandMan\` 一切 `.cpp` |
| `sbie-cli` server（管道、会话、leader、日志泵） | `Sandboxie\core\svc\namedpipeserver.*`（命名管道 API 用法参考，注意那是 SbieSvc 内部用）；`Sandboxie\core\apps\control\`（SbieCtrl 会话模型，GPLv3，可复制） | `QSbieAPI\SbieAPI.cpp` `run()` 泵循环（`SbieAPI.cpp:711-758`）、`GetLog`（`2438-2534`）、`GetQueueReq`（`613-658`）、`TakeOver`（`819-831`） | `SandMan.cpp`、`SandManRecovery.cpp`、`BoxJob.cpp`、`BoxMonitor.cpp`、`BoxTransfer.cpp`、`OnlineUpdater.cpp` |
| `sbie-cli` 命令层/JSON 输出 | — | — | — |
| 构建脚本（`build_oss.bat`、vcxproj） | 根 `build.bat`（`C:\Users\Administrator\Documents\sbie\build.bat`，构建环境编排可模仿）；`Sandboxie\core\svc\SboxSvc.vcxproj`（vcxproj 结构参考，GPLv3） | — | `SandboxieTools\` 内任何构建文件 |

注意两处易错点：

- `QSbieAPI\SbieUtils.cpp`（LGPL）中的 Install/Uninstall/Start/Stop 组件逻辑（KmdUtil 命令行拼装）
  **可以参考**，但被调用对象 `KmdUtil.exe` 来自 core（GPLv3），运行时调用不受限。
- `SandboxieTools\ImBox\ImBox.h` 被 `QSbieAPI\SbieAPI.cpp:44` include（`#include "../../SandboxieTools/ImBox/ImBox.h"`）——
  这是**上游自身的许可证污染点**。本项目**不得** include 该头文件；ImBox 相关功能
  （加密盘沙箱）第一期不做，仅保留 LPC 协议文档（`03-svc-protocol.md` §IMBOX）。

## 3. 禁止清单（绝对不可打开作为编写参考的实现文件）

- `SandboxiePlus\SandMan\` 整目录的一切 `.cpp/.h`（custom license）。例外：为确认"功能面清单"
  （要重写哪些功能）可查阅 `SandMan.h`、`Windows\` 下的窗体类名列表；**不得**将其中任何
  代码结构、字符串表、资源 ID 迁入本项目。功能面以 04-modules.md 已固化的命令树为准。
- `SandboxieTools\` 整目录（无许可证）。
- `SandboxiePlus\MiscHelpers\`、`UGlobalHotkey\`、`QtSingleApp\`（与本项目技术栈无关，引入即
  引入 Qt 依赖，同时 LGPL/BSD 混杂无益）。

## 4. 复制 GPLv3 文件的操作规范

从 `Sandboxie\`（GPLv3）复制头文件/源码片段到 `SandboxieOSS\` 时：

1. 保留原始版权头与 GPLv3 声明不动；
2. 在文件顶部追加一行：`// Vendored from Sandboxie core (GPLv3): <原相对路径> @ <upstream commit/版本 5.73.5>`；
3. 不改动宏内容本身（`api_defs.h` 的枚举顺序是 ABI，见 02-driver-api.md）；
4. 优先只 vendor 头文件（`api_defs.h`、`api_flags.h`、`msgids.h`、`*wire.h`、`defines.h` 需要的
   片段），不 vendor .c/.cpp 实现——实现按行为重写，避免 core 代码在两处漂移。

## 5. 坑记录

1. 初稿曾把 `QSbieAPI\SbieAPI.cpp` 标为"可复制"。纠正：QSbieAPI 是 LGPL，且依赖 Qt；
   只允许**参考协议实现**，源码不得进入 `SandboxieOSS\`。
2. 初稿曾允许参考 `SandMan\BoxMonitor.cpp`（磁盘占用扫描）。纠正：BoxMonitor 位于 SandMan
   目录（custom license），**禁止**。磁盘占用的行为参考改记为：对沙箱根目录做递归大小统计
   （行为等价于资源管理器目录属性），不复刻其实现。
3. `Sandboxie\core\svc\` 目录是 GPLv3（core 的一部分），本文档曾把 "svc" 与 "QSbieAPI" 的
   许可都写成 LGPL——实为：core\svc = GPLv3（可复制），QSbieAPI = LGPL（仅参考）。已修正。
