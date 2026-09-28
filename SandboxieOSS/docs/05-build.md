# 05 — 构建集成

参考实现（实读）：统一构建脚本 `SandboxieOSS\build_all.bat`（8 步，产出
`Installer\SbieOSS_x64\`，构建链与 Plus 断开）；Plus 侧一键构建为仓库外层
`C:\Users\Administrator\Documents\sbie\build.bat`（10 步，产出
`Installer\SbiePlus_x64\`，**保持原样不改、独立并存**）；MSVC 工程结构参考
`Sandboxie\core\svc\SboxSvc.vcxproj`（GPLv3）。

## 1. 目录布局（SandboxieOSS\）

```
SandboxieOSS\
 ├─ SandboxieOSS.sln            # 仅含下两工程
 ├─ SbieCore.vcxproj            # 静态库（DriverApi/SvcClient/QueueClient/Model/Util）
 ├─ sbie-cli.vcxproj            # 控制台 exe（cli/ + server/ + ipcc/）
 ├─ build_all.bat               # 统一构建（§4：core 子集 + sbie-cli + sbie-gui
 │                              #   → Installer\SbieOSS_x64\）
 ├─ build_oss.bat               # sbie-cli 单独构建（§3，可独立运行）
 ├─ build_gui.bat               # sbie-gui 单独构建+发布（09 §5，可独立运行）
 ├─ make_dist.bat               # 最小发行包组装（§8）
 ├─ vendor\                     # 从 Sandboxie core (GPLv3) 复制的 ABI 头（01 §4 规范）
 │   ├─ api_defs.h  api_flags.h          # ← Sandboxie\core\drv\
 │   ├─ msgids.h    sbieiniwire.h
 │   │   ProcessWire.h queuewire.h
 │   │   InteractiveWire.h               # ← Sandboxie\core\svc\
 │   └─ defines_win32.h                  # ← Sandboxie\common\defines.h 需要片段 + win32_ntddk.h 的
 │                                        #   UNICODE_STRING64/MAX_PORTMSG_LENGTH 片段
 ├─ SbieCore\
 │   ├─ DriverApi\  SvcClient\  QueueClient\  Model\  Util\
 ├─ sbie-cli\
 │   ├─ cli\  server\  ipcc\SbieIpc.h
 ├─ sbie-gui\                   # WinUI 3 GUI（09-gui.md）
 └─ dist\                       # make_dist 产物（.gitignore 忽略）
```

vendor 头**作为文件提交**（构建期不再从 `Sandboxie\` 复制，保证冻结基线 5.73.5 的 ABI
不随上游漂移）；每个文件顶部带来源注记（01-license-map §4）。

## 2. 工程要点（两份 vcxproj 的公共约定）

| 项 | 值 |
|---|---|
| ToolsVersion / PlatformToolset | `v143`（VS2022） |
| WindowsTargetPlatformVersion | `10.0`（取机器最新 10 SDK；CI 固定 `10.0.19041.0`） |
| 语言标准 | `C++20`（`<LanguageStandard>stdcpp20</LanguageStandard>`） |
| 字符集 | Unicode |
| 配置 | `Release|x64`（另有 `Debug|x64`；**无 Win32/ARM64 目标**，第一期仅 x64） |
| 运行时 | `/MD`（Release）/`/MDd`（Debug）——sbie-cli 可依赖系统 CRT 分发，不静态链接 |
| SbieCore（Lib） | ConfigurationType=StaticLibrary；`/Zc:__cplusplus`；无预编译头（保持可移植） |
| sbie-cli（Application） | ConfigurationType=Application；链接 `SbieCore.lib`；`/SubSystem:Console`；入口 `wmain` |
| 附加包含目录 | `$(ProjectDir)\..\vendor;$(ProjectDir)\..\SbieCore;$(ProjectDir)\..\sbie-cli` |
| ntdll 符号 | `NtConnectPort/NtRequestWaitReplyPort/NtOpenFile/NtDeviceIoControlFile` 经 `GetProcAddress(GetModuleHandleW(L"ntdll.dll"))` 动态解析（对齐 QSbieAPI 的显式声明方式，`SbieAPI.cpp:861-863` 的 `extern "C"` 声明亦可），**不链接 ntdll.lib**，保持零附加导入库 |
| 警告 | `/W4 /WX`（Release） |

不依赖：Qt、.NET、OpenSSL、7-Zip、WDK。**不触碰驱动工程**（冻结策略，
README；统一构建仅按命令行调用 `SandboxDrv.sln`，不改任何工程文件）。

## 3. `SandboxieOSS\build_oss.bat`（sbie-cli 单独构建）

环境引导与根 `build.bat` 第 0 步同款（vswhere → vcvars64，System32 提前防
GNU find/link 遮蔽），随后构建本 sln 并把产物复制进统一布局：

- 输出目录约定：vcxproj 的 `OutDir` 保持默认 `x64\Release\`，由脚本复制到
  `Installer\SbieOSS_x64\`（保证独立可构建，不把 OutDir 直接指到 Installer）。
- 布局目录不存在时脚本自建（统一布局由 `build_all.bat` 日常产出，但单独跑
  `build_oss.bat` 不再要求先跑完整链）。
- sbie-cli.exe 运行时需要同目录的 `SbieDll.dll` 与 `SbieMsg.dll`（由统一布局
  提供），不额外复制。
- 脚本为规范本体（全 ASCII + CRLF），本文不再内嵌全文以防双处漂移。

## 4. 统一构建 `build_all.bat`（构建链与 Plus 断开）

**用法**：`cmd /c SandboxieOSS\build_all.bat [--no-pause]`
**产物**：`Installer\SbieOSS_x64\`（自含 OSS 运行布局，git 忽略）

### 4.1 外层 build.bat 10 步分类表（core 必需 / Plus 专属）

外层 `sbie\build.bat` 是 Plus 的 10 步一键构建（CI 镜像）。逐步实读分类：

| 外层步骤 | 脚本 / sln | 分类 | 判据与说明 |
|---|---|---|---|
| 0 | vswhere→vcvars64；检查 7-Zip / SDK 19041 / WDK | **core**（VS+WDK 部分） | 7-Zip（translations.7z 打包）与 SDK 19041（SbieShell 钉死）检查仅 Plus 需要，OSS 链不检查；WDK 任何已装 10.x km 头即可 |
| 1 | `Sandboxie\SandboxDll.sln` Win32 SbieRelease | **core** | 32 位 SbieDll/SbieSvc = 布局 `32\` WOW64 对（沙箱内 32 位应用注入用） |
| 2 | `Sandboxie\Sandbox.sln` x64 SbieRelease | **core** | SbieSvc/SbieDll/SbieMsg/RpcSs/DcomLaunch/KmdUtil/Start/SbieIni/SboxHostDll/BITS/WUAU/Crypto/SbieCtrl（SboxDrv 列于 sln 但**无 Build.0**，不随建） |
| 3 | `Sandboxie\SandboxDrv.sln` x64 SbieRelease | **core** | SboxDrv（WDK 驱动）+ SboxMsg + Parse |
| 4 | `SandboxiePlus\install_qt.cmd` / `install_jom.cmd` | Plus | Qt 6.8.3 / jom 下载 |
| 5 | `SandboxiePlus\qmake_plus.cmd`（SandMan + Qt 库） | Plus | Qt/qmake/moc 链 |
| 6 | `SandboxiePlus\SbieShell\SbieShell.sln` | Plus | 资源管理器扩展（NuGet 还原） |
| 7 | `SandboxieTools\SandboxieTools.sln` | **拆分** | `ImBox` = core 运行时（SbieSvc MountManager 以 `<安装目录>\ImBox.exe` 拉起，core\svc\MountManager.cpp:848-887）；UpdUtil（Plus 更新器）/ MiniDump（Plus 崩溃收集）= Plus 侧 |
| 8 | `Installer\fix_qt5_languages.cmd` / `get_openssl.cmd` / `get_7zip.cmd` | Plus | Qt 翻译 / OpenSSL / 7z.dll 运行库下载 |
| 9 | `Installer\copy_build.cmd` | Plus | 合并出 SbiePlus_x64 全量布局（Qt6*.dll、SandMan、translations.7z、SbieShellExt…显式 copy 清单，sbie-cli 不在其列） |
| 10 | `Installer\get_assets.cmd` | Plus | Inno 安装器素材 |
| — | `SandboxieOSS\build_oss.bat` / `build_gui.bat` / `make_dist.bat` | OSS | sbie-cli / sbie-gui / dist（本仓库新组件） |

### 4.2 build_all.bat 的 8 步流程

```
[0] 环境：System32 提前 → vswhere → vcvars64 → msbuild/cl 在位；
    WDK = 任意已装 "%ProgramFiles(x86)%\Windows Kits\10\Include\*\km"；
    VC 运行库三件套来源 %VCToolsRedistDir%\x64\Microsoft.VC143.CRT 在位
[1] SandboxDll.sln  /p:SbieRelease /p:Win32   （WOW64 对）
[2] Sandbox.sln     /p:SbieRelease /p:x64     （x64 core 用户态）
[3] SandboxDrv.sln  /p:SbieRelease /p:x64     （WDK 驱动）
[4] SandboxieTools.sln /t:ImBox /p:Release /p:x64（工程目标选择）
[5] 组装 Installer\SbieOSS_x64：原地清空（根可能被外部进程 CWD
    钉住，children-first 清空等价全新）→ core 运行时面（下表）
    → Templates.ini（源树裁剪版）+ templates\ 树（源树 robocopy /MIR）
[6] call build_oss.bat                         （+ sbie-cli.exe）
[7] call build_gui.bat                         （+ sbie-gui\ 整目录）
[8] 校验布局完整性（缺任一即 FAIL）→ 逐文件打印（路径+字节+合计）
```

### 4.3 组装布局（`Installer\SbieOSS_x64\`，文件面 = make_dist 21 文件清单）

| 布局路径 | 来源 | 说明 |
|---|---|---|
| `SbieSvc.exe` `SbieDll.dll` `SbieMsg.dll` `KmdUtil.exe` `SandboxieRpcSs.exe` `SandboxieDcomLaunch.exe` | `Sandboxie\Bin\x64\SbieRelease\` | x64 core 运行时（判据见 §8.2） |
| `ImBox.exe` | `SandboxieTools\x64\Release\` | 磁盘映像/ram box 运行时 |
| `Templates.ini` | `SandboxieOSS\Templates.ini`（源树裁剪版） | 冻结运行时骨架（§8.2 A/B 证据；`Sandboxie\install\Templates.ini` 为 legacy 全量源，仅迁移参考用） |
| `msvcp140.dll` `vcruntime140.dll` `vcruntime140_1.dll` | `%VCToolsRedistDir%\x64\Microsoft.VC143.CRT` | 目标机免装 VC++ redist（UCRT 为 Win10+ 自带） |
| `32\SbieDll.dll` `32\SbieSvc.exe` | `Sandboxie\Bin\Win32\SbieRelease\` | WOW64 对 |
| `driver\SbieDrv.sys` | `Sandboxie\Bin\x64\SbieRelease\SbieDrv.sys` | **WDK 构建原生产出原样放置** |
| `driver\SbieDrv.inf` | `Sandboxie\core\drv\SbieDrv.inf`（如 Bin 出现 inf 则优先构建产物） | WDK 构建本身不产 inf；源文件与旧 Plus 布局所发 inf 逐字节一致（diff 验证） |
| `driver\SbieDrv.cat`（如构建产出） | Bin 同目录 | stock WDK `/t:build` 不产 cat（Inf2Cat 未启用）→ 当前布局无 cat |
| `sbie-cli.exe` | build_oss.bat | OSS CLI |
| `sbie-gui\`（整目录） | build_gui.bat（dotnet publish） | OSS GUI（235 文件，.NET 8 框架依赖 + WinAppSDK self-contained） |

明确**不进**布局：SandMan/Qt6*.dll/QSbieAPI/MiscHelpers/UGlobalHotkey/QtSingleApp、
platforms\styles\tls\、OpenSSL、7z.dll、translations.7z、SbieShellExt.dll、
SbieShellPkg.msix、UpdUtil、MiniDump、Start/SbieIni/SboxHostDll/SbieCtrl/BITS/WUAU/
Crypto（判据同 §8.2"明确不带"）、全部 pdb。

### 4.4 决策记录（实读后选定，构建链选型）

1. **三个 core sln 整建，不用 `/t:` 抽项目**：实读发现 core 各 vcxproj 内
   **零 `ProjectReference`**（依赖在 sln 配置层 / 链接预构建 lib，如 SboxSvc 链
   Common.lib）。sln 级 `/t:单项目` 不会自动带依赖，干净机器上会断链。而
   SandboxDll/Sandbox/SandboxDrv 三个 sln **本身就只含 core 工程**（无任何
   Qt/SandMan 工程）——"core 子集"由选 sln 达成，与 Plus 断开由"不调 Plus
   步骤"达成，无需在 core sln 内抽目标。代价：多编译 Start/SbieCtrl 等
   core 副产品（不入布局，无害）。
2. **SandboxieTools.sln 用 `/t:ImBox`**：该 sln 混有 Plus 侧 UpdUtil/MiniDump；
   ImBox.vcxproj 零 ProjectReference、仅链系统库（ntdll/Wtsapi32/Advapi32/
   Bcrypt），单目标安全（实跑验证）。这是全链唯一的工程目标选择点。
3. **不改任何工程文件**：CORE 与 SandboxiePlus 工程文件零触碰；全部选择经
   msbuild 命令行参数完成。
4. **inf 来源**：SboxDrv.vcxproj 无 inf 条目（WDK /t:build 不产 inf/cat）；取
   `Sandboxie\core\drv\SbieDrv.inf` 源文件原样（与旧 `SbiePlus_x64\driver\
   SbieDrv.inf` diff 逐字节一致）；脚本保留"Bin 出现 inf 优先取构建产物"的
   条件分支，防 WDK staging 行为变化。
5. **cat 条件携带**：`if exist Bin\...\SbieDrv.cat` 才复制——构建产 cat 就带
   （当前不产）。无任何 cat/签名硬性断言（见 §4.5）。
6. **VC 三件套**：与 copy_build.cmd 同源（`%VCToolsRedistDir%\x64\
   Microsoft.VC143.CRT`），但只取 make_dist 清单的三件（不带 concrt140）。
7. **外层 build.bat 不改**：Plus 路径独立并存，`Installer\SbiePlus_x64\`
   继续由它产出；两布局互不覆盖。

### 4.5 签名与构建解耦

- 构建链**零签名调用**：不调 signtool/inf2cat，无签名验证门禁。
  `driver\` = WDK 构建原生产出原样放置（sys+inf，构建产 cat 就带）。
- `make_dist.bat --verify` 对 cat/签名**仅报告不门禁**（无 cat → 报告
  "WDK build output as-is, signing decoupled"；有 cat → 逐文件打印
  Authenticode 状态，informational）。
- **内核签名是独立后续流程**：从 KernelSigner 手工存档取 sys+cat 配对
  （旧 `Installer\SbiePlus_x64\driver\` 的 Lab 配对签名三元组即该存档的
  实例——注意其 sys 与本链 WDK 产出是不同构建，混用会导致 cat 校验失败，
  替换时必须整组配对）。流程：构建产出 → 存档配对校验 → 替换/补充
  `driver\` 内容，全程不在构建脚本内。
- sbie-cli.exe / sbie-gui.exe 为普通用户态文件，走常规代码签名，同样独立
  于构建（开发期允许未签名）。

## 5. IDE / 本地开发

- 打开 `SandboxieOSS\SandboxieOSS.sln` 即可独立开发 sbie-cli（运行时依赖
  `SbieDll.dll` 等由统一布局提供——调试时把工作目录设为
  `Installer\SbieOSS_x64\`，或用 02 §1 的显式目录加载传参）。
- sbie-gui 单独开发见 09-gui.md §5（`build_gui.bat` 可独立运行）。
- 单元测试（第二期）：`SbieCore.Tests.vcxproj`（doctest 头文件库随仓库
  vendored，单头、MIT、允许引入——**例外需在 01-license-map 登记**；或直接
  手写断言宏避免例外）。Json/PathMapper/ValidateName/Snapshots(临时目录)
  可全离线测试。

## 6. 坑记录

1. **根 build.bat 不在仓库内**：外层一键构建位于仓库上一级
   `C:\Users\Administrator\Documents\sbie\build.bat`（Plus 路径，保持原样）。
   OSS 统一构建入口为仓库内 `SandboxieOSS\build_all.bat`，不再有"并入根
   build.bat 第 11 步"的方案（旧方案废弃：两条构建链独立并存）。
2. **`copy_build.cmd` 不会自动带上 sbie-cli.exe**：Installer 合并脚本是显式
   copy 清单（`Installer\copy_build.cmd:132-146`）；OSS 布局由 build_all.bat
   自行组装，不依赖 copy_build.cmd。
3. vcvars64 之前把 System32 提到 PATH 最前（根 build.bat:40 的做法）是防
   Git Bash 的 GNU find/link 遮蔽；build_all/build_oss 已保留同款两行，勿删。
4. `/W4 /WX` 下 vendored 的 core 头（C 风格、老式强制转换）会报噪音——vendor
   头统一以 `#pragma warning(push,3)` 包裹（在 vendor 侧包裹，不在编译选项上
   全局降级）。
5. **（M1 实测）.bat 必须纯 ASCII**：cmd.exe 按 OEM 代码页（中文系统 = GBK）
   解析 .bat；UTF-8 中文注释的尾部字节会与 CR/LF 配对吞掉换行，把下一行
   命令拼进上一行（实测出现 `'ink' 不是内部或外部命令` 之类碎片）。
   build_all/build_oss/build_gui/make_dist 四脚本全 ASCII + CRLF。
6. **（M1 实测）vcxproj 必须开 `/utf-8`**：源码为 UTF-8 无 BOM（含中文注释），
   MSVC 默认按系统代码页（GBK）读——多字节尾字节同样吞换行。两工程已加
   `<AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>`。
   新源文件保持 UTF-8（无 BOM 亦可），勿删该开关。
7. **（实现偏离 §1 目录树，已定案）**：vcxproj 实际位于子目录内——
   `SbieCore\SbieCore.vcxproj`、`sbie-cli\sbie-cli.vcxproj`。sln 仍在
   `SandboxieOSS\` 根；两工程 OutDir 统一为 `SandboxieOSS\x64\$(Configuration)\`。
8. **（M1 实测）stderr 与 `_O_BINARY`**：stdout/stderr 置 binary 后 **禁止
   `fwprintf`**（宽流在 binary 模式写裸 UTF-16）。诊断输出统一走
   `util::PrintErrLineUtf8(WideToUtf8(...))` 写 UTF-8 字节。
9. **（统一构建实测）括号路径 + 括号块的 cmd 解析炸弹**：`%VCToolsRedistDir%`
   含 `Program Files (x86)`，在 `if ... ( echo ... %VAR% ... )` 块内展开时其中的
   `)` 会提前闭合块——报 `此时不应有 \Microsoft。`，且**解析错误以 exit 0
   静默退出**（无 BUILD FAILED）。规则：块内 echo 含括号路径的变量必须加引号
   （`echo ... "%VAR%"`）。
10. **（统一构建实测）Git Bash 调 .bat 的 MSYS 转换**：`MSYS_NO_PATHCONV=1`
    会使 `cmd //c` 的 `//c` **不**转换为 `/c`——cmd 当成交互式启动，打完
    banner 吃到 EOF 即 exit 0，**什么都没跑**。正确姿势：不加
    MSYS_NO_PATHCONV，用 `cmd //c "path\\to\\script.bat" --no-pause`，
    重定向日志用正斜杠路径。
11. **（统一构建实测）任意 sbie-cli 命令会预拉起 server**：CLI 的 Run() 对
    一切命令 EnsureConnected——`server stop` 后紧接着跑 `server status` 会
    立刻复活一个新 server 实例（uptime=0）。冒烟脚本顺序应为
    start→status→stop，stop 后不再跑会连接的命令（`--no-server` 除外）。
12. **（实读）Sandbox.sln 中的 SboxDrv 无 Build.0 条目**：驱动工程列于 sln
    但不随 sln 构建（需 WDK 环境），必须单独建 `SandboxDrv.sln`——build_all
    的 [3] 独立成步即为此。

## 7. 验收记录

### 7.1 M0+M1（2026-09-27，历史——当时输出并入 SbiePlus_x64）

机器：Windows 10 x64（10.0.26100），VS2022 BuildTools v143，SDK 10.0
（机器最新 10.0.26100.0），本机装有 Sandboxie-Plus 5.73.5 且驱动/SbieSvc
在运行。

构建：`SandboxieOSS\build_oss.bat` 全绿（SbieCore + sbie-cli，
0 error / 0 warning，/W4 /WX）；Debug|x64 同样通过。

实跑（同目录 SbieDll.dll 5.73.5）：

```
> sbie-cli version
sbie-cli 0.1.0
driver 5.73.5 (abi 0x57230, alive)
svc 5.73.5 (abi 0x57230)

> sbie-cli status          → driver/svc/boxes 全量输出
> sbie-cli box list        → DefaultBox / New_Box（FILE_ROOT 已 NT→DOS 化）
> sbie-cli proc list       → 沙箱内 cmd.exe + RpcSs/DcomLaunch 桩
```

- `svc 5.73.5`：SbieSvc LPC 分块协议（`NtConnectPort` +
  `MSGID_SBIE_INI_GET_VERSION`）实跑成功。
- `--json` 各命令输出过 `python json.load` 机器校验。
- 降级路径：空 `--sbie-dll-path` / 无 SbieDll 目录 → 回退搜索链成功；
  未知全局选项 exit 2；未实现命令 exit 1；server 依赖命令 exit 4。

### 7.2 统一构建（2026-09-27 实测，`build_all.bat --no-pause`）

```
[0/8] VS=BuildTools 2022 / WDK km=10.0.26100.0
[1/8] SandboxDll.sln Win32  → SbieDll.dll/SbieSvc.exe(Win32) 等 6 工程
[2/8] Sandbox.sln x64       → SboxSvc/SboxDll/SboxMsg/com 桩/KmdUtil 等
[3/8] SandboxDrv.sln x64    → SbieDrv.sys (WDK)
[4/8] SandboxieTools /t:ImBox → ImBox.exe（UpdUtil/MiniDump 未构建）
[5/8] 组装 Installer\SbieOSS_x64
[6/8] build_oss.bat        → sbie-cli.exe
[7/8] build_gui.bat        → sbie-gui\（dotnet publish 235 文件）
[8/8] 布局校验+打印         → 338 files, 154,827,002 bytes
BUILD SUCCEEDED（增量 24 s；冷构建为分钟级）
```

- **布局面**：根 12 文件（sbie-cli + 8 core + Templates.ini + VC 三件套）、
  `32\` 2 文件、`driver\` sys+inf（无 cat）、`sbie-gui\` 整目录。
- **冒烟（新布局）**：`sbie-cli --no-server version` → cli 0.1.0 /
  driver 5.73.5 (abi 0x57230, alive) / svc 5.73.5，exit 0；`box list`
  exit 0；`server start`（幂等，已运行 pid）/`server status`/`server stop`
  exit 0（守护进程行为见 §6 坑 11）。
- **GUI**：`sbie-gui.exe --selftest` → **31 passed, 0 failed**，exit 0
  （CliBridge 从 exe 祖先目录定位到新布局 `sbie-cli.exe`）。
- **make_dist --verify** 全绿（明细 §8.4）。
- **零 Plus 痕迹**：构建日志 grep
  `qmake|sandman|qt6|qt_|sbieshell|openssl|7-zip|7z.dll|install_qt|copy_build|get_assets|translations.7z|UpdUtil|MiniDump`
  → 0 命中。

## 8. dist 发行包（`make_dist.bat`）

`SandboxieOSS\make_dist.bat` 从统一布局组装 **Sandboxie-OSS 最小发行包**：
`sbie-cli` + core 运行时（GPL core 二进制）+ 许可文本，不含任何 Plus 专属
组件、Qt 运行时与 sbie-gui（GUI 随布局分发，见 09-gui.md）。

### 8.1 依赖前提

`build_all.bat` 已跑完：`..\Installer\SbieOSS_x64\` 布局在位（core 运行时 +
sbie-cli.exe）。脚本对**全部输入文件逐一核存在**，缺任一直接 `DIST FAILED`。

### 8.2 清单判据（逐文件核实记录，2026-09-27）

判据 = **sbie-cli 运行 + 沙箱功能所需**；核实手段 = `dumpbin /dependents` +
源码审计（core\dll、core\svc、sbie-cli/SbieCore）。

| dist 内路径 | 来源（layout 相对） | 判据 |
|---|---|---|
| `sbie-cli.exe` | 根 | 交付物本体。**静态导入仅系统 DLL + VC 运行时**，无任何 Sandboxie 二进制的链接期依赖；SbieDll.dll 由 `drv::LoadSbieDll` 运行时 LoadLibrary（同目录为搜索候选 2 号，见 02 §1） |
| `SbieDll.dll` | 根 | sbie-cli 动态加载（proc start 等注入路径）；同时是 SbieSvc/KmdUtil/SandboxieRpcSs/SandboxieDcomLaunch/SbieIni/Start/SboxHostDll 的静态导入 |
| `SbieSvc.exe` | 根 | 沙箱服务（LPC 端口、驱动代理） |
| `SbieMsg.dll` | 根 | 消息文本资源（资源 DLL，源 `Sandboxie\msgs\msgs.mc`；布局无 msgs 文本目录） |
| `KmdUtil.exe` | 根 | `sbie-cli maint start/stop --driver` 经 `SbieDll_RunFromHome` 从安装目录拉起（`SbieCore\Model\Maintenance.cpp:99-148`） |
| `ImBox.exe` | 根 | 磁盘映像/ram box 运行时依赖：SbieSvc MountManager 拉起（`Sandboxie\core\svc\MountManager.cpp:848-887`）。ImBox 源码在 SandboxieTools（无许可证，禁复用），**只随 GPL core 布局分发其二进制** |
| `SandboxieRpcSs.exe`、`SandboxieDcomLaunch.exe` | 根 | 沙箱内 COM/RPC 桩，`core\dll\ipc_start.c`：请求 `epmapper` 端口即自动启动（除非 `NoSandboxieRpcSs=y`）——默认沙箱功能必需件（x64） |
| `Templates.ini` | 根 | 冻结内核在每次配置重载时读 `(Home)\Templates.ini`（`core\drv\conf.c` `Conf_Read`）；骨架节（`[TemplateDefaultPaths]` 等）是载荷路径（§8.2 修订 2 的 A/B 证据）。V2 CLI 自身模板操作走 `templates\` 树（`SbieCore\Model\Templates.cpp:215`），不经此文件 |
| `msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` | 根 | sbie-cli 与 ImBox 的 VC 运行时导入（目标机未装 VC++ 2015-2022 redist 时必需；UCRT 为 Win10+ 系统自带） |
| `driver\`（整目录原样） | `driver\` | WDK 构建产出原样：sys+inf 必需；**cat 如存在随目录拷贝，无签名断言**（签名解耦，§4.5——内核签名走 KernelSigner 手工存档的独立流程） |
| `32\SbieDll.dll`、`32\SbieSvc.exe` | `32\` | WOW64 对：沙箱内 32 位应用注入用 |
| `LICENSE-OSS` | `..\LICENSE.Classic` | GPLv3 全文（core 同源许可） |
| `THIRD-PARTY.md` | `thirdparty\README.md` | thirdparty 组件来源/许可证/状态说明的拷贝 |
| `docs\README.md` | `docs\README.md` | OSS 项目文档索引（含许可证声明与冻结策略） |
| `VERSION.txt` | 生成 | sbie-cli 版本（实跑 staged 副本 `version` 解析，兼作同目录 SbieDll 冒烟）+ SbieDrv 文件版本 + 构建日期 |

**明确不带**（判据核实为"非必需"或"Plus 专属"）：`SandMan.exe`、`Qt6*.dll`、
`QSbieAPI.dll`、`MiscHelpers.dll`、`UGlobalHotkey.dll`、`qtsingleapp.dll`、
`platforms\`、`styles\`、`tls\`、`7z.dll`、`translations.7z`、
`troubleshooting.7z`、`SbieShellExt.dll`、`SbieShellPkg.msix`、`UpdUtil.exe`、
`MiniDump.exe`、`SbieCtrl.exe`（旧 UI）、
`SboxHostDll.dll`/`SbieIni.exe`/`Start.exe`（V2 exec 经 SvcClient
RunSandboxed/SbieDll 路径实现同类功能，dumpbin 证实 sbie-cli 不依赖它们）、
全部 `.pdb`、`sbie-gui\`（GUI 随布局 `Installer\SbieOSS_x64\sbie-gui\` 分发，
最小 dist 不含——09-gui.md）、`Sandboxie.ini`/`SbieSettings.ini`（按 core
规则首次运行生成）。

**V2 部署层修订（2026-09-28，实测定论）**：

1. **五个沙箱内模拟服务全带**（BITS/WUAU/Crypto 从"不带"改为随 dist）。
   实读拉起链：RpcSs/DcomLaunch = `core\dll\ipc_start.c:61-115` 任何非
   RpcSs 盒内进程触碰 `epmapper` 端口按需拉起 RpcSs，RpcSs 请求
   `actkernel` 拉起 DcomLaunch（仅 `NoSandboxieRpcSs=y` 关闭）——默认
   沙箱必需；BITS/WUAU/Crypto = `core\dll\scm.c:1064-1109`
   （Scm_IsBoxedService **硬编码** bits/wuauserv/cryptsvc/MSIServer/
   TrustedInstaller）+ `scm_create.c:972-1009`（盒内 StartService 上述
   服务名 → SbieDll_RunFromHome 启动对应桩 exe）——**非模板门控**，
   缺失即盒内 Windows Update/BITS 下载/证书服务静默失败。三桩合计约
   0.4MB，全带（build_all 布局面与 make_dist ROOT_FILES 已同步）。
2. **Templates.ini 部署裁剪版（全量必须论已被干净 A/B 矩阵推翻，
   2026-09-28 重测定论）**：部署原则 = `templates\` 放已迁移配置
   （V2 树），`Templates.ini` 放**未迁移存量 + 冻结运行时启动骨架**
   ——不是同一份内容两处放。裁剪版（951 行 / 45 节 / 0 条
   `Template=` 引用）= 7 个骨架节 + 36 个未迁移 `[Template_*]` 壳 +
   空的 `[DefaultTemplates]`（原 10 条引用的目标模板全部已迁移，
   死引用只会在每次内核重载时产生 SBIE1411 告警）。

   **A/B 实验矩阵**（现场：生产安装 `C:\Program Files\Sandboxie-Plus\`
   作为驱动 Home；每次换文件后 `sbie-cli sync-config <box>` 触发
   `ReloadConf` 内核重读；探针 = `exec <box> --wait cmd /c exit 7`
   期待精确 rc=7、`cmd '/c echo mark> C:\marker.txt'` 查盒内
   drive\C\marker.txt、`tasklist` 查功能）：

   | 变体 | 内容 | standard | app-plus | hardened-plus |
   |---|---|---|---|---|
   | A0 | legacy 全量 4612 行 | rc=7/写/查全过 | — | — |
   | B1 | 仅 `[DefaultTemplates]` 10 行（最小 stub） | **rc=127 死亡** | — | — |
   | B2 | B1 + `[TemplateDefaultPaths]` | 全过 | — | — |
   | B3 | 仅 `[TemplateDefaultPaths]` | 全过 | **rc=1 失败** | — |
   | B4 | 空文件（无任何节） | **rc=127 死亡** | — | — |
   | B6 | 裁剪版（骨架全集+壳，0 引用） | rc=7/写/查全过 | rc=7/写/查全过 | rc=7/写/查全过 |
   | A/A | 还原全量 | rc=7 | rc=7 | — |

   **根因链（B1/B4 的 127）**：驱动日志 SBIE2112
   `OpenSection (C0000022) \KnownDlls\kernel32.dll`——缺
   `[TemplateDefaultPaths]` 即缺 `OpenIpcPath=\KnownDlls\*`，
   盒内进程映射 kernel32 被拒 → 进程初始化夭折 → 退出码 127。
   前任观察到的"SYSTEM 上下文日志洪泛"实为 SBIE1411（缺模板
   告警，pid=4 SYSTEM 上下文；本机系统 ini [GlobalSettings] 的
   8 条 legacy `Template=` 在任何裁剪文件下都会触发，wrn 级
   非致命）。

   **结论**：(a) 全量 4612 行**不必须**——400 个已迁移模板本体
   在该文件里纯属死重；(b) 骨架节**必须**——标准盒最低需
   `[TemplateDefaultPaths]`，plus/app 型还需 `[TemplatePModPaths]/
   [TemplateAppCPaths]`（B3 的 app-plus rc=1 → B6 全过即证），
   故裁剪版保留全部 7 个骨架节；(c) `--verify` 以**与源资产
   字节一致**（fc /b）为门，非体积阈值。

   **可复现命令**（任何人可跑；结束时必须还原生产文件）：
   ```
   cp "C:\Program Files\Sandboxie-Plus\Templates.ini" <backup>
   cp <变体文件> "C:\Program Files\Sandboxie-Plus\Templates.ini"
   sbie-cli create-box %LOCALAPPDATA%\SandboxieOSS\boxes\stubT
   sbie-cli sync-config %LOCALAPPDATA%\SandboxieOSS\boxes\stubT
   sbie-cli --settle 60 exec %LOCALAPPDATA%\SandboxieOSS\boxes\stubT --wait cmd /c exit 7   # 期待 7；127=骨架缺失
   sbie-cli --settle 60 exec %LOCALAPPDATA%\SandboxieOSS\boxes\stubT --wait cmd '/c echo m> C:\marker.txt'
   sbie-cli log --last 200   # 查 SBIE1411/SBIE2112（需先停 SandMan）
   sbie-cli unregister %LOCALAPPDATA%\SandboxieOSS\boxes\stubT
   cp <backup> "C:\Program Files\Sandboxie-Plus\Templates.ini" && sbie-cli sync-config ...  # A/A 还原
   ```
   （Git Bash 下注意 `MSYS_NO_PATHCONV=1`，否则 `/c` 被改写成
   `C:/` 导致 cmd 交互式挂起。）
3. **V2 模板树随 dist 分发**：`templates\`（406 文件）= SBIE_TEMPLATE_DIR
   初始内容，**由 build_all.bat 从源树 `SandboxieOSS\templates\`
   robocopy /MIR 镜像进布局**（不再手工拷贝/补文件；make_dist
   --verify 对全树做 SHA256 对比）；sbie-cli 另有 `<exe 目录>\templates`
   内置兜底根（环境变量根优先），dist 零配置自洽。

### 8.3 使用法

```
cmd /c SandboxieOSS\make_dist.bat            REM 组装 + 打包 + 控制台清单
cmd /c SandboxieOSS\make_dist.bat --verify   REM 另解压 zip 到 dist\_verify：
                                             REM   实跑解压副本（version 命令）；
                                             REM   driver cat/签名如有则逐文件
                                             REM   报告（informational，非门禁）
```

输出：`dist\Sandboxie-OSS-x64\`（staged 树）与 `dist\Sandboxie-OSS-x64.zip`
（Compress-Archive，含顶层目录）。控制台逐文件打印 **相对路径+字节大小+合计**。
`dist\` 生成物不入 git（`dist\.gitignore` 忽略全部）。dist 为可选步骤、不并入
默认构建（`build_all.bat` 末尾无调用；`build_oss.bat` 末尾保留提示行）。

### 8.4 实跑验收记录（2026-09-27 统一布局；2026-09-28 裁剪链复验）

- `make_dist.bat --verify` 全绿（2026-09-28 复验，裁剪版链）：
  **429 文件 / 9,159,433 字节**（zip 3,276,861 字节；较 2026-09-27 的
  20 文件 / 10,268,670 字节——多出的 406 文件 = templates\ 树随 dist
  首次进入 staged 面，Templates.ini 从 152,006 B 全量降为裁剪版）。
  （统一前为 21 文件——多出的 `driver\SbieDrv.cat` 来自 KernelSigner
  存档；现按"构建产 cat 就带"规则，stock WDK 构建无 cat。）
- 新不变量全过：dist `Templates.ini` 与源资产 **fc /b 字节一致**；
  `templates\` 全树 **SHA256 一致**（406 文件）；五个服务桩在场。
- 一致性总 diff（2026-09-28，Get-FileHash 全树对比）：源
  `SandboxieOSS\Templates.ini` = 布局 = staged = `_verify` 解压副本
  （同哈希 BE7410210CEE…）；templates 树 源=布局=staged（406 文件
  IDENTICAL）；staged=`_verify`（429 文件 IDENTICAL）；布局→staged
  根 15 文件逐一同哈希——**diff 为零**。
- driver 报告行：`driver report : no cat present - WDK build output as-is,
  signing decoupled`（--verify 输出，非门禁）。
- 解压副本独立运行：`dist\_verify\Sandboxie-OSS-x64\sbie-cli.exe
  --no-server version` → `sbie-cli 2.0.0 / driver 5.73.5 (abi 0x57230,
  alive) / svc 5.73.5`，exit=0——同目录 SbieDll.dll 探测成功。
- `--sbie-dll-path` 显式路径与回退搜索链实测同前（§7.1）。
- 已知边界：本机驱动/服务在系统级安装，"driver alive"来自系统实例；
  发行包安装到裸机后需先 `maint start`（管理员）装驱动/服务方可沙箱；
  裸机部署需启用测试签名或先完成 §4.5 的内核签名流程（WDK 产出为
  测试签名）。

## 9. 流程规范（编排者指令固化，2026-09-28）

以下三条为编排者裁定的强制流程，本节为其固化位置。违反任一条的
变更视为污染，后续 agent 有权也有义务回滚。

1. **dist 内容必须由构建脚本从源树推导**。`Installer\SbieOSS_x64\`
   布局与 `dist\` 发行包内的每一个文件都必须能回答"来自源树哪个
   文件、被哪个脚本拷贝"。禁止向布局/dist 手工放置或修改文件后不
   回写源树或构建脚本——手工应急后必须补脚本并使 `build_all.bat`
   + `make_dist --verify` 全绿。当前推导链：源树
   `SandboxieOSS\Templates.ini` →（build_all [5] copy）→ 布局 →
   （make_dist ROOT_FILES）→ dist，`--verify` 以 fc /b 字节一致为
   门；源树 `SandboxieOSS\templates\` →（build_all [5] robocopy
   /MIR）→ 布局 templates\，`--verify` 对全树 SHA256 对比。
2. **"外部依赖不可行/必须"类结论必须附可复现 A/B 实验命令**。任何
   形如"X 不可行因为 Y"、"必须保留 Z 否则坏"的部署决策，其依据
   实验必须写成任何人可独立重跑的命令序列（含环境前置、每步期待
   值、现场还原步骤），标注在对应文档节（如 §8.2 的 A/B 矩阵与
   可复现命令块）供编排者复核。只给结论不给命令的"实测"一律按
   未验证处理。结论被新实验推翻时，旧文档**改写**并保留勘误注记
   （示例：docs/11 E2 的 446 节勘误），不得留双结论。
3. **测试 agent 的"部署产物"断言必须与源树 diff 验证**。测试报告
   中一切"dist/布局含（或不含）某文件/某内容"的断言，出具前必须
   用确定性对比（fc /b、robocopy /L、Get-ChildItem+Get-FileHash 等）
   与源树或构建产物实际核对，并在报告中给出核对命令与输出摘要；
   禁止以"上次部署应该是…"式推断充当验证。
