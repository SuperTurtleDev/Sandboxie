# thirdparty — Plus 开源组件源码保全（未参与 sbie-cli 构建）

本目录保存从 `SandboxiePlus\` 拷贝的开源组件源码。**当前 sbie-cli 是零 Qt 依赖的
纯 Win32/C++ 程序，本目录任何组件都不被 `SandboxieOSS.sln` / `build_oss.bat` 引用
或构建**。拷贝目的仅两点：源码保全（GPLv3 项目整体自包含，避免日后 Plus 树变动或
移除导致参考/复用来源丢失），以及未来 GUI 复用（见 docs/01-license-map.md §2 的
复用判定——这四个组件全部允许代码级复用）。

## 组件清单

| 目录 | 来源 | 许可证 | 许可证文件 | 用途（未来 GUI） |
|------|------|--------|-----------|------------------|
| `QSbieAPI\` | `SandboxiePlus\QSbieAPI\` | LGPL-3.0 | `QSbieAPI\LICENSE` | Sandboxie 的 Qt API 封装（IPC/驱动/服务调用、Box/Process/Template 模型）。sbie-cli 的 `SbieCore\SvcClient` 协议实现以其为行为参考（docs/01 §3） |
| `MiscHelpers\` | `SandboxiePlus\MiscHelpers\` | LGPL-3.0 | `MiscHelpers\LICENSE` | Qt 通用工具库（设置、模型/视图、对话框、归档等） |
| `UGlobalHotkey\` | `SandboxiePlus\UGlobalHotkey\` | Public Domain | （无文件，声明见 `UGlobalHotkey\README.md` License 节） | 跨平台全局热键（Qt 扩展） |
| `QtSingleApp\` | `SandboxiePlus\QtSingleApp\` | BSD | （Qt Solutions 组件，任务书标注 BSD；无独立 LICENSE 文件） | 单实例应用支持（QtLocalPeer / QtSingleApplication）。sbie-cli 自身的单实例方案不依赖它（00-architecture §4），仅 GUI 需要 |

许可证版本说明：`QSbieAPI\LICENSE` 与 `MiscHelpers\LICENSE` 文件全文均为
**LGPL Version 3 (29 June 2007)**（随源码逐字拷贝于本目录，以文件为准）。

## 状态说明（重要）

- **全部为 Qt 生态组件；当前 sbie-cli（零 Qt 依赖）不构建它们。**
- 拷贝遵循"没有用的组件不必拷"原则：这四个是**未来 GUI 有用的最小集**——
  GUI 需要 API 层（QSbieAPI）、通用控件（MiscHelpers）、热键（UGlobalHotkey）、
  单实例（QtSingleApp）。
- **明确不拷**的 Plus 侧组件及原因：
  - `SandMan\`（主 GUI）——custom license（非 OSI，LICENSE.Plus），docs/01 §0.3
    禁止除阅读确认功能面外的一切复用；
  - `SandboxieTools\`（ImBox/UpdUtil/MiniDump 等）——目录内无任何许可证文件，
    按不可用处理（docs/01 §1 第 4 条）；
  - Qt 本体（Qt6 *.dll / qmake 工程链）、`MiscHelpers` 之外的归档组件等——
    属于环境依赖而非可保全源码，且 OSS 树不用 Qt。

## 拷贝方式与清洁度

- 拷贝方式：`cp -r` 整目录（`SandboxiePlus\<组件>` → `thirdparty\<组件>`），
  之后剔除生成物。实际剔除仅 2 个文件：
  `UGlobalHotkey\.qmake.stash`（qmake 缓存）、`UGlobalHotkey\.gitignore`
  （上游 SCM 辅助文件，避免影响本仓库忽略规则）。源树中不存在
  obj/x64/Makefile*/.git 等其他生成物。
- 纳入 git 前核查：**无 >1MB 文件**（最大为 MiscHelpers 源文件与 QtSingleApp
  文档，均为文本）；唯二非文本文件是 `QtSingleApp\doc\html\images\qt-logo.png`
  与 `QtSingleApp\doc\images\qt-logo.png`（各 4,075 字节，组件自带文档资源，
  保留）。
- 规模：QSbieAPI 32 文件 / 456KB，MiscHelpers 119 文件 / 1008KB，
  UGlobalHotkey 18 文件 / 88KB，QtSingleApp 54 文件 / 349KB；
  合计 223 文件 / 约 1.9MB。

## 与 dist 的关系

`make_dist.bat` 打包的发行物**不包含**本目录组件（它们只进源码仓，不进运行时
发行包；发行包也不含任何 Qt 运行时）。见 docs/05-build.md 的 dist 章节。
