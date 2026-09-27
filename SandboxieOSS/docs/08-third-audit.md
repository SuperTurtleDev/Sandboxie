# 08 — 第三轮审计：三轴能力面对照 + 自审一致性

审计日期：2026-09-27。本文为只读审计产物：未改动任何代码；文档级修复仅限
docs\04（§4.2/§4.3 两处回填，均标注来源 08）。实测部分（--help/--show-transport/
--json）全部为只读命令，无测试残留。

> **波次 E 状态回填（2026-09-27，实施后追加）**：审计正文与差距表原文未动；
> §2.2/§2.3/§3.2/§5.2/§5.3 增"状态（波次 E 回填）"列/注记，逐项登记收口情况
> （08-P1×2、08-P2-4/5/6/7、tpl 三连 op 收口；08-P2-1/2 留波次 F；
> SBIE_INI_TEMPLATE 判定处置见 §2.3）。实施与实测详录：docs\04 §22。

与前两轮的视角分工：**06 = QSbieAPI 方法面**（API 签名级对照），**07 = SandMan
用户可见功能面**（菜单/页面/向导/窗口），**08（本文）= 三轴能力面**（SbieDll 导出面 /
SbieSvc 消息面 / 配置键执行者面）+ **自审一致性**（文档 vs 二进制 vs 代码 + op 面
完整性 + 输出信封）+ **上游动态**。SandMan 侧仅记录规格语义，未复制任何代码。

## 0. 摘要

- **新发现：P1×2、P2×7**；三轮累计后 CLI 完整度重核为 **≈96%（基数 128 项，
  覆盖 123）**——07 文本的 ≈91% 是波次中的保守快照，其 §5.1 表内行值未随波次
  收口更新（行计数与备注自相矛盾），本文按 07 口径逐项重核修正（§5.1）。
- **两项 P1**（一为"键可写但静默失效"族新成员，一为正确性小缺陷）：
  1. **08-P1-1**：`ExcludeFromTerminateAll` 全局终止时不生效——QSbieAPI 的全局
     TerminateAll 会跳过设了该键的沙箱（SbieAPI.cpp:1786-1792），OSS 的
     `proc kill-all --all`（client 与 server 两处枚举点）全树零引用该键。
  2. **08-P1-2**：log 文案 ≥3 插入串丢失——SbieDll 导出有数组变体
     `SbieDll_FormatMessage(code, const WCHAR** ins)`（support.c:830，可传 4–6
     插入），OSS 绑定表只绑了 0/1/2 三个定参变体，server LogPump.cpp:135-141 与
     client log_cmd.cpp:124-130 都只把**前两个**插入串传给格式化器——消息表存在
     数十条 %3 文案与 4 条 %4 文案（含 SBIE2201 箱容量提示），第 3+ 插入串内容
     在渲染文本中丢失。
- **自审一致性总体结论：文档滞后于二进制、二进制好于文档**。`--help` 输出是
  命令面的**超集**（含 copy/export/import/types、--no-guardians、--move 等波次
  A/B/D3 增项），docs\04 §4 命令树未回填且有 1 个幽灵动词（`snapshot info`）；
  JSON 信封（8 命令 ×2 形态实测）**零漂移**，全部合规 04 §7.2。

---

## 1. 方法与五轴清单

### 1.1 轴1 — SbieDll 导出面（137 项 → 绑定 32 项）

- **导出枚举**：PE 导出表直接解析（PowerShell 读 `C:\Program Files\
  Sandboxie-Plus\SbieDll.dll` 字节流，Export Directory names 数组），得 **137 项
  命名导出**——与 06/04 §8.14 前任核对数一致（枚举过程工作文件已清理）。
- **绑定表**：实读 `SbieCore\DriverApi\DriverApi.h:165-208`（struct Api 成员）+
  `DriverApi.cpp:78-`（GetProcAddress 表），**32 项**，与 02 §3/§8.8 登记一致。
- **语义核对**：对全部未绑定且疑似有价值的导出，回读 `Sandboxie\core\dll\
  sbieapi.h` / `sbiedll.h` / `support.c` / `process_api.c` 原型与实现定分类。

### 1.2 轴2 — SbieSvc 消息面（vendor\msgids.h → SvcClient 24 个）

- 全量枚举 `vendor\msgids.h`：**105 个 `#define MSGID_*` 行**（含 3 行注释掉的
  旧 id、13 个组基址 id、4 个通知 id，**操作型 MSGID ≈85 个**）；
  对照 `SbieCore\SvcClient\` 与 `SbieCore\Model\` 的实际 MSGID 引用——
  **SvcClient 实现 24 个**（SBIE_INI 10 + PROCESS 6 + QUEUE 3 + IMBOX 5）。
- 对每个未实现 MSGID 回读 `Sandboxie\core\svc\*` 服务端 handler，判定"客户端
  是谁"（沙箱内代理 / 管理端 / GUI）后归类。

### 1.3 轴3 — 配置键面执行者审计

- 驱动侧：`core\drv\**` 全部 `Conf_Get*` 调用点（显式键名 ~16 处）+ 通用规则
  引擎读面（`proc->box->name, setting` 形态）。
- 服务侧：`core\svc\**` 读取的 ~40 键（进程启动时行为键族）。
- 沙箱内 DLL：`core\dll\**` 读取的 ~60 键（in-sandbox 执行者）。
- **箱内服务代理**：`apps\com\RpcSs\linger.c`（Linger/Leader 执行者——本轮关键
  新证据）。
- 模板资产：`install\Templates.ini` 的 `[TemplateSettings]` 默认变量（21 个
  `Tmpl.<Browser>` 形）+ `Tmpl.*` 元键分布（Title/Class/Url/Scan 系 8 类）。

### 1.4 轴4 — 自审一致性（实测型）

- 4a：`sbie-cli.exe --help`（顶层）+ 10 个命令组 `--help`（box/proc/cfg/template/
  log/trace/force/maint/img/usb）对照 docs\04 §4 命令树与 §4.2/§4.3 规格表；
  并从代码注册表（`Commands.cpp:570-606` + 各 Register 块）提取**实际命令全集**
  做三方核对。
- 4b：`server\Dispatcher.cpp:2466-2560` 全部 op 注册（61 个 kOp 常量对照）vs
  Stub 残留；client 侧 27 个命令文件的 `ipcroute::Invoke` 计数 + 显式直连注释
  （`proc start --wait`、`log watch --interactive`、proc_d3/box_d3 头注）。
- 4c：8 条命令 × `--show-transport` + `--json` 信封实测（status/version/box
  list/proc list/cfg get/template check/log dump/trace dump，外加 usb/ramdisk
  status 共 10 条 transport 抽查）。

### 1.5 轴5 — 上游动态

- `git log`（本仓 master = 波次线 + d79424d "Merge pull request #1 from
  sandboxie-plus/master"）：逐 commit diff 用户合并的上游 PR；近 3 个月窗口内
  上游（sandboxie-plus/master）的全部可及历史。

## 2. 差距表

> 编号 `08-P1-n / 08-P2-n / 08-N-A-n`，与 06/07 编号空间隔离。每项含
> 【轴号/来源锚点/OSS 现状（实读）/建议/规模】。06/07 已收口项不重复列。

### 2.1 P0 — 无新发现

三轮对照（方法面/功能面/能力面）后，CLI 核心流程无新增 P0 级缺口；06 P0×12
与 07 P0×2 均已收口（06 §3、07 §3 登记）。

### 2.2 P1 — 有价值且影响语义正确性（2 项）

| # | 缺口【轴/锚点】 | OSS 现状（实读） | 建议 | 规模 | 状态（波次 E 回填） |
|---|---|---|---|---|---|
| 08-P1-1 | **全局终止不读 `ExcludeFromTerminateAll`**【轴3；QSbieAPI SbieAPI.cpp:1786-1792（TerminateAll 遍历 skip 该键，bNoExceptions 逃生）；键面来源 SandMan OptionsStop/OptionsAdvanced（07 §1.3 已列键名但两轮均未审执行侧）】 | 全树零引用（除 `thirdparty\QSbieAPI` vendored 副本）：`proc kill-all --all` 的 client 枚举点（proc_cmd.cpp:285-320）与 server HProcKillAll 全局形都直接 EnumBoxes→KillBox。键可写（通用 cfg set），语义静默失效——07-P0-1/2 同族性质，但触发频率低于触发器/守护族，列 P1 | 全局路径按 box 读该键（默认 n）跳过，附 `--no-exceptions` 逃生旗标（对齐 bNoExceptions 语义）；按 box 的单箱 kill-all 维持不检查（QSbieAPI 单箱 TerminateAll 亦不查） | 微（client+server 两处） | **已收口（波 E）**：client proc_cmd.cpp（ExcludedFromTerminateAll 助手）+ server HProcKillAll 两路径均按 box 读键跳过（ConfigStore Get，noExpand/noTemplates 只读 box 节自身）；`--no-exceptions` 逃生旗标（仅全局形合法，单箱/无 --all 时 USAGE 拒）；单箱形维持不查（对齐 QSbieAPI 单箱语义）；文本/JSON 输出增 skipped 计数。实测 docs/04 §22.1 |
| 08-P1-2 | **log 文案 ≥3 插入串丢失**【轴1×轴4；core\dll\support.c:830-922 `SbieDll_FormatMessage(code, const WCHAR** ins)`（内部 ins[6]，FormatMessage ARGUMENT_ARRAY）；消息面 Sbie-English-1033.txt：%3 文案数十条 + %4 文案 4 条（SBIE2201 箱容量、SBIE1235 配置写入失败、SBIE1206 等）】 | 绑定表只绑 0/1/2 定参变体（DriverApi.h:155-157）；server `LogPump.cpp:135-141` 与 client `log_cmd.cpp:124-130` 同构：`ins.size()>=2` 即只取前两个调 FormatMessage2 → %3/%4 位渲染为空，**内容丢失**（回退拼接仅在格式化整体失败时触发，此处不触发） | 绑定表 +1 项 `SbieDll_FormatMessage`（类型 `WCHAR* (CALLBACK*)(ULONG, const WCHAR**)`，传最多 4 个插入的数组，不足补 nullptr）；两处调用点改为数组变体 | 微 | **已收口（波 E）**：绑定表 +1（P_SbieDll_FormatMessage；%N↔ins[N]，前 5 插入置 ins[1..5]，空槽 nullptr）；LogPump FormatEntryText 与 client FormatText 均改数组变体。**回填修正**：实测复核（导出级探测 + 表枚举）SbieMsg.dll 编译入表的 SBIE 1xxx/2xxx 文案均 ≤2 插入——%4 文案属 3xxx txt 家族（未编译入 MSGTAB、不流经驱动日志队列），故本项实为**契约补全**而非活缺陷（1399 记录实携 ~6 插入，遇 %4+ 文案旧代码确会丢）；导出级实测 SBIE1317 %2+%3 完整渲染，docs/04 §22.2 |

### 2.3 P2 — 增强（7 项）

| # | 缺口【轴/锚点】 | OSS 现状（实读） | 建议 | 规模 | 状态（波次 E 回填） |
|---|---|---|---|---|---|
| 08-P2-1 | **box 路径规则审计命令**【轴1；`SbieApi_QueryPathList`（sbieapi.h:169-174：path_code/path_len/path_str/process_id/prepend_level）= API_QUERY_PATH_LIST；02 §2 已列"box info --paths 进阶"未实现】 | 导出未绑、无等价命令。Resource Access 页（07 §1.3）的 open/closed/read/write 四族路径规则的**驱动解析终态**无观测面 | `sbie box rules <name> [--kind open\|closed\|read\|write]`（path_code 对应四族 + prepend_level=false）；排障价值：确认模板注入/规则特异性（UseRuleSpecificity）后的最终列表 | 小 | 未做——波次 F（观测增强，08 §6 建议） |
| 08-P2-2 | **模板文件夹变量专用面**【轴2/轴3；驱动展开 `Conf_Expand_Template`（drv\conf_expand.c:246-288：查 `[TemplateSettings]\变量.用户名` → `[TemplateSettings]\变量`，值 `?`=询问标记）；默认值在 install\Templates.ini `[TemplateSettings]`（Tmpl.Firefox 等 21 变量）；SbieSvc 专写消息 `MSGID_SBIE_INI_TEMPLATE` 0x1806（sbieiniserver.cpp:1099-1176，自动拼 `.username` 后缀免管理员判定）；SandMan 编辑面 OptionsTemplates.cpp:286-341】 | 无专面、代码零引用；**纯键面已可用**——`cfg get/set --section TemplateSettings`（ConfigStore 放行任意 ≤64 节名，ConfigStore.cpp:41 实读；写走通用 SET_SETTING，管理员/密码面照常） | `sbie template folder list|set|clear`：list 呈现"变量 / Templates.ini 默认值 / Sandboxie.ini 覆盖（含 per-user 形）/ 是否 `?`"，set 可选走 0x1806 自动后缀或直写覆盖键 | 小 | 未做——波次 F。**SBIE_INI_TEMPLATE（0x1806）判定处置（波 E）**：不绑定该专写消息——其唯一增益是"自动拼 `.username` 后缀 + 免管理员判定"，而通用 cfg set --section TemplateSettings 直写覆盖键已等价可用（管理员/密码面照常，本仓无 GUI 询问路径，`?` 标记无消费者）；folder 专用面（list 呈现三方对照）仍留波次 F，届时若实现 set 再评估走 0x1806 |
| 08-P2-3 | **template scan 量级固化**（06 P2-2 维持"后续"的依据补强）【轴3；Templates.ini 检测元数据实数：Scan 150 / ScanService 68 / ScanProduct 25 / ScanKey 21 / ScanIpc 6 / ScanFile 3 / ScanWinClass 1 / ScanScript 1 = **275 条**，分布于 ~437 模板节】 | `template check` 仅列生效集；无安装检测。275 条元数据意味着逐模板检测器集确属独立波次量级 | 维持 06/04 §21.3 处置（后续独立波次）；本文固化量级数字供排期 | —（登记） | 登记完结（无代码项）——scan 留独立波次（06 P2-2） |
| 08-P2-4 | **proc suspend-box/resume-box IPC 化**【轴4b；proc_d3.cpp:11-12 自注："纯 client 直连（SbieSvc 消息面）；server op 规格待 D1/D2 接线：proc.suspendBox / proc.resumeBox"】 | 两命令为**写路径**却恒 client 直连（SUSPEND_RESUME_ALL），与单进程 suspend/resume（有 op proc.suspend/resume，Dispatcher.cpp:2506-2507）不一致；iperoute 降级框架本可承载 | server 补 proc.suspendBox/proc.resumeBox（params {box?}，box 空=全局），client 换 ipcroute::Invoke | 微 | **已收口（波 E）**：kOpProcSuspendBox/ResumeBox + HProcSuspendBoxImpl（SuspendResumeAll 经 SvcCall 专职线程）；client IPC 优先、降级直连保留。实测 docs/04 §22.3 |
| 08-P2-5 | **box snapshot default IPC 化**【轴4b；box_d3.cpp:543-600 纯 client 文件操作（Snapshots.ini [Current] Default）】 | 同族其余 5 动词均有 op（box.snap.*，Dispatcher.cpp:2493-2497），唯 default 恒直连——族内路径不一致 | 补 kOpBoxSnapDefault（读/写两形态），client 接线 | 微 | **已收口（波 E）**：kOpBoxSnapDefault + HBoxSnapDefault（读 {name} / 写 {name, id|clear}）；行级改写助手并入 Model SnapshotManager::SetDefault（additive，server/client 两路径共用；UTF-8 无 BOM 整文件 Load→改→Save）。实测 docs/04 §22.3 |
| 08-P2-6 | **组级 --help 非组感知**【轴4a；实测 10 组（box/proc/cfg/template/log/trace/force/maint/img/usb）`<group> --help` 全部打印与顶层完全相同的帮助文本，rc 0】 | Cli 路由把 `--help` 全局拦截；仅 usage 错误路径有 per-command 用法文案（EmitError usage: 行）。注册表（Commands()）已有所需数据 | Route() 对"组名 + --help"形态打印该组子命令清单（组 → 子命令枚举自注册表） | 微 | **已收口（波 E）**：Cli.cpp PrintGroupUsage——`<group> --help` 打印该组子命令清单（注册表枚举，字典序）+ 组级提示（第三级路由/别名/旗标注记）；未注册组退顶层用法（向后兼容）；组名前 --help 仍为顶层。实测 docs/04 §22.4 |
| 08-P2-7 | **Tmpl.Hide 过滤**【轴3；Templates.ini 1 条 `Tmpl.Hide=y`（Plus UI 隐藏弃用模板；core 无读者，为 Plus 侧约定）】 | OSS `template list` 不过滤，列全量 437 | `template list` 默认滤 Hide、`--all` 显示（与 Plus 呈现对齐）；价值低，可并入任意波次顺带 | 微 | **已收口（波 E）**：ipcc/TmplHide 助手（Templates.ini + Sandboxie.ini 本地节扫描，小写归一比较；Model\Templates 禁改故独立实现）；server tpl.list 增 all 参数（Sandboxie.ini 路径在 SvcCall 内取得——worker 线程不得自行触 SbieSvc），client `--all` 旗标 + 直连降级同滤。实测：默认 435 / --all 436，ScreenReader（唯一 Hide 条目）默认不列。docs/04 §22.4 |

### 2.4 N-A — 明确不适用（本轮新登记两批，逐类给理由）

**轴1 未绑定导出 105 项的归类**（137 − 32 绑定）：

| 类 | 计数 | 项（代表） | 不适用理由 |
|---|---|---|---|
| 已有等价路径（功能已覆盖） | 14 | `SbieApi_DisableForceProcess`（force on/off 经 SbieApi_Ioctl 直投，DriverApi.cpp:520-530）、`SbieApi_ProcessExemptionControl`（proc exempt ioctl 直投，proc_d3.cpp:529-532）、`SbieDll_KillOne/KillAll`（走 MSGID_PROCESS_KILL_*）、`SbieDll_RunSandboxed`（走 MSGID_PROCESS_RUN_SANDBOXED）、`SbieDll_Queue*`×5（自有 QueueClient）、`SbieDll_QueryConf/UpdateConf`（已绑 SbieApi 系）、`SbieDll_Mount/Unmount`（img ops 经 MountManagerWire）、`SbieApi_GetVersion/QueryProcess/QueryProcessEx`（旧包装，Ex 变体已绑） | 绑定它们只是形态统一，无新能力；维持 02 §3.7 决策 |
| 沙箱内 hook/注入/COM/SCM 基础设施 | ~57 | `SbieDll_Hook*/Inject*/Com*/Scm_Hook*/Match*/AllocMem…`、`SbieApi_HookTramp` | 客户端是沙箱内进程/SbieSvc 注入侧，管理端 CLI 无调用语境 |
| 沙箱内辅助/UI/语言 | ~12 | `SbieDll_GetBorderColor/GetLanguage/GetDrivePath/GetUserPathEx/DisableCHPE…` | in-sandbox 行为或 GUI；CLI 无 i18n 面 |
| 沙箱内上报/代理侧 | ~14 | `SbieApi_MonitorPut*`×5（trace 上报侧；管理端只读 MonitorGet2）、`SbieApi_CheckInternetAccess/GetBlockedDll/SetUserName/GetUnmountHive/QueryProcessInfoEx`（ext_data 形参用于 SbieSvc 内部 info 码，process_api.c:474/528/544/565） | 驱动内部/SbieSvc 专用语义 |
| 诊断写日志族 | 5 | `SbieApi_Log/LogEx/vLogEx/LogMsgEx/LogMsgExt` | QSbieAPI 无对应方法；CLI 制造事件可用 `proc start`；若将来要 log 管道自测命令可升级为 P2 |
| **缺口（→ 上表）** | 2 | `SbieDll_FormatMessage`（08-P1-2）、`SbieApi_QueryPathList`（08-P2-1） | — |
| 边缘可议 | 1 | `SbieApi_GetFileName`（API_GET_FILE_NAME，句柄→NT 路径） | 恢复/排障可用的底层件，但现有 PathMapper/Recovery 已覆盖用户面；不单列 |

**轴2 未实现操作型 MSGID ~61 个的归类**（操作型 ≈85 − SvcClient 24；逐个回读
服务端 handler 定客户端归属）：

| 类 | 计数 | 项 | 不适用理由 |
|---|---|---|---|
| 沙箱内代理（客户端=沙箱内进程） | ~51 | PSTORE 6、SERVICE 5（boxed SCM 代理）、TERMINAL 5、NAMED_PIPE 9、FILE 7、NETAPI 1、COM 10、IPHLP 4、EPMAPPER 1、QUEUE_PUTREQ/GETRPL/STARTUP 3 | 这些 pipe 面的请求方是 SbieDll 沙箱内代理（Scm_Hook/NAMED_PIPE 转发等）；管理端调用无语义 |
| 进程组补充 | 5 | CHECK_INIT_COMPLETE、GET_WORK_DEPRECATED、SET/OPEN_DEVICE_MAP、RUN_UPDATER | 前四=沙箱内/弃用；RUN_UPDATER=更新器（产品域，许可证禁） |
| SBIE_INI 补充 | 4 | GET_WAIT_HANDLE（QSbieAPI 连接等待内部件）、RUN_SBIE_CTRL（GUI）、RC4_CRYPT（Plus 内部防篡改）、**SBIE_INI_TEMPLATE**（→ 08-P2-2，唯一有价值） | 前三无 CLI 面 |
| IMBOX 补充 | 1 | IMBOX_UPDATE（0x1D06） | **上游本身未实现**（MountManager.cpp:472-485 返回 ERROR_CALL_NOT_IMPLEMENTED "todo"）——非缺口，登记备查 |

**轴3 键面执行者核验通过（键面即功能，非缺口）**：

| 键族 | 执行者（实读证据） | 结论 |
|---|---|---|
| LingerProcess/LeaderProcess/LingerLeniency/LingerExemptWnds | **箱内 RpcSs**（apps\com\RpcSs\linger.c:256-332 读配置、:447-557 终止逻辑）——修正一个潜在误判：执行者不在 GUI，也不在 OSS server 职责内 | 通用 cfg set 写键即生效，无需 OSS 执行者 |
| ForceRestartAll / ForceRestart / NoRestartOnPCA | SbieDll 进程内自重启（dllmain.c:974-991） | 键面即功能 |
| ForceMarkOfTheWeb / MarkOfTheWebBox / ForceBoxDocs | 驱动（drv\process_force.c） | 键面即功能 |
| AutoExec / StartProgram / StartService | SbieDll 沙箱启动时执行（07 §2 已记，本轮复核） | 键面即功能 |
| SbieSvc 启动时键族（OriginalToken/StripSystemPrivileges/UseRamDisk/RamDiskSizeKb 等约 40 键） | SbieSvc 进程编排路径（core\svc ~40 处 QueryConf） | 键面即功能（UseRamDisk/RamDisk* 已在 04 §18 CLI 化呈现） |
| SbieCtrl_HideMessage / AlertBeforeStart / StartRunAlertDenied / ForgetPassword / DblClickAction | SandMan UI 行为键 | N-A（数据面等价：log watch --msg 过滤 / proc list） |

## 3. 自审一致性（轴4 全量记录）

### 3.1 docs\04 §4 命令树 vs --help vs 代码注册表（三方核对）

**实际命令全集（代码注册表提取，Commands.cpp:570-606 + 27 个 Register 块）**：
14 组 — status/version、server start/stop/status、box（list/create/types/info/get/
set/list-setting/rename/delete/enable/disable/clean/size/copy/export/import/dump/
explore/recover(list/copy/add)/snapshot(list/take/remove/select/set-info/default))、
proc（list/info/start/kill/kill-all/suspend/resume/suspend-box/resume-box/exempt)、
cfg（get/set/unset/list-setting/reload/path/lock/unlock/whoami/dump)、template
（list/info/apply/revoke/check/gen-browser)、log（watch/dump，messages 别名）、
trace（watch/dump)、force（on/off/status)、maint（status/start/stop/install/
uninstall)、img（list/status/create/mount/unmount)、ramdisk（status)、usb（status/
sync)、doctor。

| 差异 | 方向 | 详情 | 处置 |
|---|---|---|---|
| §4 树缺 `box copy\|export\|import\|types` | 文档滞后 | 波 B/D3 引入（04 §17/§19 有详表）未回填 §4 树；--help 已含 | **已修 docs\04 §4**（本文波次） |
| §4 树含幽灵动词 `snapshot info` | 文档错误 | 实测 `box snapshot info DefaultBox` → `unknown snapshot subcommand: info`（USAGE）；verbs=list/take/remove/select/set-info/default（box_snapshot.cpp:24、Commands_D3.cpp:48）；06 §5 曾指出但 §4 树一直未改 | **已修 docs\04 §4** |
| §4.2 表 `server start [--idle <sec>]` | 文档错误 | 实际旗标 `--idle-timeout N`（server_cmd.cpp:17/66）；另缺波 A 的 `--no-guardians`（server_cmd.cpp:56-69） | **已修 docs\04 §4.2** |
| §4.3 表波次旗标缺失 | 文档滞后 | `box create` 缺 --type/--location/--temp 等（§17/§19）、`box clean` 缺 --no-triggers（§16）、`box recover copy` 缺 --move/--no-check（§17）——§4.3 行文还保留"拷贝语义非移动"旧句 | **已加注 docs\04 §4.3**（指向 §16-§19，不重抄） |
| `--help` 顶层为命令面超集 | 正向 | help 更新至 D3 后（含伪命令路由说明、doctor、--no-guardians 等），文档树落后 | 随上项修复对齐 |
| 组级 `--help` 无组感知 | 缺口 | 10 组实测同一输出 | 08-P2-6（代码级，只列不改） |

其余 12 组动词集合与 §4 树/§4.x 表一致；`log messages` 别名（04 §9）实测在。

### 3.2 Dispatcher op 面 vs Stub vs client 直连

- **op 注册面**：61 个 kOp 常量（ipcc\SbieIpc.h）全部有注册（kOpServerShutdown 在
  ServerMain 工作线程特判，Dispatcher.cpp:2560 注）——**无孤儿常量、无漏注册**。
- **Stub 残留 5 项**：tpl.list / tpl.info / tpl.check（真残缺，client 依赖
  ERR_NOT_IMPLEMENTED 自动降级直连，功能无损——06 §5 已裁定可接受，补齐各 微）；
  log.event / trace.event（server→client 推送面的入向占位，**非缺口**）。
  【波次 E 回填：tpl 三连已换真 handler（HTplList/HTplInfo/HTplCheck），
  Stub 占位函数退役——注册面零 Stub 残留；log.event/trace.event 入向占位维持。】
- **降级特判残留**：IpcRoute.cpp:60-70 的 103 识别 + "not implemented" 消息标记
  双保险仍在（为 tpl 三连服务）；box/cfg 命令文件已无硬编码直连跳过（06 P1-4
  修复后干净）。
- **恒直连清单**（按"是否有 IPC 化理由"分档）：

| 命令 | 直连理由 | 可 IPC 化？ |
|---|---|---|
| proc suspend-box / resume-box | D3 未接线（自注待办） | **是**（08-P2-4） |
| box snapshot default | D3 纯文件操作 | **是**（08-P2-5，族内不一致） |
| cfg whoami / box dump / cfg dump | 只读/本地文件 | 可（低价值；维持亦可） |
| template gen-browser | 注册表探测 + 直写节 | 可（低价值） |
| proc exempt | 直驱动 ioctl，无 SbieSvc | 否（无收益） |
| proc start --wait | 本进程持句柄等退出码（04 §11 裁定） | 否（架构性） |
| log watch --interactive | 需本进程为会话 leader | 否（架构性，06 P2-10 域） |
| maint 全组 / doctor / box explore / box types | 机器级 / 本地诊断 / ShellExecute / 静态表 | 否（设计如此，04 §5） |

### 3.3 输出信封实测（8 命令 ×2 形态）

| 命令 | transport | --json 信封 | 备注 |
|---|---|---|---|
| status | ipc | `{"ok":true,"data":{driver{…},service{…},server{…}}}` | 合规 |
| version | ipc | `{"ok":true,"data":{cli,driver,service}}` | 合规 |
| box list | ipc | `{"ok":true,"data":[{name,enabled,active_procs,file_root,…}]}` | 数组形态合规 |
| proc list | ipc | `{"ok":true,"data":[]}` | 空数组合规 |
| cfg get（--section） | ipc | `{"ok":true,"data":{section,setting,values}}` | 合规 |
| template check | **direct**（server op not implemented） | 数组形态合规 | tpl 三连降级路径实测正常 |
| log dump --last 3 | ipc | `{"ok":false,"error":{"code":4,"message":"log pump unavailable…"}}` | 错误信封合规；code 4 正确——**运行态注记**：实测机 SandMan.exe 在跑并持有会话 leader，server LOG_PUMP=no（server status 可见），按设计报 SERVER_UNAVAILABLE，非缺陷 |
| trace dump --last 3 | ipc | `{"ok":false,"error":{"code":5,"message":"trace buffer empty…"}}` | 空缓冲 NOT_FOUND 语义合规 |

**结论：信封零漂移**（10 条 transport 抽查：ipc×9、stub 降级×1）。

## 4. 上游动态（轴5）

- **用户合并的上游 PR**（d79424d，merge 于 2026-09-27）= upstream sandboxie-plus/
  master 的 3 个 commit：
  1. abd4c24：CHANGELOG 更新——**预告 1.18.6 / 5.73.6**（内容仅一条：修复 SandMan
     中 SBIE 消息链接在 URL 编码字符被当作消息占位符时损坏）；
  2. 25763f0：`SandMan.cpp` FormatSbieMessage——链接构建移到占位符替换**之后**并
     对链接做 HTML 转义（GUI 日志视图域；CLI 的 log watch 不构建超链接，无对应面）；
  3. fe6ed44：语言文件同步（任务边界：忽略）。
- **CLI 域功能变更：零**——未触及 `Sandboxie\`（core/driver/SbieSvc）、msgids、
  api_defs、Templates.ini、导出面。
- **维护注记（5.73.6 若发布）**：OSS 的驱动门是 **ABI 门**而非版本串门
  （`kExpectedAbi = 0x57230`，Util\Version.h:14；AbiMatches 只比 abi，
  DriverApi.cpp:614-617；版本串不匹配仅在 status/doctor 呈现 Warn）——上游
  CHANGELOG 未提 ABI 变更，预计 5.73.6 驱动可直接工作。无动作项。

## 5. 三轮合并视图与完整度

### 5.1 覆盖矩阵（06 + 07 + 08 累计，逐项重核）

**先修正 07 的记账**：07 §5.1 表内行值（如 A 域 25/32、D 域 6/8）停留在波次中
快照，其备注列又把所列缺口标注为"已收口"——行计数与备注互相矛盾；其 ≈91% 的
总评因此偏保守。按 07 的 125 项口径对**当前实现**逐项重核（波 A/B/D1/D2/D3
全部落地后）：键面域（B 36 + C 45）**全覆盖**（OnBoxDelete/OnFileRecovery/映像
链路经波 A/B/D2 收口，TemplateSettings 数据面走通用 cfg 路径）；动作/窗口/向导/
运维域的波次缺口（预设建箱、导出导入、复制、trace、装卸、挂起全局、伪命令、
恢复 move/check、doctor、浏览器模板）**全部已收口**。

**当前仍未覆盖的可数项（07 口径内）**：外壳集成（06 P2-8，已裁定**不做**）、
interactive 人工决策协议（06 P2-10，后续）——07 口径覆盖 **123/125 ≈ 98%**。

**08 三轴把三项此前不在 07 宇宙内的缺口显式入册**：

| + | 项 | 性质 | 规模 |
|---|---|---|---|
| +1 | `ExcludeFromTerminateAll` 语义（08-P1-1；键面存在、执行语义在 QSbieAPI 侧，07 功能面枚举漏计） | 语义正确性 | 微 |
| +1 | 模板安装检测 scan（06 P2-2；07 正文提为余量但未入 125 项槽位；量级 275 检测器，08-P2-3 固化） | 功能缺口 | 中 |
| +1 | log ≥3 插入串完整性（08-P1-2；质量项，非功能面） | 语义正确性 | 微 |

**合并口径：基数 128，覆盖 123 → ≈96%（区间 94–97）**。完成 08-P1×2 后
125/128 ≈ **98%**；完成 scan 与 iq 协议两个中规模波次后 ≈ **99%**；理论封顶
留 ~1 点永久不做（外壳集成）——07 报告的"97% 封顶/3% 永久"系行值未更新所致的
保守估计，本文重核后上修。

> 域归槽说明：07 表 A/D 域的逐槽记账存在上述内部矛盾，本文不逐槽复刻，改以
> "07 口径内缺口清单 + 08 新入册三项"的可复核方式给出聚合数；键面域（B/C）的
> 全覆盖结论经 08 轴3 执行者核验二次确认。

### 5.2 剩余清单（全量，三轮合并后仍未做的可做项）

| 项 | 出处 | 规模 | 性质 | 状态（波次 E 回填） |
|---|---|---|---|---|
| ExcludeFromTerminateAll 荣誉 + --no-exceptions | 08-P1-1 | 微 | 语义正确性 | **已收口（波 E）** |
| SbieDll_FormatMessage 数组变体绑定（log 文案） | 08-P1-2 | 微 | 语义正确性 | **已收口（波 E）**（实为契约补全，见 §2.2 回填修正） |
| box rules（QueryPathList） | 08-P2-1 | 小 | 观测增强 | 未做——波次 F |
| template folder 专用面 | 08-P2-2 | 小 | 可用性（数据面已通） | 未做——波次 F（SBIE_INI_TEMPLATE 处置见 §2.3 回填） |
| proc suspendBox/resumeBox op + snapshot default op | 08-P2-4/5 | 微×2 | 路径统一 | **已收口（波 E）** |
| 组级 --help | 08-P2-6 | 微 | UX | **已收口（波 E）** |
| Tmpl.Hide 过滤 | 08-P2-7 | 微 | 呈现对齐 | **已收口（波 E）** |
| tpl.list/info/check server op 补齐 | 06 §5 遗留 | 微×3 | 路径统一 | **已收口（波 E）**（Stub 退役，注册面零残留） |
| template scan（应用检测） | 06 P2-2（08-P2-3 量化） | 中 | 独立波次 | 未做 |
| interactive 人工决策 iq.ask/answer 协议 | 06 P2-10 | 中 | 独立波次 | 未做 |
| 永久 N-A：GUI 深水区（托盘/热键/弹窗交互/trace 栈符号化/外壳集成 P2-8/--drv-cache P2-11）、许可证域（ImBox 运行时已 CLI 化、更新器/addons） | 06/07 N-A 全表 | — | 不做 | 维持不做 |

> 波次 E 完成后完整度：08 §5.1 预估的 125/128 ≈ 98% 达成（P1×2 收口 +
> 路径统一/UX 微件不在 128 基数内）。剩余：box rules / template folder
> （波 F，+0 点基数）、scan / iq 协议（各自独立，+2 点 → ≈99%）、永久
> N-A ~1 点。

### 5.3 自审不一致修复清单

**文档级（本轮已直接修复，见 docs\04 变更）**：
1. §4 命令树：box 行补 `copy|export|import|types`；snapshot 行删幽灵动词 `info`。
2. §4.2 表：`--idle` → `--idle-timeout`，补 `--no-guardians`。
3. §4.3 表尾加注：波次 A/B/D 旗标（--no-triggers、--move/--no-check、--type 预设
   等）以 §16–§19 详表为准。

**代码级（只列不改；波次 E 回填：下列各项已全部收口，见 §2.2/§2.3 状态列
与 docs\04 §22）**：08-P1-1、08-P1-2、08-P2-4/5/6、tpl 三连 server op、
Tmpl.Hide（08-P2-7）；未做项仅剩 08-P2-1/2（波 F）与 scan/iq（独立波次）。

## 6. 结论与后续波次建议

1. **能力面三轴对照证实"绑定/消息/键面"三层无系统性遗漏**：SbieDll 137 导出中
   未绑定的 105 项里，除 2 项缺口（FormatMessage 数组变体、QueryPathList）外全部
   为沙箱内基础设施或已有等价路径；SbieSvc 105 MSGID 中未实现的 ~61 个全部无管理端
   语义；键面执行者核验新增确认 Linger/Leader 族执行者在**箱内 RpcSs**（修正了
   "可能需要 GUI/守护执行者"的潜在误判方向）。
2. **自审一致性呈"文档滞后、实现良好"格局**：信封零漂移、op 注册无孤儿、降级
   框架干净；文档修复三处已落地；剩余两处路径不一致（suspend-box/snapshot
   default）是 D3 自注待办，规模 微。
3. **建议波次**：
   - **波次 E（微件打包，半天级）**：08-P1-1 + 08-P1-2 + 08-P2-4/5/6 + tpl 三连
     op + Tmpl.Hide——全部微件，合计约 1.5 人日，完成后完整度 ≈98%。
   - **波次 F（观测增强）**：08-P2-1（box rules）+ 08-P2-2（template folder 面）。
   - **波次 G（维持 04 §21.3 排期）**：06 P2-2 scan（275 检测器）与 06 P2-10
     iq 协议，各自独立；两项完成后 ≈99%。
   - 上游跟随：零待办；5.73.6 发布后跑一次 doctor 确认 ABI 即可。

## 7. 复核指引（本文关键判定的证据锚点）

- 导出面：`Installer\SbiePlus_x64\SbieDll.dll`（= 本机安装目录同文件）PE 导出表
  137 项；`SbieCore\DriverApi\DriverApi.h:165-208`、`DriverApi.cpp:78-`、
  `DriverApi.cpp:520-530/537-575`（Ioctl 等价路径）。
- FormatMessage：core\dll\support.c:755-963（_2/数组变体/0/1/2 定参包装）；
  server\LogPump.cpp:129-156；cli\Commands\log_cmd.cpp:119-135；
  msgs\Sbie-English-1033.txt（%3×数十、%4×4）。
- 消息面：vendor\msgids.h 全文；SbieCore 内 MSGID 引用（24 个）；
  core\svc\sbieiniserver.cpp:1099-1176（TEMPLATE）、MountManager.cpp:472-485
  （IMBOX_UPDATE todo）。
- 键面：drv\conf_expand.c:246-288；drv\conf.c:1646-1656（TemplateSettings 特节）；
  apps\com\RpcSs\linger.c:256-332/447-557；core\dll\dllmain.c:974-991；
  drv\process_force.c；install\Templates.ini（[TemplateSettings] / Tmpl.Scan* 计数）。
- ExcludeFromTerminateAll：thirdparty\QSbieAPI\SbieAPI.cpp:1786-1792（vendored
  对照）；OSS 全树 grep 零引用；proc_cmd.cpp:285-320。
- 自审：Commands.cpp:570-606；ipcc\SbieIpc.h（61 kOp）；Dispatcher.cpp:2466-2560；
  IpcRoute.cpp:60-70；proc_d3.cpp:11-12/529-532；box_d3.cpp:543-600；
  server_cmd.cpp:17/56-69。
- 上游：git d79424d（merge）、fe6ed44/25763f0/abd4c24（upstream 三 commit）；
  Util\Version.h:14（ABI 门）。
