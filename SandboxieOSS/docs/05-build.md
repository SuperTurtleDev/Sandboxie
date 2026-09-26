# 05 — 构建集成

参考实现（实读）：根构建脚本 `C:\Users\Administrator\Documents\sbie\build.bat`
（10 步一键构建，vcvars64 环境、`msbuild /p:Configuration=… /p:Platform=x64` 模式，
产物并入 `Installer\SbiePlus_x64\`）；MSVC 工程结构参考
`Sandboxie\core\svc\SboxSvc.vcxproj`（GPLv3）。

## 1. 目录布局（SandboxieOSS\）

```
SandboxieOSS\
 ├─ SandboxieOSS.sln            # 仅含下两工程
 ├─ SbieCore.vcxproj            # 静态库（DriverApi/SvcClient/QueueClient/Model/Util）
 ├─ sbie-cli.vcxproj            # 控制台 exe（cli/ + server/ + ipcc/）
 ├─ build_oss.bat               # 独立构建脚本（§3）
 ├─ vendor\                     # 从 Sandboxie core (GPLv3) 复制的 ABI 头（01 §4 规范）
 │   ├─ api_defs.h  api_flags.h          # ← Sandboxie\core\drv\
 │   ├─ msgids.h    sbieiniwire.h
 │   │   ProcessWire.h queuewire.h
 │   │   InteractiveWire.h               # ← Sandboxie\core\svc\
 │   └─ defines_win32.h                  # ← Sandboxie\common\defines.h 需要片段 + win32_ntddk.h 的
 │                                        #   UNICODE_STRING64/MAX_PORTMSG_LENGTH 片段
 ├─ SbieCore\
 │   ├─ DriverApi\  SvcClient\  QueueClient\  Model\  Util\
 └─ sbie-cli\
     ├─ cli\  server\  ipcc\SbieIpc.h
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

不依赖：Qt、.NET、OpenSSL、7-Zip、SandboxieTools、WDK。**不触碰驱动工程**（冻结策略，
README）。

## 3. `SandboxieOSS\build_oss.bat`（规范）

与根 `build.bat` 第 0 步同款环境探测（vswhere → vcvars64），随后单独构建本 sln 并把产物
复制进 `Installer\SbiePlus_x64\`：

```bat
@echo off
setlocal EnableExtensions
REM ============================================================
REM  Sandboxie-OSS (sbie-cli) standalone build - x64
REM  Output: ..\Installer\SbiePlus_x64\sbie-cli.exe
REM ============================================================
cd /d "%~dp0"
set "REPO=%CD%\.."

REM --- 0. 与根 build.bat 相同的 vcvars64 环境引导 ---
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [ERROR] vswhere.exe not found & goto :fail )
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH ( echo [ERROR] No VS2022 C++ toolset & goto :fail )
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where msbuild >nul 2>&1 || ( echo [ERROR] msbuild not on PATH & goto :fail )
where cl >nul 2>&1 || ( echo [ERROR] cl.exe not on PATH & goto :fail )

REM --- 1. 构建（仅用户态，无需 WDK/Qt） ---
echo [1/2] Building SandboxieOSS x64 (Release)
msbuild /t:build SandboxieOSS.sln /p:Configuration=Release /p:Platform=x64 -maxcpucount:8
if errorlevel 1 goto :fail

REM --- 2. 产物并入安装布局 ---
echo [2/2] Copying output
if not exist "%REPO%\Installer\SbiePlus_x64\" (
    echo [ERROR] %REPO%\Installer\SbiePlus_x64 missing - run root build.bat first.
    goto :fail
)
copy /y x64\Release\sbie-cli.exe "%REPO%\Installer\SbiePlus_x64\sbie-cli.exe"
if errorlevel 1 goto :fail

echo BUILD SUCCEEDED: %REPO%\Installer\SbiePlus_x64\sbie-cli.exe
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
```

要点：

- 输出目录约定：vcxproj 的 `OutDir` 保持默认 `x64\Release\`，由脚本复制到
  `Installer\SbiePlus_x64\`（对齐根 build.bat 第 9 步 `copy_build.cmd` 的"合并到统一布局"
  模式；不把 OutDir 直接指到 Installer，保证独立可构建）。
- sbie-cli.exe 运行时需要同目录的 `SbieDll.dll` 与 `SbieMsg.dll`（由既有安装布局提供），
  不额外复制。
- 签名：sbie-cli.exe 是普通用户态可执行文件，走常规代码签名即可；**不涉及** 驱动签名
  （README 冻结策略的 70 美元/次成本只属于 SbieDrv.sys）。开发期允许未签名。

## 4. 并入根 build.bat 的第 11 步（仅文档记录，不改脚本）

在根 `C:\Users\Administrator\Documents\sbie\build.bat` 的第 10 步
（`call Installer\get_assets.cmd`）之后、`BUILD SUCCEEDED` 汇总之前插入：

```bat
REM ------------------------------------------------------------
REM 11. Sandboxie-OSS user-space component (sbie-cli, GPLv3)
REM     纯用户态：不依赖 WDK/Qt，vcvars64 已在第 0 步就绪
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [11/11] Building SandboxieOSS x64 (sbie-cli)
call SandboxieOSS\build_oss.bat
if errorlevel 1 goto :fail
```

并在成功性检查区（现脚本检查 `SandMan.exe` 处）追加：

```bat
if not exist "Installer\SbiePlus_x64\sbie-cli.exe" (
    echo [WARN] sbie-cli.exe missing from the output folder!
    goto :fail
)
```

注：`build_oss.bat` 自带 cd /d "%~dp0"，被 call 后用 `cd /d "%REPO%"` 恢复；与现脚本
每步开头 `cd /d "%REPO%"` 的惯例一致。步骤序号显示 `[N/10]` 需相应改为 `[N/11]`
（仅显示文本）。

## 5. IDE / 本地开发

- 打开 `SandboxieOSS\SandboxieOSS.sln` 即可独立开发（先跑一次根 build.bat 或安装版
  Sandboxie 以获得运行时依赖 `SbieDll.dll`——调试时把工作目录设为
  `Installer\SbiePlus_x64\`，或用 02 §1 的显式目录加载传参）。
- 单元测试（第二期）：`SbieCore.Tests.vcxproj`（doctest 头文件库随仓库 vendored，
  单头、MIT、允许引入——**例外需在 01-license-map 登记**；或直接手写断言宏避免例外）。
  Json/PathMapper/ValidateName/Snapshots(临时目录) 可全离线测试。

## 6. 坑记录

1. **根 build.bat 不在仓库内**：实读确认一键构建脚本位于仓库**上一级**
   `C:\Users\Administrator\Documents\sbie\build.bat`（仓库根 `Sandboxie_unbusiness\` 内只有
   `MergeDbg.cmd`/`TestCI.cmd`）。因此"`%REPO%`"按该脚本定义 = `sbie\build.bat` 所在目录，
   而第 11 步片段中的 `SandboxieOSS\` 相对路径应以**该脚本 cd 后的 %REPO%**（即
   `...\sbie`，等同仓库父目录视角 `Sandboxie_unbusiness\SandboxieOSS`）为准——若未来脚本
   移入仓库根，片段原样成立。**TODO-VERIFY**：并入时确认 %REPO% 指向与 `SandboxieOSS\`
   的相对关系（当前根 build.bat 的 %REPO% = C:\Users\Administrator\Documents\sbie，仓库在
   %REPO%\Sandboxie_unbusiness，故第 11 步调用路径应为
   `call Sandboxie_unbusiness\SandboxieOSS\build_oss.bat` —— 以实际并入位置为准修正）。
2. **`copy_build.cmd` 不会自动带上 sbie-cli.exe**：Installer 合并脚本是显式 copy 清单
   （`Installer\copy_build.cmd:132-146`），未列出的文件不进安装布局；因此 build_oss.bat
   必须自己复制（§3 已含），不能指望第 9 步。
3. vcvars64 之前把 System32 提到 PATH 最前（根 build.bat:40 的做法）是防 Git Bash 的
  GNU find/link 遮蔽；build_oss.bat 已保留同款两行，勿删。
4. `/W4 /WX` 下 vendored 的 core 头（C 风格、老式强制转换）会报噪音——vendor 头统一以
  `#pragma warning(push,3)` 包裹（在 vendor 侧包裹，不在编译选项上全局降级）。
5. **（M1 实测）build_oss.bat 必须纯 ASCII**：cmd.exe 按 OEM 代码页（中文系统 = GBK）
   解析 .bat；UTF-8 中文注释的尾部字节会与 CR/LF 配对吞掉换行，把下一行命令拼进上一行
   （实测出现 `'ink' 不是内部或外部命令` 之类碎片）。已改全英文注释。
6. **（M1 实测）vcxproj 必须开 `/utf-8`**：源码为 UTF-8 无 BOM（含中文注释），MSVC 默认
   按系统代码页（GBK）读——多字节尾字节同样吞换行，导致 `Status.h` 内 `inline bool`
   一行被上一行注释吃掉等语法错。两工程已加
   `<AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>`。新源文件保持
   UTF-8（无 BOM 亦可），勿删该开关。
7. **（实现偏离 §1 目录树，已定案）**：vcxproj 实际位于子目录内——
   `SbieCore\SbieCore.vcxproj`、`sbie-cli\sbie-cli.vcxproj`（与源码同处，M0 交付任务书
   如此要求；§1 树中画在根的写法以本条为准）。sln 仍在 `SandboxieOSS\` 根；两工程
   OutDir 统一为 `SandboxieOSS\x64\$(Configuration)\`（`$(MSBuildThisFileDirectory)..\
   x64\...`，与 §3 的 `copy x64\Release\sbie-cli.exe` 对齐）。
8. **（M1 实测）stderr 与 `_O_BINARY`**：stdout/stderr 置 binary 后 **禁止 `fwprintf`**
   （宽流在 binary 模式写裸 UTF-16，输出变成 `s\0b\0i\0e\0`）。诊断输出统一走
   `util::PrintErrLineUtf8(WideToUtf8(...))` 写 UTF-8 字节。

## 7. 验收记录（M0+M1，2026-09-27 实测）

机器：Windows 10 x64（10.0.26100），VS2022 BuildTools v143，SDK 10.0（机器最新
10.0.26100.0），本机装有 Sandboxie-Plus 5.73.5 且驱动/SbieSvc 在运行。

### 构建

```
> SandboxieOSS\build_oss.bat
[1/2] Building SandboxieOSS x64 (Release)
    （SbieCore.vcxproj + sbie-cli.vcxproj 全部编译，0 error / 0 warning，/W4 /WX）
[2/2] Copying output
BUILD SUCCEEDED: ...\Sandboxie_unbusiness\Installer\SbiePlus_x64\sbie-cli.exe
```

Debug|x64 同样通过（msbuild /p:Configuration=Debug 手测，略）。

### 实跑（Installer\SbiePlus_x64\sbie-cli.exe，同目录 SbieDll.dll 5.73.5）

```
> sbie-cli version
server not running - degraded to direct driver connection   [stderr，仅一次]
sbie-cli 0.1.0
driver 5.73.5 (abi 0x57230, alive)
svc 5.73.5 (abi 0x57230)
exit=0

> sbie-cli status
driver: 5.73.5 / abi 0x57230 / alive
features: ObCB,SbieLogin,Win32kHook,DynDataOk
service: connected / 5.73.5
server: not running
boxes: 2
exit=0

> sbie-cli box list
NAME        ENABLED  ACTIVE_PROCS  FILE_ROOT
DefaultBox  yes                 0  C:\Sandbox\Administrator\DefaultBox
New_Box     yes                 0  C:\Sandbox\Administrator\New_Box
exit=0

> sbie-cli proc list           [沙箱内启动 cmd 后]
PID    BOX         IMAGE                    SESSION  STARTED
56600  DefaultBox  SandboxieDcomLaunch.exe        1  2026-09-27 04:51:15
21988  DefaultBox  SandboxieRpcSs.exe             1  2026-09-27 04:51:15
56792  DefaultBox  cmd.exe                        1  2026-09-27 04:51:16
exit=0
```

要点核对：

- `svc 5.73.5` —— SbieSvc LPC 分块协议（`NtConnectPort` + `MSGID_SBIE_INI_GET_VERSION`）
  实跑成功，即 SbieCore/SvcClient 的完整状态机（序号/分块/重连）可用。
- `box list` 的 FILE_ROOT 已 NT→DOS 化（`\Device\HarddiskVolumeX\...` → `C:\...`）。
- `proc list` 的 STARTED 由 `PsGetProcessCreateTimeQuadPart`（100ns/1601 纪元）转本地时间。
- `--json` 各命令输出均通过 `python json.load` 机器校验（略，见 §7 实跑 JSON 样例）：
  `{"ok":true,"data":{"driver":{"alive":true,...,"abi":356912,"abi_hex":"0x57230",
  "features":[...]},...,"boxes":2}}`。
- 降级与错误路径：`--sbie-dll-path` 传空目录 / 从无 SbieDll.dll 的构建目录运行 →
  回退搜索链（同目录→Program Files→注册表 SbieSvc ImagePath→PATH）后仍成功；
  未知全局选项 exit 2；未实现命令 exit 1；server 依赖命令 exit 4。
- `status --json` 特例：本机驱动运行中，"驱动未运行优雅降级"路径未实测（不为此停机）。
  代码路径：`Gather()` 在 SbieDll 缺席时仍探测 `\Device\SandboxieDriverApi`，status
  输出 `alive:false` 并 exit 3，不崩溃。
