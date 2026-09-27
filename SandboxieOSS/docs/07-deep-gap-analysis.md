# 07 — 深度功能差距分析：SandMan 用户可见功能全集 vs sbie-cli

审计日期：2026-09-27。本文为只读审计产物：未改动任何代码，仅新增本文档。

## 0. 摘要

- **对照对象变更**：与 06 不同，本文按任务授权**实读了 SandMan（`SandboxiePlus\SandMan\`，custom
  license）全部用户可见功能面**——主菜单/工具栏/托盘/沙箱与进程右键、设置对话框 9 大页 24 子页、
  沙箱选项对话框 14 大页约 35 子页、三个向导、独立窗口（快照/恢复/弹窗/Trace/映像/选择器）、
  运维面（导出导入/磁盘映像/RAM 盘/USB/addons/视觉特性）。**报告中只记录功能规格语义**
  （命令/参数/输出/交互行为/配置键效果），全部用本文自己的语言描述，未粘贴、转译或复刻任何
  SandMan 代码；每条注明来源 `文件:行号` 以便复核。OSS 侧现状以 `SandboxieOSS\` 代码实读为准。
- **新发现计数**：**P0 新增 2 项、P1 新增 4 项**、P2 新增 5 项、N-A 新增约 12 组（06 的
  P0 12 项 / P1 7 项已全部收口，不重复计）。
- **新 P0 速览**（均为"键可写但无执行者"的语义性缺口，06 无法看见——06 对照的是 QSbieAPI
  方法面，而这两项功能**不在 QSbieAPI 里，是 SandMan 自己实现的**）：
  1. **07-P0-1**：`OnBoxDelete` 触发器——SandMan 在删除/清空沙箱内容时逐条执行该键指定的
     命令（展开 `%env%` 后异步运行）；OSS 的 `box clean` / `box delete --files` 命令路径
     **不执行任何触发器**，键可写但永远不生效。**→ 已于波次 A 收口（docs/04 §16）。**
  2. **07-P0-2**：空箱守护行为族——`AutoDelete`（空箱后自动清内容）、`AutoRemove`（清空后
     连节删除）、`OnBoxTerminate`（最后一个进程退出时执行命令）、`Temp_`/`Local_Temp_`
     一次性模板的自动清理，全部由 SandMan 常驻进程监听"箱内进程归零"事件驱动；OSS server
     无任何 box 生命周期监视，这四个键**全部静默失效**。**→ 已于波次 A 收口（docs/04 §16）。**
- **CLI 完整度结论**：对"CLI 语义宇宙"（全部用户可见功能去掉 GUI-only 与许可证禁用项后
  约 125 个可数功能项）当前覆盖率约 **83%（区间 80–85%）**；动作面约 63%、设置/选项键面
  96–100%、窗口功能面约 50%。完成本文新 P0+P1（6 项）后约 **92%**，再完成 06 P2 遗留与
  本文 P2 后约 **97%**。剩余 3% 为长期不做或依赖许可证组件的项。依据与波次建议见 §5。

---

## 1. 方法与对照清单

### 1.1 方法

1. **SandMan 侧枚举**（全部实读，只记规格）：
   - 主菜单三套（新式 / 简单式 / Vintage SbieCtrl 式）：`SandMan.cpp:824-978`（CreateMenus）、
     `SandMan.cpp:980-1109`（CreateOldMenus）、维护子菜单 `SandMan.cpp:754-791`、
     帮助菜单 `:806-822`、视图基菜单 `:793-804`。
   - 工具栏可配置项全集：`SandMan.cpp:1156-1208`（GetAvailableToolBarActions）。
   - 托盘菜单：`SandManTray.cpp:152-247`（Show/Hide、箱列表、Terminate All、Lock All、
     Pause Forcing、Disable Recovery/Messages、Exit——全部与主菜单动作同源）。
   - 沙箱/进程/组右键菜单：`Views/SbieView.cpp:132-155`（运行菜单右键：Pin to Run Menu /
     Create Shortcut）、`:174-288`（新式箱菜单）、`:347-412`（Vintage 箱菜单）、
     进程菜单 `:293-312`、预设子菜单 `:241-262`、工具子菜单 `:264-270`。
   - 设置对话框：`Forms/SettingsWindow.ui`（9 大页 24 子页，行号见 §1.2）+
     `Windows/SettingsWindow.cpp:1453-1550`（加载键面）与 `:1792-2168`（保存键面）。
   - 沙箱选项对话框：`Forms/OptionsWindow.ui`（14 大页约 35 子页）+ `Windows/Options*.cpp`
     10 个文件的键面（§1.3）。
   - 向导：`Wizards/NewBoxWizard.cpp`（字段注册 `:385-1003`；类型定义 `:434-461`/`:481-488`；
     创建落键逻辑 `:150-260`）、`Wizards/SetupWizard.cpp`、`Wizards/TemplateWizard.cpp`。
   - 独立窗口：`Windows/SnapshotsWindow.cpp`、`Windows/RecoveryWindow.cpp`、
     `Windows/PopUpWindow.cpp`、`Windows/BoxImageWindow.cpp`、`Windows/SelectBoxWindow.cpp`、
     `Views/TraceView.cpp`（Trace/资源监视）、`Windows/CompressDialog.cpp`/`ExtractDialog.cpp`、
     `Windows/EditorSettingsWindow.cpp`、`Windows/TestProxyDialog.cpp`。
   - 运维与守护逻辑：`SandMan.cpp`（OnEmptyAll/OnPauseAll/OnLockAll/OnDisableForce
     `:3538-3600`；UpdateForceUSB `:2272-2321`；DeleteBoxContent 与 OnBoxDelete `:2350-2412`；
     OnBoxClosed（OnBoxTerminate/Temp 模板清理/AutoDelete）`:2631-2672`；OnBoxCleaned
     （AutoRemove）`:2677-2689`；OnQueuedRequest `:3451-3461`）、`SandManRecovery.cpp`
     （恢复/检查器 `:109-310`）、`BoxTransfer.cpp`（导出导入）、`AddonManager.cpp`、
     `SbiePlusAPI.cpp`（箱类型判定 `:682-706`、禁网预设 `:708-729`）。
2. **OSS 侧现状**：实读 `sbie-cli\cli\Commands.cpp:559-592` 与 `cli\Commands\*.cpp` 各
   Register 块（当前命令全集见 §1.4）、`server\Dispatcher.cpp:1992-2058`（op 表）、
   `SbieCore\Model\*.cpp`（Boxes/Processes/ConfigStore/Recovery/Snapshots/Templates/
   Maintenance/BoxUsage）、`cli\Cli.cpp:21-57`（全局选项）。
3. **分级**：P0 = CLI 用户核心流程缺失（含"现有命令语义错误/静默失效"）；P1 = 常用功能缺失；
   P2 = 增强；N-A = GUI-only 或许可证/产品域且无 CLI 价值（逐项给理由）。

### 1.2 SettingsWindow 页面清单（`Forms/SettingsWindow.ui`）

| 大页(:行) | 子页(:行) | 性质 |
|---|---|---|
| General Config (:55) | General Options (:65) / Notifications (:289) | 混合（ini 键 + Plus.ini UI 键） |
| Shell Integration (:473) | Windows Shell (:483) / System Tray (:722) / Run Menu (:914) | 混合（外壳注册表 + 托盘 UI + RunCommand ini 键） |
| Interface Config (:1066) | User Interface (:1076) / Interface Options (:1264) / Window Options (:1550) | Plus.ini UI 键（GUI 域） |
| Add-Ons Manager (:1694) | Optional Add-Ons (:1704) / Add-On Configuration (:1786) | 产品域（在线组件） |
| Updates (:1914) | Update Settings (:1924) | 产品域（UpdUtil 许可证禁用） |
| Advanced Config (:1982) | Sandboxie Config (:1992) / Sandboxie.ini (:2251) | **ini 键**（根路径/WFP/保护/兼容） |
| Program Control (:2434) | Program Alerts (:2444) / Force Process Options (:2541) / USB Drive Sandboxing (:2618) | **ini 键**（Alert/通知/USB 接管） |
| App Templates (:2690) | App Compatibility (:2700) / Local Templates (:2799) | **ini 键**（Template/TemplateReject） |
| Edit ini Section (:2938) | — | 全节文本编辑（GUI 形态） |

SettingsWindow 写入 Sandboxie.ini 的键全集（`SettingsWindow.cpp:2004-2150`，加载对照
`:1480-1543`）：`DefaultBox`、`FileRootPath`、`KeyRootPath`、`IpcRootPath`、
`LockBoxToUser`、`RamDiskSizeKb`、`RamDiskLetter`、`NetworkEnableWFP`、
`EnableObjectFiltering`、`EnableWin32kHooks`、`SandboxieLogon`、`SandboxieAllGroup`、
`UseSandboxieUAC`、`BoxAliasDisplayMode`、`ImportBox`、`EditAdminOnly`、`EditPassword`
（经 LockConfig，:2047-2058）、`ForceDisableAdminOnly`、`ForgetPassword`、
`StartRunAlertDenied`、`AlertStartRunAccessDenied`、`NotifyForceProcessDisabled`、
`NotifyForceProcessEnabled`、`AlertProcess`、`AlertFolder`、`ForceMarkOfTheWeb`、
`MarkOfTheWebBox`、`ForceBoxDocs`、`ForceUsbDrives`、`UsbSandbox`、
`DisabledForceVolume`、`Template`/`TemplateReject`（兼容页 :2126-2150）、`RunCommand`
（运行菜单 :1986-1998，编码 `name|command` 或 JSON，MakeRunEntry `:2828-2850`）；
用户节键 `SbieCtrl_HideMessage`（:1971-1984）、`SbieCtrl_EnableAutoStart` /
`SbieCtrl_AutoStartAgent`（:1832-1837）；全局语言 `$:Language`（:1839）。

### 1.3 OptionsWindow 页面清单（`Forms/OptionsWindow.ui`）与键族

| 大页(:行) | 子页(:行) | 键族（来源 `Windows/Options*.cpp`） |
|---|---|---|
| General (:52) | Box (:62) / File (:392) / File Migration (:661) / Restrictions (:840) / Isolation (:1020) / Run Menu (:1111) | Enabled（含用户列表）、BoxNameTitle、BorderColor/BoxIcon、Note、AutoDelete/AutoRemove/NeverDelete、CopyLimitKb/CopyNewer/CopyAlways/CopyEmpty/CopyBlockDenyWrite/Copy*Disabled、PromptForFileMigration、NotifyNoCopy、DblClickAction、UseFileImage/ForceProtectionOnMount、SeparateUserFolders、UseVolumeSerialNumbers、UseRamDisk、UseFileDeleteV2/UseRegDeleteV2、OpenBluetooth/Clipboard/Credentials/SmartCard/PrintSpooler 等 Open* 键、MsiInstallerExemptions、RunCommand（OptionsGeneral.cpp） |
| Security (:1263) | Hardening (:1273) / Isolation (:1481) / Box Protection (:1608) / Job Object (:1801) / Advanced Security (:2041) | UseSecurityMode、UsePrivacyMode、SysCallLockDown、CallTrace/DrvTrace 族、NoSecurityIsolation、NoSecurityFiltering、NoAddProcessToJob、StripSystemPrivileges、ProtectHostImages、ConfidentialBox/LessConfidentialBox、NotifyBoxProtected、CpuRateLimit、ProcessMemoryLimit、ProcessNumberLimit、TotalMemoryLimit、DropAdminRights/FakeAdminRights、InjectDll*（OptionsGeneral/OptionsAdvanced.cpp） |
| Program Groups (:2204) | — | ProcessGroup（OptionsGrouping.cpp） |
| Program Control (:2278) | Force (:2300) / Breakout (:2441) | ForceProcess/ForceFolder/ForceChildren、BreakoutProcess/Folder/Document 族（OptionsForce.cpp） |
| Stop Behaviour (:2592) | Linger (:2602) / Leader (:2681) / Stop Options (:2760) | LingerProcess、LingerLeniency、LingerExemptWnds、LeaderProcess、ExcludeFromTerminateAll、ForceRestartAll（OptionsStop.cpp） |
| Start Restrictions (:2831) | — | StartRunAlertDenied（箱级）、AlertBeforeStart（OptionsStart.cpp） |
| Resource Access (:2943) | Files (:2953) / Registry (:3060) / IPC (:3167) / Wnd (:3274) / COM (:3387) / Policies (:3500) | Open/Closed/Read/Write FilePath|KeyPath|IpcPath|PipePath、OpenWinClass/NoRenameWinClass、OpenClsid/ClosedClsid、BoxedCOM、UseRuleSpecificity、UsePrivacyMode、ClosePrintSpooler 等（OptionsAccess.cpp） |
| Network (:3612) | Restrictions (:3622) / Firewall (:3727) / DNS Filter (:3892) / Proxy (:3971) / Other (:4133) | AllowNetworkAccess、InternetAccessDevices、NotifyInternetAccessDenied、PromptForInternetAccess、NetworkFirewall 族（Allow/Block/BlockPorts/BlockDNS/ICMP/BindAdapter*）、NetworkDnsFilter、NetworkUseProxy 族（地址/端口/认证/加密口令）、BlockNetParam、UseProxyThreads（OptionsNetwork.cpp） |
| File Recovery (:4284) | Quick (:4294) / Immediate (:4373) | RecoverFolder、AutoRecover、AutoRecoverIgnore、UseAutoRecoverIgnoreForQuick（OptionsRecovery.cpp） |
| Various (:4483) | Compatibility (:4499) / Users (:5371) / Tracing (:5439) / Debug (:5652) / Debug Options (:5677) / Config Dump (:5753) | Template（含 Local_）、Enabled=y,用户1,用户2…（OptionsAdvanced.cpp:380-386/688-695）、FileTrace/IpcTrace/PipeTrace/GuiTrace/KeyTrace/ClsidTrace/DnsTrace/NetFwTrace 族、ApiTrace、DebugOptions、HideDiskSerialNumber 等隐藏族、AutoExec、StartProgram/StartService、OnBoxDelete/OnBoxTerminate/OnFileRecovery（触发器，OptionsAdvanced.cpp:321/636/900） |
| App Templates (:5849) | Templates (:5859) / Template Folders (:6021) / Accessibility (:6083) | Template=、Tmpl.Folder 覆盖、UseElectronDetection 等（OptionsTemplates.cpp） |
| Edit ini Section (:6196) | — | 全节文本编辑（GUI 形态） |

### 1.4 OSS sbie-cli 当前命令面（实读 `cli\Commands.cpp:559-592` + 各 Register 块）

`status`、`version`；`server start|stop|status`；`box list|create|info|get|set|list-setting|
rename|delete|enable|disable|clean|size|snapshot(list/take/remove/select/set-info)|recover
(list/copy/add)`；`proc list|info|start|kill|kill-all|suspend|resume`；`cfg get|set|unset|
list-setting|reload|path|lock|unlock`；`template list|info|apply|revoke|check`；`log watch|
dump|messages`；`force on[<sec>]|off|status`；`maint status|start|stop`。全局选项
`--json --quiet --no-server --password --no-refresh --show-transport --sbie-dll-path`
（`Cli.cpp:21-57`）。server op 表见 `Dispatcher.cpp:1992-2058`（tpl.list/info/check 与
log.event 仍为 Stub，client 直连实现，功能不受损）。

---

## 2. 键面覆盖验证（"配置键即功能"类）

**结论先行：SettingsWindow 与 OptionsWindow 写入 Sandboxie.ini 的全部键（§1.2 约 36 键 +
§1.3 约 45 键族）均可由 OSS 通用路径操纵**——`cfg set --section <s>` 接受任意节名
（≤64 字符，`SbieCore\Model\ConfigStore.cpp:35-64` 经 SbieSvc `SBIE_INI_SETTING_REQ`
透传，含 `Template_*`（Templates.ini 节）、`$`（用户节）与 GlobalSettings）；box 节经
`box set` 四写模式（update/append/insert/index）。这与 06 §4-9 的结论一致并扩展到了
本报告新枚举的全部键。**例外（键可写但效果需要代码逻辑，即本报告的缺口）**：

| 键 | 效果执行者（SandMan 侧） | OSS 现状 | 归档 |
|---|---|---|---|
| `OnBoxDelete` | SandMan 删除/清空内容时逐条执行命令（`SandMan.cpp:2402-2412`） | `box clean/delete` 不执行（`SbieCore\Model\Boxes.cpp:201-234` 无触发器逻辑） | **07-P0-1** |
| `OnBoxTerminate` | SandMan 监听箱内进程归零后执行（`SandMan.cpp:2631-2637`） | 无监视者 | **07-P0-2** |
| `AutoDelete` / `AutoRemove` | 空箱后自动清内容 / 清空后连节删（`SandMan.cpp:2659-2672`、`:2677-2689`） | 无监视者 | **07-P0-2** |
| `Temp_`/`Local_Temp_` 模板 | 空箱后从 Template 列表移除并清节（`SandMan.cpp:2640-2657`） | 无监视者 | **07-P0-2** |
| `OnFileRecovery` | 恢复文件前执行外部检查器命令（`SandManRecovery.cpp:109-131`，含 FileChecker addon） | `box recover copy` 不执行检查器 | **07-P1-3** |
| `ForceUsbDrives`/`UsbSandbox`/`DisabledForceVolume` | 守护枚举 USB 卷→写 `ForceFolder` 列表（`SandMan.cpp:2272-2321`，含不存在时自动建箱并设 UseFileDeleteV2 等初始键） | 键可写但无人生成 ForceFolder | **07-P2-1** |
| `RamDiskSizeKb`/`RamDiskLetter`/`UseRamDisk` | 挂载由 SandboxieTools 的 ImDisk 驱动承担；SbieSvc 侧编排接口在核心（`Sandboxie\core\svc\MountManager.h:82-91`） | 键可写；运行时组件许可证禁用 | N-A-7 |
| `UseFileImage`/`ConfidentialBox` | 磁盘映像创建/挂载（ImBox，SandboxieTools） | 许可证禁用 | 06 N-A-1 维持 |

其余键的效果执行者核实结果：`BorderColor`/`BoxNameTitle`/`CoverBoxedWindows` 等视觉键由
SbieDll（核心，冻结期内不变）在沙箱进程内实现——键面即功能；`StartProgram`/`StartService`/
`AutoExec` 由 SbieDll 在沙箱启动时执行（`Sandboxie\core\dll\custom.c:106-109` 等）——键面即
功能；网络防火墙/DNS 过滤键由驱动 WFP 层执行——键面即功能；`ProcessGroup` 访问规则组是纯
ini 数据结构——键面即功能。

---

## 3. 差距主表

> 编号规则：`07-P0-n / 07-P1-n / 07-P2-n / 07-N-A-n`，与 06 编号空间隔离。"新发现"指
> 06 表中不存在对应项；06 已列项在此只登记合并视图（§4）。

### 3.1 P0 — CLI 用户核心流程缺失（新发现 2 项；**两项均已于波次 A 收口，见 docs/04 §16**）

| # | 功能【SandMan 规格 / 来源】 | OSS 现状 | CLI 建议 | 规模 |
|---|---|---|---|---|
| 07-P0-1 | **OnBoxDelete 触发器**（✅ 已收口：`box clean`/`box delete --files` 删除内容前逐条执行，`--no-triggers` 逃生旗标，`%SANDBOX%` 变量支持——实现与语义决策见 docs/04 §16）：删除沙箱内容（右键 Delete Content / 删除沙箱）前，逐条读取该箱 `OnBoxDelete` 键的命令值，展开箱变量后作为宿主命令异步执行，UI 呈现进度并可取消（`SandMan.cpp:2402-2412`；选项页键面 `Windows/OptionsAdvanced.cpp` OnBoxDelete/OnBoxDeleteDisabled）。典型用途：清箱时清缓存脚本、日志归档。 | ~~`box clean` / `box delete --files` 全链路无触发器概念~~ 已收口（server op no_triggers 参数 + client 旗标 + 直连路径，Model `RunBoxTriggers` 共享执行器）。 | 已按建议实现（命令路径内同步执行 + 逃生旗标 + %SANDBOX% 展开）。 | 已完成 |
| 07-P0-2 | **空箱守护行为族**（✅ 已收口：server 常驻监视器 1s 轮询，OnBoxTerminate/AutoDelete/AutoRemove/Temp_ 前缀四行为全实现，`--no-guardians`/`GuardiansEnabled` 开关，`server status` GUARDIANS 列——实现与语义决策见 docs/04 §16）（常驻管理器职责，全部由"箱内进程归零"事件驱动）：① `OnBoxTerminate`：最后一个进程退出时执行命令（`SandMan.cpp:2631-2637`）；② `Temp_`/`Local_Temp_` 一次性模板：箱空后从 `Template` 列表移除并清模板节（`:2640-2657`）；③ `AutoDelete=y`：箱空后自动清空内容（先给一次恢复机会）（`:2659-2672`）；④ `AutoRemove=y`：清空后连 ini 节删除（OnBoxCleaned，`:2677-2689`）。 | ~~OSS server 无任何 box 生命周期监视~~ 已收口（`server\Guardian.cpp`；②按任务书规格实现为"箱名前缀等同 AutoRemove"，模板节清理形态为遗留，见 04 §16 遗留 5）。 | 已实现（轮询选型与"恢复机会"的 CLI 化文档化均记录于 04 §16）。 | 已完成 |

### 3.2 P1 — 常用功能缺失（新发现 4 项；**四项均已于波次 B 收口，见 docs/04 §17**）

| # | 功能【SandMan 规格 / 来源】 | OSS 现状 | CLI 建议 | 规模 |
|---|---|---|---|---|
| 07-P1-1 | **建箱类型预设**（✅ 已收口：`box create --type` 六类预设 + `box types` 辅助命令——实现与语义决策见 docs/04 §17）（NewBoxWizard）：建箱时先选 7 类预设之一——Security Hardened(+Data Protection)、Standard(+Data Protection)、Application Compartment(+Data Protection)、Confidential Encrypted（`Wizards/NewBoxWizard.cpp:434-461,481-488`），每类映射固定键组（Hardened→`UseSecurityMode=y`；+Data Protection→叠加 `UsePrivacyMode=y`；Compartment→`NoSecurityIsolation=y`+`Template=RpcPortBindingsExt`；加密类→UseFileImage 链路，许可证禁；`:150-260` 落键）。高级页另含：位置/箱版本(V2 删除)/独立用户目录/卷序列号/自动删除(临时箱=AutoDelete+AutoRemove)/自动恢复/禁网(设备式或 WFP 式)/共享访问/提示访问/降权/伪管理员/MSI 服务/箱令牌/映像保护/覆盖窗口/共享模板三模式（`:627-1051` 字段注册）。 | ~~`box create <name> [--template]` 仅写 Enabled=y，无类型概念~~ 已收口（六类=7 类去掉许可证禁的 Confidential Encrypted；键组映射按本行规格；高级页旗标未做，见 04 §17 遗留）。 | 已按建议实现（`--type hardening\|hardened-plus\|standard\|standard-plus\|app\|app-plus`）。 | 已完成 |
| 07-P1-2 | **沙箱导出/导入**（✅ 已收口：`box export --to <dir|.sbx/.zip>` / `box import <path> --name`，zip 自研 store + 目录形态双支持——实现与 zip/dir 决策见 docs/04 §17）（迁移与备份核心）：多选箱导出为 7z/zip 归档，可选每箱一文件、导出全局配置节、归档口令加密；导入时冲突处理（改名/跳过/覆盖）、别名探测（`BoxTransfer.cpp` 全文 1296 行 + Compress/Extract 对话框；入口 `SandMan.cpp:831-834`、`SbieView.cpp:269`）。含内容导出时对加密箱要求先挂载。 | ~~无任何等价~~ 已收口（ini 节 box.ini + FileRoot 树 content/ 双形态包；加密/口令归档列为遗留）。 | 已按建议实现（zip 走自研最小读写器，见 04 §17 决策）。 | 已完成 |
| 07-P1-3 | **恢复的移动语义与安全校验**（✅ 已收口：`box recover copy --move` + OnFileRecovery 检查器缺省执行、`--no-check` 逃生——实现与语义决策见 docs/04 §17）：① SandMan 恢复=**移动**（恢复成功即从沙箱内删除原文件），失败文件逐个列出（`SandManRecovery.cpp:266-283`，rename 语义）；② 恢复前对每个文件执行检查器命令（箱键 `OnFileRecovery` 列表 + FileChecker addon 脚本，超时 15 秒/命令，失败可逐文件/全部决定是否仍恢复，`SandManRecovery.cpp:109-131,196-228`）；③ 恢复完成后可选打开文件/在资源管理器定位（`:274-283`）。 | ~~`box recover copy` 为纯拷贝语义，不执行任何检查器~~ 已收口（move=拷后删源；检查器拒绝=跳过并列出，`--no-check` 放行；③为 GUI 动作，N-A）。 | 已按建议实现（server op recover.copy 增 move/on_file_recovery 参数）。 | 已完成 |
| 07-P1-4 | **复制沙箱**（✅ 已收口：`box copy <src> <dst> [--content]`——实现与语义决策见 docs/04 §17）（Duplicate Box Config（整节复制到新箱名，含组归属与别名恢复）与 Duplicate Box with Content（叠加目录树复制，异步进度）（`SbieView.cpp:1633-1693`））。 | ~~无整节读出/写入命令~~ 已收口（整节读出→节替换写入+可选目录树拷贝；不触发 OnBoxDelete、源箱只读）。 | 已按建议实现（`box copy`，06 P2-9 节 dump 的整节读出子能力同源落地）。 | 已完成 |

### 3.3 P2 — 增强（新发现 5 项）

| # | 功能【SandMan 规格 / 来源】 | OSS 现状 | CLI 建议 | 规模 |
|---|---|---|---|---|
| 07-P2-1 | **USB 沙箱自动接管**：全局 `ForceUsbDrives=y` 时，枚举本机 USBSTOR 卷（卷序列号十六进制 `HHHH-LLLL` 为键），未被 `DisabledForceVolume` 排除的卷的挂载点全部写入 `UsbSandbox` 箱的 `ForceFolder`；目标箱不存在时自动创建并设 V2 删除+卷序列号键（`SandMan.cpp:2272-2321`；设置页卷清单 `SettingsWindow.cpp:1686-1744`）。 | 键可写无生成者（§2）。 | `sbie usb sync`（一次性：枚举→建箱（若缺）→写 ForceFolder，输出将接管的卷表）；长期可挂 server 周期任务。卷枚举需 SetupAPI（新 Util 模块）。 | 中（卷枚举） |
| 07-P2-2 | **箱类型派生判定**：由键组合算出 7 类显示类型与图标（Hardened=UseSecurityMode、+Plus=叠加 UsePrivacyMode、Compartment=NoSecurityIsolation、Insecure=UnsecureDebugging、Private=UseFileImage+Confidential、Open=根开放）（`SbiePlusAPI.cpp:682-706`）。 | `box info` 仅 6 字段（name/enabled/三根路径/has_processes/has_snapshots，`box_manage.cpp:96-117`）。 | `box info` 加 `type` 派生字段（纯键面判定，无 IO）+ `NeverDelete`/`box size` 顺带；`box list --type` 过滤。 | 微 |
| 07-P2-3 | **Start.exe 伪程序入口的验收与文档**：运行菜单的"运行程序/开始菜单/宿主开始菜单/autorun/默认浏览器/邮件/资源管理器/注册表/程序和功能/cmd(管理员/32 位)"全部通过把伪命令串交给 Start.exe 实现（`SbieView.cpp:1482-1521`：`run_dialog`/`default_browser`/`mail_agent`/`auto_run`/`explorer.exe …`/`regedit.exe`/`control.exe appwiz.cpl`/`cmd.exe` 等）。 | `proc start <box> <cmd...>` 命令串透传 Start.exe（`proc_cmd.cpp` 直连与 IPC 同为命令行转发），**理论上原样可用**但从未验收、未文档化。 | 补验收用例（6 个伪命令逐一跑通）+ `proc start --help` 文档小节。 | 微（测试+文档） |
| 07-P2-4 | **故障排除向导**（Troubleshooting Wizard）：JS 脚本驱动的问题诊断树，随已知应用库（`Troubleshooting/` 目录：AppCompatibility.js、KnownApps/、Sandboxing/、UI/、多语言 _lang.json）分发（`SandMan.cpp:816`、`Wizards/BoxAssistant.cpp`）。 | 无。脚本资产在仓库内（GPL 侧），无解释器。 | 长期可选：`sbie doctor <app>` 执行诊断脚本需要 JS 引擎——与"无第三方依赖"冲突，**建议不做或改为静态检查清单**（读 KnownApps 建议键组）。列为观察项。 | 大（若做）/ 0（不做） |
| 07-P2-5 | **浏览器兼容模板生成器**（TemplateWizard）：选浏览器 exe→自动识别引擎（Gecko/Chromium）与 profile 目录→勾选 cookies/密码/书签/偏好等直接访问项→生成 `Local_` 模板节并可三模式挂到箱（`Wizards/TemplateWizard.cpp` 全文）。 | 模板节可由 `cfg set --section Template_Local_*` 手写（键面通），但浏览器探测与 profile 枚举逻辑无等价。 | `sbie template wizard` 形态不现实；可做 `template scan-browser <exe>` 输出建议的 Local_ 节文本（探测逻辑为注册表/文件读，可自研）。 | 中（探测） |

### 3.4 N-A — GUI-only / 许可证 / 产品域（新登记 12 组，06 已有 16 项不重复）

| # | 功能【来源】 | 不适用理由 |
|---|---|---|
| 07-N-A-1 | 视觉特性呈现：箱边框着色/厚度/最大化内框（BorderColor 族键）、窗口标题注入（BoxNameTitle）、箱图标着色/覆盖图标（BoxIcon/ColorBoxIcons）、覆盖箱窗口（CoverBoxedWindows）、PinToTray（`OptionsGeneral.cpp` 键面；`SandMan.cpp:877-925` 视图项） | **键本身可写（§2 已验证），绘制者是 SbieDll/驱动——键面即功能**；GUI 观感项无 CLI 呈现面。 |
| 07-N-A-2 | 托盘/全局热键/置顶/窗口布局/多显示器定位/视图模式切换（`SandManTray.cpp` 全文；`SandMan.cpp:795-925`；热键 Panic/TopMost/PauseForce/Suspend `SettingsWindow.cpp:1894-1904`） | GUI 域。热键动作的语义等价已覆盖：Panic→`proc kill-all --all`、PauseForce→`force on`、Suspend→挂起命令组。 |
| 07-N-A-3 | 沙箱分组/排序/隐藏箱（`SbieView.cpp:3086-3108` BoxGrouping 存为用户节 TextMap；`:146-147` 自定义排序） | 纯 GUI 组织。键可经用户节写（无呈现意义）；`--json` 输出平铺列表即 CLI 形态。 |
| 07-N-A-4 | 即时恢复弹窗与消息弹窗的窗口交互（置顶/到前台/多显示器）（`PopUpWindow.cpp` 全文；`SandManRecovery.cpp:30-50`） | GUI 交互域；事件呈现等价已有（`log watch` 2199 等），决策等价为 06 P2-10。 |
| 07-N-A-5 | 恢复历史日志窗口/消息日志窗口/保留已终止进程树（`SandMan.cpp:3064-3110,930`） | GUI 日志面板域。 |
| 07-N-A-6 | 异步操作进度与取消（Stop Operations 右键、进度对话框）（`SbieView.cpp:183`；`SandMan.cpp:4254-4289`） | GUI 进度域；CLI 一次性命令无长任务面板。 |
| 07-N-A-7 | RAM 磁盘（RamDiskSizeKb/RamDiskLetter/UseRamDisk 键；挂载编排接口在核心 `Sandboxie\core\svc\MountManager.h:82-91`） | 键可写；**运行时挂载驱动（ImDisk）属 SandboxieTools，许可证禁用**（01 §2）。与 06 N-A-1 同源。 |
| 07-N-A-8 | 磁盘映像加密箱全家：BoxImageWindow（口令+≥256MB 映像创建）、Mount/Unmount Box Image、Lock All Encrypted Boxes、NewBoxWizard 的 Confidential 类型（`Windows/BoxImageWindow.cpp`；`SandMan.cpp:839,3563-3583`；`SbieView.cpp:222-223`） | ImBox 属 SandboxieTools，许可证禁用（06 N-A-1 维持，此处补全 GUI 面细节）。 |
| 07-N-A-9 | addons 在线管理与更新检查（AddonManager 安装/移除可选组件、更新源）（`AddonManager.cpp:46-101`；设置页 `SettingsWindow.cpp:2215-2275`） | 产品域（在线下载 + UpdUtil 许可证）；FileChecker 的**数据面**已并入 07-P1-3（检查器执行器），脚本用户可自制。 |
| 07-N-A-10 | Setup 首跑向导（UI 模式选择/WFP 开关/外壳集成/开机自启/更新偏好）（`Wizards/SetupWizard.cpp` 全文） | GUI 向导；其每一项落点均已有 CLI 组合：NetworkEnableWFP 键 / 外壳注册表（06 P2-8）/ SbieCtrl_EnableAutoStart 用户节键 / 更新项 N-A。 |
| 07-N-A-11 | 多语言 UI（24 个 .ts）与无障碍（字体缩放/高对比/替行色）（`SandMan\sandman_*.ts`；`SettingsWindow.cpp:1798-1823`） | `Language` 键可写（`$` 节）；CLI 输出面向脚本（--json），自身不做 i18n；无障碍为 GUI 呈现域。 |
| 07-N-A-12 | NT 命名空间浏览器/Trace 栈符号化/外部编辑器选择/代理测试对话框/SelectBox 运行选择器/恢复窗口"关闭至程序停止"（`Views/NtObjectView.cpp`；`Views/TraceView.cpp` 栈列；`Windows/EditorSettingsWindow.cpp`、`TestProxyDialog.cpp`、`SelectBoxWindow.cpp`） | GUI 调试/辅助窗口域。Trace 数据面缺口的 CLI 形态归 06 P2-1。 |

---

## 4. 与 06-gap-analysis.md 的关系与合并视图

### 4.1 分工

- **06 覆盖 QSbieAPI 方法面**（API 签名级对照），据此收口了 P0×12 / P1×7（已全部完成，
  见 06 §3 完成登记），剩余 P2×14。
- **07（本文）覆盖用户可见功能面**（菜单/页面/向导/窗口/守护行为），其中大量功能**不在
  QSbieAPI 里**——它们是 SandMan 在 GUI 层实现的行为（触发器执行、空箱守护、导出导入、
  类型预设、USB 接管、恢复移动/检查），这类缺口 06 方法论天然看不见，是本文的新发现主体。

### 4.2 合并视图（全部缺口项的现状一览）

| 缺口 | 06 编号 | 07 编号 | 状态 |
|---|---|---|---|
| 终止/挂起/启用/清理/容量/配置锁/恢复基础/参数接线 | P0-1..12 | — | ✅ 已收口（06 §3.1 登记） |
| 禁用强制/维护启停/全局终止/参数语义 | P1-1..7 | — | ✅ 已收口（06 §3.2 登记） |
| trace/监控命令组 | P2-1 | （07-N-A-12 仅窗口形态） | 待做 |
| 模板应用检测（scan） | P2-2 | — | 待做（07-P2-5 为其浏览器特化建议） |
| 快照默认标记 | P2-3 | — | 待做 |
| 空置/初始化状态 | P2-4 | 07-P2-2（扩展为类型+状态派生组） | 待做 |
| proc info 令牌/图像类型列 | P2-5 | — | 待做 |
| 箱级整体挂起/恢复 | P2-6 | 07 补充：全局形（Suspend All=逐箱 SetSuspendedAll，`SandMan.cpp:3554-3561`）亦缺，`--all` 旗标同做 | 待做 |
| 组件装卸 install/uninstall | P2-7 | — | 待做（07-N-A 中 SetupWizard 的外壳项与其相邻） |
| 外壳集成/快捷方式 | P2-8 | — | 待做 |
| 节全量导出（box/cfg dump） | P2-9 | 07-P1-4 依赖并扩展（节**写入**/替换也要暴露） | 待做（升格为 P1 依赖） |
| interactive 人工决策 | P2-10 | 07-N-A-4 弹窗 4 类语义细化（`PopUpWindow.cpp:315` 打印假脱机、`:496-503` 大文件迁移/上网询问、`:558-565` 即时恢复、`:665` 迁移进度） | 待做 |
| 驱动缓存旁路写/whoami/explore/进程豁免 | P2-11..14 | — | 待做 |
| **OnBoxDelete 触发执行** | — | **07-P0-1** | ✅ 已收口（波次 A，docs/04 §16） |
| **空箱守护（OnBoxTerminate/AutoDelete/AutoRemove/Temp 模板清理）** | — | **07-P0-2** | ✅ 已收口（波次 A，docs/04 §16） |
| **建箱类型预设** | — | **07-P1-1** | ✅ 已收口（波次 B，docs/04 §17） |
| **导出/导入归档** | — | **07-P1-2** | ✅ 已收口（波次 B，docs/04 §17） |
| **恢复移动语义+检查器** | — | **07-P1-3** | ✅ 已收口（波次 B，docs/04 §17） |
| **复制沙箱** | — | **07-P1-4** | ✅ 已收口（波次 B，docs/04 §17） |
| USB 接管/类型派生/伪命令验收/doctor/浏览器模板 | — | 07-P2-1..5 | 新发现 |
| ImBox/RAM 盘/更新/addons/托盘热键布局等 | N-A-1/2/5 等 | 07-N-A-1..12（细化补全） | 维持 N-A |

### 4.3 已确认覆盖（非缺口，抽样列示）

运行沙箱化（`proc start`，含伪命令透传待验收）、终止箱内全部（`proc kill-all`）、全局终止
（`--all`）、暂停强制（`force`，时长默认 `ForceDisableSeconds` 键）、维护启停（`maint`）、
快照全家（take/remove/select/set-info；"回空箱"=remove 当前+组合）、恢复基础（list/copy/
add/--to/--overwrite）、模板应用/撤销/列生效、全局与箱级键面（§2 全表）、重命名/删除/启用/
停用/清空/容量、三个 ini 的编辑入口（`cfg path`+外部编辑器；Templates.ini 同目录）、
便携布局（`cfg path` 的 is_home/location 字段）。

---

## 5. 结论

### 5.1 CLI 完整度评估

按 §1 对照清单可数化（去重后）：

| 域 | 可数项 | 已覆盖（含键面/组合） | 覆盖率 | 主要缺口 |
|---|---|---|---|---|
| A 菜单/工具栏/托盘/右键动作 | 32（扣除 N-A 15 项） | 20 | **63%** | 预设建箱、导出导入、复制、（06 已列）trace/装卸/外壳/挂起全局形 |
| B SettingsWindow ini 键 | 36 | 36 | **100%** | 键面全覆盖（3 键效果受限见 §2） |
| C OptionsWindow 键族 | 45 | 43 | **96%** | 仅 OnBoxDelete/OnFileRecovery 执行语义、映像链路 |
| D 独立窗口功能 | 8（扣除 N-A） | 4 | **50%** | trace 窗口数据面（06 P2-1）、交互决策（06 P2-10）、压缩/导出组件、恢复 move/check |
| E 向导 | 2（扣除 Setup N-A） | 0 | **0%** | 类型预设、浏览器模板生成 |
| F 运维（映像/RAM/USB/addons/便携/多语言） | 2（扣除 N-A 6 项） | 1 | **50%** | USB 接管；便携布局已覆盖 |
| **合计（CLI 语义宇宙）** | **125** | **104** | **≈83%** | （B/C 权重高是合理的：键面是 CLI 的天然形态） |

- **当前 ≈83%（区间 80–85%）**：读路径、写键面、生命周期显式命令（创建/删除/快照/恢复基础/
  模板/日志/force/maint）均已稳定；缺口集中在 **SandMan 守护行为（07-P0-2）与操作工程化
  （导出导入/预设/复制）** 两簇——前者是"server 作为常驻管理器"的定位缺口，后者是纯增量功能。
- 完成 07-P0×2 + 07-P1×4（+P1-4 依赖的 06 P2-9）→ **≈92%**；再完成 06 P2-1/2/6/7/8/10 与
  07-P2-1/2/3 → **≈97%**。剩余为 07-P2-4/5 与深水区（trace 栈符号化等），可长期搁置。

### 5.2 建议波次划分

| 波次 | 内容 | 规模合计 | 理由 |
|---|---|---|---|
| 波次 7：触发器与生命周期 | 07-P0-1（命令路径触发器）→ 07-P0-2（server 箱空监视+四行为）；顺带 `server status` 守护呈现 | 中 | **✅ 已完成（docs/04 §16 验收）**。修复"键可写但静默失效"的语义陷阱，最高优先 |
| 波次 8：创建与复制体验 | 07-P1-1（--type 预设+常用旗标）、07-P1-4（duplicate）、06 P2-9 扩展（节 dump/节写入暴露）、07-P2-2（info 派生字段）、06 P2-3/P2-4 顺带 | 中 | **核心已完成的子集 ✅（07-P1-1/4 + 节整读/整写子能力，docs/04 §17）；07-P2-2 与 06 P2-3/4 及高级旗标仍待做** |
| 波次 9：迁移与恢复增强 | 07-P1-2（export/import，先 --dir+zip store 形态）、07-P1-3（--move + --check） | 中-大 | **✅ 已完成（docs/04 §17 验收；zip 自研 store 读写器 + --move + 检查器缺省执行）** |
| 波次 10：诊断与自动化 | 06 P2-1（trace）、06 P2-2（模板 scan）、07-P2-1（usb sync）、06 P2-6 全局形、06 P2-10（交互决策）、07-P2-3（伪命令验收） | 中 | 观测与自动化层；顺序可按用户反馈调 |
| 搁置 | 07-P2-4（doctor，与无依赖约束冲突）、07-P2-5（浏览器模板探测）、06 P2-5/11/12/13/14、全部 N-A | — | 价值/成本比低或依赖外部决策 |

### 5.3 复核指引（本文全部关键判定的证据锚点）

- SandMan 菜单/工具栏/托盘/右键全集：`SandMan.cpp:754-1109,1156-1208`、
  `SandManTray.cpp:152-247`、`Views/SbieView.cpp:132-412,1482-1545,1633-1693`。
- 设置键面加载/保存：`Windows/SettingsWindow.cpp:1453-1550,1746-1790,1929-2168`。
- 选项键面：`Windows/Options*.cpp`（§1.3 表内逐文件）。
- 触发器/守护行为：`SandMan.cpp:2350-2412,2631-2689`、`SandManRecovery.cpp:109-131,196-310`。
- 类型预设与向导：`Wizards/NewBoxWizard.cpp:150-260,385-1051`。
- 导出导入/映像/USB/addons：`BoxTransfer.cpp`、`Windows/BoxImageWindow.cpp`、
  `SandMan.cpp:2272-2321`、`AddonManager.cpp:46-101`。
- OSS 现状：`sbie-cli\cli\Commands.cpp:559-592`、`cli\Commands\box_manage.cpp:96-117`、
  `cli\Commands\box_create.cpp`、`SbieCore\Model\Boxes.cpp:201-251`、
  `SbieCore\Model\ConfigStore.cpp:35-64`、`server\Dispatcher.cpp:1992-2058`。
