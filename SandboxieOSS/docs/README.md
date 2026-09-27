# Sandboxie-OSS 用户空间组件 — 文档索引

> 仓库：`C:\Users\Administrator\Documents\sbie\Sandboxie_unbusiness`（Sandboxie-Plus 源码树）
> 新项目根：`SandboxieOSS\`（本目录的上一级）
> 许可证：**GPLv3**（与 Sandboxie core / LICENSE.Classic 一致）

## 项目简介

Sandboxie-OSS 是 Sandboxie-Plus 的 **GPLv3 纯用户空间替代组件**，用于替换 SandMan
（`SandboxiePlus\SandMan\` 整目录，custom license，非 OSI，**禁止复制其任何代码**）所承担的
"常驻用户态管理程序" 角色。因许可证限制，本项目对 SandMan 只做**行为级（功能面）重写**，
不参考、不翻译、不搬运其任何源码。

技术栈与边界（固定约束）：

- **C++20 / MSVC v143 / Win32 API / MSBuild vcxproj**。不使用 .NET，不使用 Qt，不引入任何第三方运行库依赖（含 JSON 库——`--json` 输出使用项目内手写最小实现）。
- 唯一交付物为 **`sbie-cli.exe`**（GUI 暂缓）。同一可执行文件承载两种角色：
  - `sbie-cli --start-server`：服务端组件（下文简称 **server**）。动态加载 `SbieDll.dll`（`LoadLibrary` + `GetProcAddress`，**不做链接期依赖**，理由见 `02-driver-api.md`），直连 Sandboxie 驱动 `SbieDrv.sys` 设备接口，并通过 LPC 端口与 `SbieSvc` 通信；按**登录会话单实例**运行。
  - `sbie-cli`（默认客户端模式，下称 **client**）：与会话内 server 经**命名管道**通信；会话内第一条命令执行时若 server 未运行则**自动拉起**（会话模型参考现版 `Start.exe` / SandMan：一个登录会话一个实例）。
- 与驱动/SbieSvc 的关系：**只调用、不修改**。冻结策略见下节。

## 驱动冻结策略（重要）

**驱动 `SbieDrv.sys`、服务 `SbieSvc.exe`、注入 DLL `SbieDll.dll` 的代码自 2026-09-27 起冻结 3 个月（至 2026-12-27）。**

理由：每次驱动签名需支付 **70 美元/次**（EV 代码签名证书 + Microsoft 硬件开发人员中心 attestation 签发的成本摊销）。冻结期内：

1. **禁止改动** `Sandboxie\core\drv\`、`Sandboxie\core\svc\`、`Sandboxie\core\dll\`、`Sandboxie\core\low\`、`Sandboxie\core\drv` 依赖的 `Sandboxie\common\` 内核侧共享文件；因此也不需要重新构建/签名 SbieDrv.sys。
2. **只改用户态新组件**：`SandboxieOSS\` 下的全部代码。新组件通过已冻结的二进制接口（`SbieDll.dll` 导出 + `SbieSvc` LPC 协议 + 驱动 IOCTL）工作，这些接口在冻结期内保持字节级不变。
3. SbieDll.dll / SbieSvc.exe 随冻结一并不改（它们与驱动同源同签名批次构建）；新组件通过 `SandboxieOSS\build_oss.bat` 单独构建，统一构建 `SandboxieOSS\build_all.bat` 产出 `Installer\SbieOSS_x64\`（core 运行时 + sbie-cli + sbie-gui，构建链与 Plus 断开、签名解耦；见 `05-build.md`）。
4. 若冻结期内发现必须改驱动的缺陷：记录到对应文档的"坑记录"区并挂起，统一在解冻后批量处理、一次签名。

## 文档索引

| 文档 | 内容 |
|---|---|
| [00-architecture.md](00-architecture.md) | server/client 进程模型、WTS 会话识别、会话命名管道命名规范、单实例保证、首命令自动拉起、生命周期与空闲退出、崩溃恢复 |
| [01-license-map.md](01-license-map.md) | 许可证映射：新项目每个部分允许参考的旧代码路径；禁止清单 |
| [02-driver-api.md](02-driver-api.md) | SbieDll.dll 导出的驱动 API：函数清单、调用约定、参数结构体布局、返回码语义、非沙箱要求、GetProcAddress 动态绑定规范 |
| [03-svc-protocol.md](03-svc-protocol.md) | SbieSvc LPC 协议：端口名、MSG_HEADER、MSGID_SBIE_INI_* / PROCESS_* / QUEUE_* 全部请求/回复结构、分块传输、interactive queue 会话队列 |
| [04-modules.md](04-modules.md) | 模块划分（SbieCore 静态库 / sbie-cli.exe / 共享 IPC 定义）、各模块对外接口契约（多 agent 并行开发用）、完整 CLI 命令树与输出格式规范 |
| [05-build.md](05-build.md) | MSBuild 方案布局、统一构建 `build_all.bat`（core/Plus 步骤分类表、8 步流程、组装布局、决策记录）、`build_oss.bat`/`build_gui.bat`/`make_dist.bat`、输出到 `Installer\SbieOSS_x64\`、签名与构建解耦（KernelSigner 独立后续流程） |

## 术语

| 术语 | 含义 |
|---|---|
| 驱动 / SbieDrv | 内核驱动 `SbieDrv.sys`，设备名 `\Device\SandboxieDriverApi` |
| SbieSvc | Windows 服务 `SbieSvc.exe`，LPC 端口 `\RPC Control\SbieSvcPort` |
| SbieDll.dll | 同时是"注入到沙箱进程的 hook DLL"与"SbieApi_* 管理导出"的宿主 DLL |
| box / 沙箱 | Sandboxie.ini 中的一个配置节，同名目录为沙箱根 |
| WTS session | 终端服务登录会话，`ProcessIdToSessionId` 取得的 ULONG id |

## 约定

- 所有协议/API 细节均来自源码实读，文档中给出 `文件:行号` 引用；未确证项一律标注 `TODO-VERIFY`。
- 每份文档末尾有"坑记录"区：发现文档初稿与源码冲突时，修正正文并将冲突记录于此。
