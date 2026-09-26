# 06 — 功能缺口分析：sbie-cli 对照 Sandboxie Plus 功能面

审计日期：2026-09-27。本文为只读审计产物：全部结论来自实读代码，未改动任何代码。
许可证边界（01-license-map.md）：QSbieAPI（LGPL）只读参考；SbieCtrl `Sandboxie\apps\control\`（GPLv3）
可读可复制；**SandMan 整目录未读**，其功能面以 `docs\04-modules.md` 已固化的映射为准；
`SandboxieTools\`（无许可证）禁用。任务描述中的 "RecoverFiles/GetRecoveredFiles/BoxSize/UpdateBoxSize"
经核实**并非 QSbieAPI 方法**（该头文件无此符号），系 SandMan 扩展层方法——本文按许可证边界以
QSbieAPI 等价面（信号/路径映射/纯文件操作）+ SbieCtrl 行为（GPL）作为该功能的对照基准。

## 1. 审计方法与对照清单

### 1.1 方法

1. 逐方法通读 `SandboxiePlus\QSbieAPI\SbieAPI.h`（公共方法全集，:33-332）、`Sandboxie\SandBox.h`、
   `BoxedProcess.h`、`SbieIni.h`、`SbieTemplates.h`、`SbieUtils.h`，形成 Plus 用户态能力权威清单；
   对语义存疑项（CleanBox/DisableForceProcess/LockConfig/GetRealPath）下探 .cpp 实读（LGPL 允许）。
2. 通读 SbieCtrl（GPLv3）菜单面：`SbieControl.rc` 的 TOP_MENU/TRAY_MENU/SANDBOX_MENU/FILE_MENU、
   `resource.h` ID 表、`QuickRecover.cpp`/`BoxFile.cpp` 的恢复行为。
3. 实读 SandboxieOSS 全部实现：`sbie-cli\cli\Commands.cpp` + `cli\Commands\*.cpp`（命令注册与参数接线）、
   `sbie-cli\server\Dispatcher.cpp`（op 表真 handler vs Stub）、`SbieCore\Model\*.cpp`（领域层实现 vs 桩）、
   `SbieCore\SvcClient\SvcClient.h`、`SbieCore\DriverApi\DriverApi.h`（已绑定导出面）。
4. SandMan 功能行为以 `docs\04-modules.md` 的映射与坑记录为唯一来源（custom license，未开其源码）。

### 1.2 状态分类

| 标记 | 含义 |
|---|---|
| 已实现 | client 命令 + IPC op（或约定的降级直连）+ Model/Svc 层全链路可用（验收见 04 §9-§12） |
| Stub降级 | server op 为 Stub（ERR_NOT_IMPLEMENTED），client 有真实现，自动降级直连，功能不受损 |
| 桩(缺失) | client 命令与/或 server op 与/或 Model 层为占位，功能不可用 |
| 正确性缺陷 | 功能存在，但默认（server 在场）路径下行为错误 |

判定以**实读行号**为准；关键证据：client 桩注册 `Commands.cpp:561-576`；server Stub 注册
`Dispatcher.cpp:1188-1223`；Model 桩 `BoxUsage.cpp:12-17`。

### 1.3 对照清单（Plus 侧能力组）

- **CSbieAPI**（连接/版本/路径/box/进程/配置/密码/强制运行/ImBox/监控/杂项）
- **CSandBox**（box 生命周期/终止/清理/快照/挂载）
- **CBoxedProcess**（进程信息/终止/挂起/令牌位）
- **CSbieIni**（通用配置读写四模式/列表/映射/节操作）
- **CSbieTemplates**（模板枚举/应用/应用检测）
- **CSbieUtils**（组件启停/装卸/外壳集成/快捷方式）
- **SbieCtrl 菜单**（File/Sandbox/View/Configure/Tray/进程右键/文件右键 + Delete/QuickRecover 选项页）

## 2. 已实现面对照快览（非缺口，供覆盖率判断）

| Plus 能力（QSbieAPI/SbieCtrl） | CLI 等价 | 状态证据 |
|---|---|---|
| Connect/TakeOver（会话 leader） | server 自动拉起 + leader 接管 | 04 §11-12；ServerState |
| GetVersion / GetDriverInfo / GetFeatureFlags | `status` / `version` | Commands.cpp:243-369 |
| ReloadBoxes / GetAllBoxes / IsBox / CreateBox / ValidateName | `box list/info/create` | Boxes.cpp:85-154,253-284 |
| QueryBoxPath（三根路径） | `box info` | Boxes.cpp:114-138 |
| RenameBox / RemoveBox（NeverDelete 保护 + BOX_BUSY） | `box rename/delete [--files/--keep-section]` | Boxes.cpp:156-234 |
| UpdateProcesses / GetAllProcesses / GetProcessById / QueryProcessInfo | `proc list/info` | Processes.cpp:34-64 |
| RunStart / RunSandboxed（含 elevated 降级） | `proc start [--elevated] [--wait]` | Processes.cpp:112-188 |
| Terminate(pid) | `proc kill` | Processes.cpp:67-70 |
| SbieIniGet/Get2/GetEx / GetTextList | `cfg get` / `box get [--index]` | ConfigStore.cpp:85-107 |
| ListSettings（枚举 setting 名，含 --all 模板项） | `cfg list-setting`(server) / `box list-setting` | ConfigStore.cpp:109-133 |
| SbieIniSet 四模式（Update/Append/Insert/Delete） | `cfg set` / `box set [--append/--insert/--index]` | ConfigStore.cpp:135-193 |
| ReloadConfig | `cfg reload [--reconfigure]` | ConfigStore.cpp:195-198 |
| GetIniPath / IsPortable(is_home) | `cfg path` | ConfigStore.cpp:200-203 |
| 整节替换/删节（rename/delete 内部技巧） | `box rename/delete` | 04 §8.15 |
| GetSnapshots/Take/Remove/Select/SetSnapshotInfo | `box snapshot list/take/remove/select/set-info` | Snapshots.cpp（757 行） |
| GetTemplates(生效列表)/应用/撤销 | `template check/list/apply/revoke/info` | Templates.cpp（390 行） |
| GetLog（SBIE 消息泵/FormatMessage） | `log watch/dump`（含 2199 恢复事件呈现） | LogPump.cpp；04 §9-12 |
| interactive queue 事件（FileMigration/InetBlockade） | `log watch --interactive`（直连）；server 泵自动应答 | 04 §12.4-6 |
| 进程启动通知/退出码 | SBIE1399 事件 / `proc start --wait` 透传 | 04 §10 |
| SendQueueRpl（无人值守应答） | server 聚合泵 retval 策略 | 04 §12.4-2 |
| 密码传输通道（SBIE_INI password 域） | `--password`/`SBIE_PASS`（直连路径全通） | Cli.h:27；ConfigStore |

## 3. 缺口表

> **完成登记（补缺波次，2026-09-27）**：下表 ✅ 前缀项已收口——P0-1/2/3/4/
> 6/7/8/9/10/11 与 P1-4/5（client 命令 + server op + params 接线全链路），
> 实现与实测记录见 docs/04 §13。
> **完成登记（box size / recover 波次，2026-09-27）**：P0-5、P0-12 收口
> （P0 全清），实现与实测记录见 docs/04 §14。
> **完成登记（P1 清尾波次，2026-09-27）**：P1-1/2/3/6/7 收口（P1 仅剩
> 无——7 项全清），实现与实测记录见 docs/04 §15。未标记项（P2）维持原状。

### 3.1 P0 — 功能完整性阻塞（12 项）

| # | 缺口 | Plus 侧对应能力 | 当前状态 | 实现建议（SbieCore API / SbieSvc 消息 / 改动文件） | 规模 |
|---|---|---|---|---|---|
| ✅ P0-1 | `proc kill-all`（按沙箱终止全部进程） | `CSbieAPI::TerminateAll` / `CSandBox::TerminateAll`；SbieCtrl ID_SANDBOX_TERMINATE | **桩**：client `Commands.cpp:568` + server Stub `Dispatcher.cpp:1197`；Model `ProcessRepository::KillBox`/`BoxRepository::TerminateAll` **已实现**（Processes.cpp:72-75、Boxes.cpp:248-251，走 `MSGID_PROCESS_KILL_ALL`） | client handler 接线（IPC `proc.killAll` + 降级直连 Model）；server 补 HProcKillAll（SvcProxy 线程）。改动：`cli\Commands\proc_cmd.cpp`、`server\Dispatcher.cpp` | 微(client)+小(server) |
| ✅ P0-2 | `proc suspend`/`resume` | `CBoxedProcess::SetSuspended`；SbieCtrl ID_PROCESS_TERMINATE 邻域 | **桩**：client `Commands.cpp:569-570` + server Stub `Dispatcher.cpp:1200-1205`；Model `Suspend/Resume` **已实现**（Processes.cpp:77-85，`MSGID_PROCESS_SUSPEND_RESUME_ONE`） | 同上：两命令接线 + HProcSuspend/HProcResume | 微+微 |
| ✅ P0-3 | `box enable`/`disable` | `CSandBox::SetEnabled` 语义（Enabled=y/n）；SandMan 右键"启用/停用" | **桩**：client `Commands.cpp:561-562` + server Stub `Dispatcher.cpp:1188`；Model `BoxRepository::SetEnabled` **已实现**（Boxes.cpp:236-246） | client 接线（op `box.setEnabled` + 直连）；server HBoxSetEnabled | 微+微 |
| ✅ P0-4 | `box clean`（清空沙箱内容、保留节） | `CSandBox::CleanBox`（NeverDelete 检查→SB_DeleteProtect、非空拒→SB_DeleteNotEmpty、`CleanBoxFolders` 递归删 FileRoot，SandBox.cpp:205-216） | **桩**：client `Commands.cpp:563` + server Stub；Model 无独立 Clean，但**部件齐备**（TerminateAll + NeverDelete 读取 + `DeleteDirRecursive`） | Model 加 `BoxRepository::Clean(name)`（= NeverDelete 拒 → TerminateAll → DeleteDirRecursive(FileRoot)，语义对齐 CleanBox：有进程可先杀再清或拒，建议拒+提示 kill-all 与 delete 一致）；client/server 各接线。改动：`SbieCore\Model\Boxes.cpp`、`box_manage.cpp`、`Dispatcher.cpp` | 小 |
| ✅ P0-5 | `box size`（磁盘占用） | Plus 侧 `BoxSize/UpdateBoxSize`（SandMan 扩展，未读）；许可内基准 = 资源管理器"目录大小"（01 §5.2 已裁定禁参考 BoxMonitor） | **已收口**（04 §14）：Model `ScanBoxSize` 实现（FindFirstFileExW 递归、重解析点跳过、契约 additive 扩展 `BoxUsageStats{total_bytes,files,dirs}`）；client `box size`（box_manage.cpp）+ server HBoxSize 全接线。实测与 `dir /a /s` / PowerShell(-Force) **逐字节一致**（1,159,498 B / 11 files / 4 dirs） | ~~三重桩~~ | 中（已完成） |
| ✅ P0-6 | `cfg unset`（删值/删整 setting） | `CSbieIni::DelValue`（value 空=整 setting、非空=RemoveValue 单值） | **桩**：client `Commands.cpp:573` + server Stub；Model `ConfigStore::Delete` **已实现**（含 index 重放组合，ConfigStore.cpp:166-193；单值 RemoveValue 语义 §8.22 已修） | client 接线（op `cfg.unset`，参数 setting/section/index）；server HCfgUnset | 微+微 |
| ✅ P0-7 | `cfg lock`（设配置密码） | `CSbieAPI::LockConfig`（`MSGID_SBIE_INI_SET_PASSWORD`，>64 WCHAR 拒；空新密码=解除锁定，SbieAPI.cpp:2238-2251） | **桩**：client `Commands.cpp:575` + server Stub；SvcClient `SetPassword(old,new)` **已实现** | client handler（校验 ≤64 WCHAR→7）；server HCfgLock（SvcProxy）。注意补"空密码=解锁"语义说明 | 微+微 |
| ✅ P0-8 | `cfg unlock`（验证密码并在连接内缓存） | `CSbieAPI::UnlockConfig`（`MSGID_SBIE_INI_TEST_PASSWORD`，成功后缓存 m->Password 供后续写，SbieAPI.cpp:2224-2236） | **桩**：client `Commands.cpp:576` + server Stub；SvcClient `TestPassword` **已实现** | client 接线 + server HCfgUnlock。**语义要补**：IPC 模式下"本连接缓存密码"需要 server 把密码与会话绑定（或依赖 P0-10 修复后每次写显式带 `--password`）；CLI 一命令一进程的现实下，推荐文档化"unlock 仅验证 + 后续写带 --password/SBIE_PASS"，避免 server 端全局明文缓存 | 微+小 |
| ✅ P0-9 | `cfg list-setting`（GlobalSettings 键枚举） | `CSbieIni::GetIniSection`（键名枚举） | **反向缺口**：**server op 已实现**（`Dispatcher.cpp:1164` HCfgListSetting），**client 命令是桩**（`Commands.cpp:574`，未接线） | client handler：IPC 优先 + 直连 `ConfigStore::ListSettings(L"GlobalSettings")`；`--section` 参数化。改动：`cli\Commands\cfg_read.cpp` | 微 |
| ✅ P0-10 | `box set --index` 经 IPC 静默丢值（**正确性缺陷**） | `CSbieIni::SetText` 替换第 i 值 | client 解析 `--index` 但**不发 IPC params**（box_manage.cpp:258-268 仅 name/setting/value/mode）；server HBoxSet **已支持 index**（Dispatcher.cpp:649-668）。server 在场（默认路径）时 `box set X K v --index 1` 被当整 setting 替换 → **其余值静默丢失**（直连路径则正确） | params 增加 `index`（server 已读）；直连/IPC 双路径统一 | 微 |
| ✅ P0-11 | 写命令 `--password`/`SBIE_PASS` 不进 IPC params（**正确性缺陷**） | 密码保护配置的写路径（`SBIE_INI_SETTING_REQ.password`，03 §3.3） | client 不发 password（box_manage.cpp/cfg_set.cpp 的 IPC params 均无）；server 回退读自身环境（Dispatcher.cpp:522-528）= **首个拉起 server 的 client 所设**。后果：`EditPassword` 已设时，后续 client 带 `--password` 走 IPC 仍以 server 侧空/旧密码发 → `WRONG_PASSWORD`(6)，**锁配置写全部失败**（降级直连才正确） | `ipcroute` 写命令 params 统一注入 `password`（box set/create/delete/rename、cfg set/unset、tpl apply/revoke）；server 已支持 | 微 |
| ✅ P0-12 | 文件恢复（快恢复） | SbieCtrl ID_FILE_RECOVER_SAME/ANY、`CQuickRecover::RecoverFile*`（GPL 可复制）；QSbieAPI 面 = `FileToRecover` 信号（2199）、`GetBoxedPath/GetRealPath`（路径映射，SbieAPI.cpp 实读：`\drive\X\`→`X:\`、`\user\current`→用户目录、`\user\all\public`、`\share`→UNC、剥 `snapshot-` 前缀） | **已收口**（04 §14）：新增 `SbieCore\Model\Recovery.{h,cpp}`（List/Copy/MapToRealPath/RecoverAddFolder，映射按 GetRealPath/GetBoxedPath 规则）+ `sbie box recover list\|copy\|add`（box_recover.cpp）+ server 三 op（recover.list/copy/add；**真实文件 IO 在 server 侧**，架构决策见 04 §14）。copy 为拷贝语义（CopyFileW 保留 mtime，非 SbieCtrl 的移动语义）；`--to` 保留 FileRoot 相对结构；实测：Documents 三文件（含子目录）→ 原位/`--to`/index/路径选择子/`--overwrite`/已存在报错（rc 1）全通，内容与 mtime 逐字节一致 | ~~缺失~~ | 中（已完成） |

### 3.2 P1 — 常用功能（7 项，已全清）

| # | 缺口 | Plus 侧对应能力 | 当前状态 | 实现建议 | 规模 |
|---|---|---|---|---|---|
| ✅ P1-1 | 禁用强制运行 | `CSbieAPI::DisableForceProcess(Set,Seconds)` / `AreForceProcessDisabled`（原始 IOCTL `API_DISABLE_FORCE_PROCESS`，SbieAPI.cpp:2593-2607；SbieCtrl ID_DISABLE_FORCE 托盘常驻项 + DisableForceDialog） | **已收口**（04 §15）：DriverApi 薄封装 `DisableForceProcess(set,get)`；`sbie force on [<seconds>]\|off\|status`；server op `force.set/force.status`（直驱动无 SbieSvc；剩余秒=server 记录时刻推算）；行为验证经 ForceProcess 探针（禁用窗口内探针沙箱外运行）；坑：SESSION 块随 leader 退出释放 → server stop 即清状态（§8.24） | ~~缺失~~ | 小（已完成） |
| ✅ P1-2 | 组件维护（启停驱动/服务） | `CSbieUtils::Start/Stop/IsRunning(eDriver/eService/eAll)`（LGPL 参考；底层 KmdUtil.exe 为 GPL core，运行时调用不受限）；SandMan"维护"菜单 | **已收口**（04 §15）：`sbie maint status\|start\|stop [--driver\|--service\|--all]`——服务=进程内 SCM（SbieSvc 注册名核实 my_version.h:66）、驱动=KmdUtil.exe（RunFromHome，命令形态核实 kmdutil.c/SandboxieVS.nsi）；机器级操作无 IPC op、只在显式命令时执行；实测 service 往返+还原；**driver 启停实现未实测**（任务边界，§15 遗留 1） | ~~缺失~~ | 中（已完成） |
| ✅ P1-3 | 全局"终止所有沙箱进程" | `CSbieAPI::TerminateAll()`（遍历全部 box）；SbieCtrl ID_TERMINATE_ALL（托盘/文件菜单） | **已收口**（04 §15）：`proc kill-all --all`（无 box）= EnumBoxes 循环 KillBox；server op proc.killAll box 可空；计数=各 box 请求时进程数之和（data 增 boxes 字段）；按 box 形态兼容不变 | ~~缺失~~ | 小（已完成） |
| ✅ P1-4 | `box list-setting --all` 恒直连 | （内部一致性，非 Plus 项） | server 已参数化 `no_tmpls`（04 §12.4-4），client `--all` 时仍跳过 IPC（box_manage.cpp:341-343） | params 加 `no_tmpls:false`，删本地特判 | 微 |
| ✅ P1-5 | `box/cfg get` 环境变量展开语义在 IPC 路径漂移 | `CSbieIni::GetText(bNoExpand)` | server 恒 noExpand（等价 --raw），client 不发 `noexpand`（box_manage.cpp:133 注释自认）；非 raw（展开 `%env%`）仅直连成立 → 两路径值可不同 | params 发 `noexpand=!raw`（server 已支持该参数，04 §12.2 对拍表） | 微 |
| ✅ P1-6 | `cfg set --no-refresh` 不进 IPC params | `SBIE_INI_SETTING_REQ.refresh`（03 §3.3） | **已收口**（04 §15）：cfg.set/box.set params 恒发 refresh；实测 IPC 路径 --no-refresh 生效（缓存/盘延迟可见，后续 refresh=true 写提交）。**语义修正**：refresh=false 变更仅存 SbieSvc 内存树（04 §8.2 原表述已改——任何 reload 通知清缓存丢未提交变更） | ~~未发~~ | 微（已完成） |
| ✅ P1-7 | `proc start` 的 cwd/env 语义差 | `RunSandboxed(WrkDir)`（QSbieAPI 语义=调用方目录继承） | **cwd 已收口**（04 §15）：`--dir` 缺省时 client 恒显式发 GetCurrentDirectoryW（ipc 实测 workdir=client cwd）；**env 差异维持文档化**（04 §12.4 坑 5 + §15 遗留 3：长期方案 `--env K=V` 透传） | ~~微 / 小~~（cwd 完成；env 注记） | 微（已完成） |

### 3.3 P2 — 增强（14 项）

| # | 缺口 | Plus 侧对应能力 | 实现建议 | 规模 |
|---|---|---|---|---|
| P2-1 | trace/监控命令组 | `EnableMonitor/IsMonitoring/GetTrace/ClearTrace`（SbieApi_MonitorControl/MonitorGetEx 绑定**已有未用**；MONITOR_GET2 需 SbieApi_Ioctl，02 §3.6）；SbieCtrl MonitorDialog | `sbie trace status/on/off/dump/clear`；server 侧泵线程扩展 | 中 |
| P2-2 | 模板"应用检测" | `CSbieTemplates::RunCheck`（注册表/文件/COM 类/服务/产品检测→eRequired/eEnabled 标注，SbieTemplates.h:14-64）；SbieCtrl ThirdPartyDialog | 现 `template check` 只列生效模板（04 §9）；加 `template scan [--class]` 做安装检测（注册表/文件探测为纯读，实现可直接参考 SbieTemplates.cpp 行为） | 中 |
| P2-3 | 快照默认标记 | `CSandBox::SetDefaultSnapshot/GetDefaultSnapshot` | `box snapshot default <name> <id>`（写 Snapshots.ini，Model Snapshots.cpp 内加一法） | 微 |
| P2-4 | box 空置/初始化状态 | `CSandBox::IsEmpty/IsInitialized` | `box info` 加两字段（FileRoot 下 drive/user 目录存在性探测） | 微 |
| P2-5 | proc info 图像类型/令牌明细 | `CBoxedProcess::GetImageType/HasElevatedToken/HasSystemToken/HasFakeToken/HasRestrictedToken/HasAppContainerToken/IsWoW64` | `proc info` 加列：`QueryProcessInfo('gpit')` 与 SBIE_FLAG_* 位解析（flags 原始值已出） | 微 |
| P2-6 | box 级整体挂起/恢复 | `CSandBox::SetSuspendedAll`；SvcClient `SuspendResumeAll` **已实现未接线** | `proc suspend-box <box>`/`resume-box`（或 `--box` 参数） | 微 |
| P2-7 | 组件装卸 | `CSbieUtils::Install/Uninstall/IsInstalled` | `sbie maint install/uninstall`（KmdUtil；需管理员） | 中 |
| P2-8 | 外壳集成 | `CSbieUtils::AddContextMenu1-4/CreateShortcut/GetStartMenuShortcut` | `sbie shell add/remove-context`、`sbie shortcut-create`（注册表/lnk 写入，SbieUtils.cpp 行为参考） | 小-中 |
| P2-9 | 节全量导出 | `CSbieIni::GetIniSection`（名+值+类型一次列全） | `box dump <name>`/`cfg dump [--section]`（list-setting+get 组合的便捷化，--json 含模板/全局回退开关） | 微 |
| P2-10 | interactive y/N 人工决策经 server | `SendQueueRpl`（UI 决策回填） | server 模式现为自动拒绝+事件呈现（04 §12.4-6）；加请求-应答 op（`iq.ask`/`iq.answer`）+ `log watch --interactive` 往返 | 中 |
| P2-11 | 驱动缓存旁路写 | `SbieIniSetDrv`（`SbieApi_UpdateConf`，绑定已有） | 边缘场景（改缓存不落盘）；`cfg set --drv-cache` | 微 |
| P2-12 | 当前用户/权限查询 | `GetCurrentUserName/Sid`、`IniGetUser(admin,section)`（SvcClient 已实现未暴露） | `cfg whoami`（用户/节名/是否管理员）——多用户环境排障用 | 微 |
| P2-13 | 在资源管理器打开沙箱 | SbieCtrl ID_SANDBOX_EXPLORE | `box explore <name>`（ShellExecute explorer FileRoot）——亦可 N-A（用户自行拼路径），列为建议 | 微 |
| P2-14 | 进程豁免开关 | `Set/GetProcessExemption`（`API_PROCESS_EXEMPTION_CONTROL`，Ioctl 可达） | `sbie proc exempt <pid> on/off`（探索性，Plus 用于特殊放行） | 微 |

### 3.4 N-A — 明确不适用（16 项）

| # | Plus 能力 | 不适用理由 |
|---|---|---|
| N-A-1 | ImBox 加密盘全家（Create/Mount/Unmount/Enum/Query/ExecImDisk、GetMountRoot） | **许可证禁用**（SandboxieTools 无许可证，01 §2 已裁定第一期不做） |
| N-A-2 | 托盘/常驻/置顶/窗口布局/新手向导/tips（ID_SHOW_WINDOW/ID_VIEW_TOPMOST/ID_SANDBOX_SET_LAYOUT…） | GUI 域 |
| N-A-3 | Finder 窗口探测（ID_FINDER_OPEN，"即时跳转"沙箱定位） | GUI 域（底层即 P2-14 的豁免机制，CLI 形态由 proc list/info 覆盖定位需求） |
| N-A-4 | 资源监视器（ID_RESOURCE_MONITOR）、窗口标题映射（UpdateWindowMap/GetProcessTitle/GetProcessWindows） | GUI 域 |
| N-A-5 | RunUpdateUtility / 在线更新 | 产品域（UpdUtil 属 SandboxieTools，许可证禁用） |
| N-A-6 | 安全擦除（ID_DELETE_SDELETE/ERASERL/ERASER6） | 外部工具编排（SDelete/Eraser 非本仓库件）；`box clean/delete` 已提供普通删除 |
| N-A-7 | RunBrowser/RunMailer 专用入口（ID_SANDBOX_RUN_BROWSER/MAILER） | `RunBrowser/RunMailer` 为配置键：`proc start <box> <默认程序>` + `cfg get RunBrowser` 通用组合即等价，无需专用命令 |
| N-A-8 | WatchIni（QFileSystemWatcher） | 事件驱动 UI 机制；CLI 按需读取 + `cfg reload` 已覆盖 |
| N-A-9 | CommitIniChanges（批量延迟写） | UI 缓冲机制；CLI 逐条即时经 SbieSvc 落盘 |
| N-A-10 | MkNewName（"NewBox (2)" 自动命名） | UI 命名生成；CLI 由调用方定名 |
| N-A-11 | OpenBox/CloseBox | GUI 开合动画钩子 |
| N-A-12 | RC4Crypt / TestSignature / Set/GetSecureParam | Plus 内部机制（哈希存储/签名校验/防篡改参数），无 CLI 用户面 |
| N-A-13 | IsWow64 | 本构建 x64-only |
| N-A-14 | ResolveSymbols/GetSymbol | GUI 崩溃栈符号化 |
| N-A-15 | 开机自启（SbieCtrl_AutoStartAgent）/沙箱自启配置 | GUI 域（任务点名：标 N-A）；若需脚本化，本质是注册表 Run 键/服务项，属 P1-2 维护域外沿 |
| N-A-16 | 内部工具函数（GetStartPath/GetSessionID/Nt2DosPath/GetVolumeSN/UpdateDriveLetters/ResolveAbsolutePath/MkEnvironment） | 已由 PathMapper/drv 薄封装内部等价，无独立命令需求（P0-12 落地时 ResolveAbsolutePath 语义随 Recovery 进 Model） |

## 4. 特别审计（任务点名九项）

1. **文件恢复（RecoverFiles/GetRecoveredFiles）**：该两名非 QSbieAPI 方法（SandMan 扩展）。QSbieAPI 面
   = `FileToRecover` 信号（log 2199，**已实现**为 `log watch` 事件呈现）+ `GetBoxedPath/GetRealPath`
   （映射规则实读 SbieAPI.cpp：`\drive\X\`→`X:\`、`\user\current`→profile、`\user\all`→ProgramData、
   `\user\public`、`\share`→UNC、剥 `\snapshot-*`）。SbieCtrl 恢复=扫描 `RecoverFolder` 设置对应的
   沙箱内目录（BoxFile.cpp:166-200）+ CopyFileW 拷出（QuickRecover.cpp:1091-1298，GPL 可复制）。
   **CLI 等价：无（P0-12）**。恢复是 Sandboxie 核心用户功能（SbieCtrl 一级菜单 + 每文件右键 +
   自动恢复三入口），缺它无法宣称 Plus 功能完整性。
2. **沙箱内容清理（CleanBox）**：**缺失**（P0-4）。语义基准实读：NeverDelete=y → SB_DeleteProtect；
   有活动进程 → SB_DeleteNotEmpty；否则递归删 FileRoot 内容、**保留 ini 节**。OSS 的 `box delete
   --files` 已含同款目录删除器（含只读属性/句柄重试，Boxes.cpp:33-76），clean = 其"保节"变体 +
   前置检查，规模小。
3. **磁盘占用（BoxSize/UpdateBoxSize）**：**三重桩**（P0-5，client/server/Model 全占位）。许可内
   行为基准已裁定为"递归目录大小统计"（01 §5.2），无协议依赖，纯 FindFirstFileW。
4. **立即停止所有进程（TerminateAll）**：**缺失**（P0-1；全局形态 P1-3）。注意 Model/SvcClient 层
   (`KillAll`→`MSGID_PROCESS_KILL_ALL` 0x1204) **已就绪且经 §8.10-22 修复验证**，纯接线工作。
5. **配置密码保护（Lock/Unlock ini）**：**缺失**（P0-7/8）。SvcClient `SetPassword/TestPassword`
   已实现；`IsConfigLocked` 判定 Model `ConfigStore::Locked()` 已实现但无 CLI 呈现——建议 `cfg
   lock`/`unlock`/`status` 三件套输出锁定态（QSbieAPI IsConfigLocked 语义：EditPassword 非空且本端
   无缓存密码）。另注意"空新密码=解除锁定"语义（SbieAPI.cpp:2248 经 SET_PASSWORD 空串实现）。
6. **禁用强制运行（DisableForceProcess 系列）**：**缺失**（P1-1）。实现无 SbieSvc 依赖：原始 IOCTL
   （`API_DISABLE_FORCE_PROCESS_ARGS{set_flag/get_flag}`），`SbieApi_Ioctl` 绑定已在表；配置面
   `ForceDisableSeconds` 走通用 `cfg set`。
7. **菜单触发类（探索性）**：资源监视器/Finder/布局/提示/tips → N-A（GUI）；Explore → 建议 P2-13
   （一行 ShellExecute）；Run Browser/Mailer → N-A-7（通用组合已覆盖）；编辑 ini → N-A（`cfg path`
   已给路径）；第三方模板检测 → P2-2。
8. **开机自启/托盘类**：N-A-2/15（GUI 域）。唯一相邻的非 GUI 项是组件维护启停（P1-2），已单独列。
9. **box options 完整键面**：**验证通过——`box set/get/list-setting` 的通用键面足够，无需逐键命令**。
   实测证据：任意键读写（§9-11 验收 TestOssOption/Template 多值/`--index`/`--all` 模板注入项 6→16）；
   SbieCtrl 全部选项页（BoxPage/AppSettings/Alert/Linger/Leader/Internet/Recover/Delete 选项）与
   SandMan 选项页最终都落 plain ini 键值，通用 set/get 覆盖；四写模式（update/append/insert/index
   重放）与 `CSbieIni::ESetMode` 对齐。剩余问题只有三个**参数接线小坑**：`--all` 恒直连（P1-4）、
   `--raw`/noexpand 不发（P1-5）、`--index`/`--password` 不发（P0-10/11）。键值对型设置
   （`SetTextMap`，如 `OpenIpcPath=程序,路径`）以字符串 append 即等价，无独立需求。

## 5. 第三波"已知未完成项"核实（含两处纠错）

| 第三波报告表述 | 实读核实 |
|---|---|
| "server Dispatcher 仍为 Stub：box.setEnabled/clean/size、proc.killAll/suspend/resume、cfg.unset/lock/unlock、tpl.list/info/check（client 侧已直连实现，server stub 触发降级）" | server Stub 清单**属实**（Dispatcher.cpp:1188-1223，13 op）。但"client 侧已直连实现"**只对 tpl.list/info/check 成立**（cfg_read.cpp:282-286 真实现，运行时按 code==103 精确降级，功能不受损——**该三项降级可接受**，补 server op 仅为路径统一，各 微）。**纠错一**：box enable/disable/clean/size、proc kill-all/suspend/resume、cfg unset/lock/unlock 的 **client 命令本身也是桩**（Commands.cpp:561-576 `CmdNotImplemented`，从未被覆盖）——这九个不是"降级"而是**整链缺失**（Model/Svc 层大多已备），即本表 P0-1..P0-8。**纠错二**：`cfg list-setting` 相反——server op 已实现（Dispatcher.cpp:1164）而 client 命令是桩（P0-9） |
| "client 预接线缺口：box set --index、cfg set --no-refresh、--password 未进 IPC params（server 已支持）" | **属实并升级**：`--index` 缺失在默认（server 在场）路径造成**静默数据丢失**（HBoxSet 把整 setting 替换为单值）、`--password` 缺失使**锁配置下经 IPC 的写全部 WRONG_PASSWORD**——按正确性缺陷列 P0-10/11；`--no-refresh` 仅语义漂移列 P1-6。server 侧支持核实：HBoxSet 读 index/password/refresh（Dispatcher.cpp:639-668）、ResolvePasswordParam（:522-528） |
| "proc start --wait 恒直连" | 属实（proc_cmd.cpp:152 "if (!wait)" 注释自明）。**评估：可接受**——--wait 依赖本进程持有 hProcess 等待退出码，句柄无法过管道；替代（server 代理等待+回传码）需新 op 且中断语义复杂。维持现状，文档已注 |
| "proc start cwd/env 语义差（docs §12.4）" | 属实，P1-7。cwd 修复 微（client 显式发 cwd）；env 继承差异（server 继承首个 client 环境）短期文档化即可。**【已收口（P1 清尾波次，04 §15）：cwd 恒发 client cwd；env 差异文档化 + `--env K=V` 长期注记】** |

另登记两处文档性偏差（非功能缺口）：docs 04 §4.2 写 `[--idle <sec>]` 实现为 `--idle-timeout`；
04 §2 命令树含 `snapshot info` 动词而 §4.3 表与实现均无（list 的 --json 已含 info 字段）。
`sbie-cli --help` 文案仍停留在 M1（"other command groups … later wave"，Cli.cpp:20-39），宜随接线更新。

## 6. 结论

### 6.1 达到"完整实现 Plus 功能（CLI 形态）"还差的工作

> **状态更新（2026-09-27，box size / recover 波次）**：P0 12 项**全部收口**
> （§3.1 全表 ✅；末两项 P0-5/P0-12 见 docs/04 §14）。
> **状态更新（2026-09-27，P1 清尾波次）**：P1 7 项**全部收口**
> （§3.2 全表 ✅；见 docs/04 §15——P1-2 的 driver 启停分支实现未实测）。
> 本节原始评估保留如下供历史对照。

**P0（12 项，阻塞完整性声明）**：
终止/挂起簇（kill-all、suspend/resume）与启用开关（enable/disable）四组纯接线（Model 已备，
各 微-小）；clean（小，部件齐备）；size（中，ScanBoxSize 需真实现）；cfg unset/lock/unlock/list-setting
（微-小，SvcClient/Model/server 多已备）；**两处 IPC 参数正确性缺陷**（--index 静默丢值、--password
锁配置写必败，各 微，是最优先应修的正确性问题）；**文件恢复**（中，唯一需要新建 Model 的 P0）。

P0 规模合计约：微×8 + 小×3 + 中×2（其中 9 项是"接线"性质——Model/SvcClient/server 三层中至少
一层已实现）。

**P1（7 项，日常可用性）**：禁用强制运行（小）、组件维护启停（中）、全局终止（小）、
以及四项 client 参数接线补漏（微×4，其中 cwd/env 含少量语义工作）。

**P2（14 项）** 与 **N-A（16 项）** 见 §3.3/§3.4；N-A 中 ImBox/更新器两项为许可证/产品边界，
永久不做。

### 6.2 总体判断

- **读路径与既有写路径已覆盖 Plus 核心面的绝大部分**：box/proc/cfg/template/snapshot/log 七组命令
  的读侧与主要写侧（create/rename/delete/set/snapshot 全家/模板应用）均已实现并经四波实测验收
  （04 §9-§12），且错误码/JSON/降级三层语义稳定。
- **缺口高度集中在五个功能簇**：终止类（kill-all/suspend）、清理与容量（clean/size）、配置锁
  （lock/unlock/unset）、参数接线（index/password/noexpand/no_tmpls/cwd）、文件恢复。除恢复与
  size 外全部为"下层已备、上层接线"的微/小工作量。
- **一处结构性风险**：client/server 双端 stub 面存在**互相等待**的错位（client 桩的 9 个命令中
  server 侧也被当"后续波次"，而 server 实现的 cfg.listSetting 又无人调用），建议下一波次以本表
  P0-1..P0-9 为清单一次收口，避免降级路径长期承担主路径职责。
