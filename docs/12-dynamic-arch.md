# 12 — 动态沙盒架构（dynamic-box-arch）

状态：**已实现（分支 dynamic-box-arch）** · 日期：2026-09-30
关联：`docs/10-v2-design.md`（V2 用户态体系）、`docs/02-driver-api.md`（既有驱动 API 面）

---

## 0. 修订：config-to-kernel 定稿（2026-09-30 第二波）

本节覆盖下文与之冲突的旧描述（旧文保留作演进记录）：

1. **驱动零内嵌默认值**。`Conf_InstallEmbeddedSkeleton()` 与
   `Conf_Skel_*` 静态表已删除——驱动不再编译任何默认 KV 表。
   `[TemplateDefaultPaths]` / `[TemplateNetworkPaths]` 两个骨架节
   **随盒走**：`API_BOX_CREATE` 的 blob 为多节 ini 形态，自带
   `[TemplateDefaultPaths]` + `[TemplateNetworkPaths]` + `[BoxConfig]`，
   内核解析器（`BoxDyn_LoadConfigText`）三节全写入 `Conf_Data`。
   未知 `[节名]` → `STATUS_INVALID_PARAMETER`（防注入任意节）。
   纯平铺 KV（无节头）仍合法 = 全部落盒节（旧单节形态）。
   模板节重装语义：删旧虚拟节再建（幂等替换）；真实 ini 提供的节
   不覆盖（ini 优先，同旧骨架语义）；节标记 `from_template`，
   节枚举继续隐藏。
2. **三模式标志物理删除**。`use_security_mode` / `use_privacy_mode` /
   `bAppCompartment` 字段及其全部分支已从驱动移除
   （process.h/process.c/token.c/thread_token.c/process_low.c/
   file.c/key.c/process_api.c/process_util.c/ipc.c）。
   行为全部由盒节纯键表达：
   - 安全模式 → `SysCallLockDown` / `RestrictDevices` / `DropAdminRights`
   - 隐私模式 → `WriteFilePath`/`WriteKeyPath`（盘根+注册表用户蜂巢
     影子）+ `Normal*Path` 白名单 + `UseRuleSpecificity=y`
   - 应用隔舱 → `OriginalToken` / `NoAddProcessToJob` /
     `NoSandboxieConsole` / `AlwaysCloseForBoxed=n` / `DontOpenForBoxed=n`
     / `ProtectHostImages=n` / `Disable{File,Key,Object}Filter=y` / COM 管道键
   - 旧 `#else`（无 USE_TEMPLATE_PATHS）硬编码路径块随之删除；
     `restrict_devices` 下盘设备 → normal 无条件执行，隐私影子由
     KV Write 键覆盖（写列表先于 normal 列表匹配，等长模式写胜）。
   - Dyndata 缺失回退（原强制 bAppCompartment）改为在
     OriginalToken / NoAddProcessToJob / NoSandboxieConsole 三个
     消费点 `|| !Dyndata_Active` 等效表达，字段清零与 MSG_1207 保留。
   - `SBIE_FLAG_PRIVACY_MODE` / `SBIE_FLAG_APP_COMPARTMENT` 对
     SbieDll 的导出改为旧键（`UsePrivacyMode` / `NoSecurityIsolation`）
     纯透传——预设不写这些键，故新体系下两 flag 恒不置位。
3. **API 权限门（三入口统一）**：`BoxDyn_CheckAccess()` ——
   提权管理员（UAC 感知：Administrators SID 须 ENABLED 且非
   deny-only，过滤令牌拒绝）或 SYSTEM（SbieSvc）；
   其余 `STATUS_ACCESS_DENIED`。沙箱内调用拒绝保留；
   EXEC/DESTROY 的"创建者或 SbieSvc"检查保留。
   TODO：SandboxieUsers 组授权（组 SID 查找机制待定）；
   deploy-rt.bat 已创建该本地组备用。
4. **预设四件套**：`sbie-cli/v3/examples/{standard,hardened,privacy,appc}.kv`
   —— 每个都是自完整多节 blob（骨架两节 + 盒节 + 行为键），
   内容从 `SandboxieOSS/Templates.ini` 对应节提取
   （privacy 含 186 条 DefaultFolder + PMod 白名单；appc 的骨架节
   去掉 LanmanRedirector/Mup/ImDiskCtl 三个网络关闭项）。
5. **SbieSvc 运行时依赖**：`sbie-rt exec` 起手
   `EnsureSbieSvcRunning()`——SbieSvc 已在跑则直通；否则 SCM
   `StartService` 优先，失败退 `CreateProcess(DETACHED)` 启动同目录
   SbieSvc.exe（注意：带 SCM 的系统上裸启 SbieSvc 会在
   StartServiceCtrlDispatcher 处退出）。部署见 `SandboxieOSS/deploy-rt.bat`
   （SbieDrv.sys 必须与 SbieDll.dll 同目录：驱动 home =
   SbieDrv.sys ImagePath 目录；SbieLow 内嵌于 SbieSvc.exe 资源
   LOWLEVEL64/32，无独立文件）。
6. `sbie-rt query TemplateDefaultPaths ...` 现在只在**至少一次
   create 之后**有值（骨架来自 blob，不再内核自举）。

---

## 1. 目标与不变量

**目标**：所有沙盒都是动态的——配置在运行时经 API 加载到内核，不再有静态 ini 沙盒。
运行环境（SbieRT）无任何内嵌 ini。

**不变量（用户架构规格，原样固化）**：

1. 内核配置只保留三个全局节：
   - `[TemplateDefaultPaths]`（进程启动必需的 IPC/文件路径骨架）
   - `[TemplateNetworkPaths]`（网络预设）
   - `[BoxConfig]` 前缀节（动态盒配置区——由 API 写入的临时配置区，
     实际节名为 `[BoxConfig_<id>]`）
2. API 三函数核心：
   ```c
   SBIE_BOX_HANDLE handle = SbieBoxCreate(configpath);  // 加载配置,生成实例和句柄
   SbieBoxExec(handle, exefile, arg1, arg2, ...);        // 在句柄沙盒中执行程序
   SbieBoxDestroy(handle);                               // 销毁沙盒实例
   ```
   进程退出 → 沙盒自动销毁（最后一个进程归零即 teardown）。
3. SbieRT 不读任何 ini 文件；骨架两节随盒 blob 走（见 §0 修订 1，
   原为"编译进驱动"，已删除）。

**边界**：V2 体系（SandboxieOSS）保留不动；本架构为并行新增，全部落在
`Sandboxie/core/drv/`（驱动可任意改）+ 一个新的最小测试工具 `sbie-cli/v3/`。

---

## 2. 总体形态

```
+-------------------+        IOCTL(API_SBIEDRV_CTLCODE)         +--------------------+
| sbie-rt (v3 CLI)  | ---------------------------------------> | SbieDrv.sys        |
| / SbieRT 宿主     |   API_BOX_CREATE / EXEC / DESTROY         |                    |
+-------------------+                                          |  api.c  (dispatch) |
       |  CreateProcessW(suspended)                            |    |                |
       |  (exec 前用户态先行建进程,模仿 SbieSvc                  |  box_dynamic.c     |
       |   RunSandboxedStartProcess 的既有流程)                 |   (句柄表/临时节    |
       |                                                       |    CRUD/自动销毁)  |
       v                                                       |    |                |
  target.exe (suspended) <-- SbieLow 注入 -- SbieSvc ---------->|  conf.c (Conf_Data |
       ^                        DriverAssist SVC_INJECT_PROCESS |    +Conf_Lock)     |
       |                                                       |  process.c (hook)  |
       + SbieDll: 以 proc->box->name(=临时节名) 查询配置 <-------+--------------------+
         —— 临时节名对 SbieDll/SbieSvc 完全透明,零改动
```

关键设计决策：**动态盒就是一个普通配置节**，节名即盒名。驱动全链路
（Box_CreateEx → Process_Create → Process_GetPaths/Conf_Get → SbieDll 查询）
都以"盒名 = 节名"工作，因此只要临时节存在于 `Conf_Data`，一切既有机制原样生效。
动态层只需要管理三件事：句柄↔节名映射、临时节生命周期、盒内进程计数。

---

## 3. 驱动侧新 API（三个 IOCTL）

### 3.1 API 码（`api_defs.h`，在既有枚举末尾追加，不重排）

```c
API_VERIFY,          // ... 既有最后一个 (0x1234004F)
API_BOX_CREATE,      // 0x12340050
API_BOX_EXEC,        // 0x12340051
API_BOX_DESTROY,     // 0x12340052
API_LAST
```

参数结构与既有 `API_ARGS_*` 宏风格一致（`API_NUM_ARGS=8` 不变）：

```c
// API_BOX_CREATE
API_ARGS_BEGIN(API_BOX_CREATE_ARGS)
API_ARGS_FIELD(UNICODE_STRING64 *, config_text) // in : KV 对流 UTF-16 文本
API_ARGS_FIELD(ULONG64 *, box_handle)           // out: 句柄
API_ARGS_FIELD(WCHAR *, box_name)               // out: WCHAR[BOXNAME_COUNT] 节名
API_ARGS_CLOSE(API_BOX_CREATE_ARGS)

// API_BOX_EXEC
API_ARGS_BEGIN(API_BOX_EXEC_ARGS)
API_ARGS_FIELD(ULONG64, box_handle)             // in : 句柄
API_ARGS_FIELD(HANDLE, process_id)              // in : 调用方预创建(suspended)的 pid
API_ARGS_FIELD(BOOLEAN, fake_admin)             // in : 授予伪管理员标志
API_ARGS_FIELD(ULONG64 *, out_process_id)       // out: 回显确认的 pid(可空)
API_ARGS_CLOSE(API_BOX_EXEC_ARGS)

// API_BOX_DESTROY
API_ARGS_BEGIN(API_BOX_DESTROY_ARGS)
API_ARGS_FIELD(ULONG64, box_handle)             // in : 句柄
API_ARGS_FIELD(ULONG, kill_processes)           // in : 非 0 = 先终止盒内全部进程
API_ARGS_CLOSE(API_BOX_DESTROY_ARGS)
```

### 3.2 `SbieBoxCreate`（API_BOX_CREATE / `Api_BoxCreate`）

输入配置 blob 为 KV 对流（UTF-16、`\r\n`/`\n` 行分隔，`#` 注释，
`Key=Value`，同名键多次出现按列表追加——等价 ini 语义的最小子集）。

流程：
1. 调用方在沙盒内（`proc != NULL`）→ `STATUS_ACCESS_DENIED`。
2. `Api_CopyStringFromUser` 拷入配置文本；解析逐行 KV。
3. 生成唯一 id：64 位单调计数器（`InterlockedIncrement64`），
   节名 `BoxConfig_<id>`（十进制，≤30 字符，满足 `Box_IsValidName`
   的 `[0-9A-Za-z_]` ≤38 字符约束）。
4. `Conf_CreateTempSection("BoxConfig_<id>")` 在 `Conf_Data` 建节
   （`is_virtual=TRUE`，独占 `Conf_Lock`），逐条 `Conf_AddTempSetting`。
5. 隐式注入 `Enabled=y`（若 blob 未自带）——使 `Conf_IsBoxEnabled`
   与 SbieSvc 侧既有校验通过。
6. 建句柄表项：`{handle=id, section, creator_pid, sid, session_id, proc_count=0}`。
   SID/Session 在 CREATE 时从当前进程令牌捕获并固化到条目，
   保证同一动态盒内所有进程共享一致的盒身份（`Box_InitKeys` 需要它）。
7. 返回句柄 + 节名（`FileRootPath` 等路径键就在 blob 里指定）。

### 3.3 `SbieBoxExec`（API_BOX_EXEC / `Api_BoxExec`）

与 `Process_Api_Start`（API_START_PROCESS，SbieSvc 专用）同构，
差异仅在盒的来源与权限模型：

1. 沙盒内调用 → 拒绝；句柄查表无果 → `STATUS_INVALID_HANDLE`；
   调用方既非创建者进程也非 SbieSvc → `STATUS_ACCESS_DENIED`。
2. `Process_ReadyToSandbox` 未置位 → `STATUS_SERVER_DISABLED`。
3. `Box_CreateEx(Driver_Pool, entry->section, entry->sid, entry->session_id, TRUE)`
   —— 走既有 `Box_InitPaths`（读临时节里的 FileRootPath/KeyRootPath/
   IpcRootPath，无则用默认值），`Conf_IsBoxEnabled` 不再查（CREATE 已
   强制 Enabled=y）。
4. `PsLookupProcessByProcessId` 校验目标 pid 存在且会话匹配
   （与 `Process_Api_Start` 相同的 `STATUS_LOGON_SESSION_COLLISION` 检查）。
5. `Process_NotifyProcess_Create(pid, Api_ServiceProcessId, 当前pid, NULL, 0, box)`
   —— 复用既有注入链：`Process_Create` → `Process_Low_Inject` →
   `SVC_INJECT_PROCESS` → SbieSvc DriverAssist 注入 SbieLow →
   SbieDll 以节名自举。**因此 EXEC 依赖 SbieSvc（或等价注入服务）在位**，
   这是既有架构的固有依赖（见 §8 遗留）。
6. 成功后 `proc_count++`。

为什么 EXEC 收 pid 而不是 exefile：内核创建用户态进程不现实；
Sandboxie 既有路径（SbieSvc `RunSandboxedStartProcess`）就是
"调用方 CreateProcess(suspended) → 把 pid 交给驱动认领"。EXEC 沿用该契约，
调用方（sbie-rt/SbieRT）在 `CREATE_SUSPENDED` 下建进程，IOCTL 成功后
`ResumeThread`，失败则 `TerminateProcess`。

### 3.4 `SbieBoxDestroy`（API_BOX_DESTROY / `Api_BoxDestroy`）

1. 权限同 EXEC（创建者或 SbieSvc）。
2. `kill_processes != 0`：先枚举 `Process_Map` 中 `box->name == 节名`
   的 pid（共享 `Process_ListLock` 下收集到栈缓冲），再逐个
   `ZwTerminateProcess`（复用 `Process_Api_Kill` 的清
   BreakOnTermination + 终止手法），循环直至盒清空。
3. `Conf_DeleteTempSection(节名)`（仅删 `is_virtual` 节，安全护栏），
   移除句柄表项。
4. 幂等：重复 destroy 同句柄 → `STATUS_INVALID_HANDLE`。

### 3.5 自动销毁（进程归零即 teardown）

三个钩子（`process.c` 调 `box_dynamic.c`）：

- **进程入盒计数**：`Process_Create()` 在 `Box_Clone` 成功后调
  `BoxDynamic_OnProcessCreate(box_name)`——**每个**进入动态盒的进程都计数
  （EXEC 认领的根进程、盒内派生的子进程、forced 进程一视同仁），
  这样"最后一个进程退出"才是真正的最后一个。
- **盒内进程退出**：`Process_Delete()` 在 `Pool_Delete(proc->pool)` 前
  调 `BoxDynamic_OnProcessDelete(proc)`——节名匹配 → `proc_count--`，
  归零即 teardown（删节 + 删表项）。计数在 `Process_Delete` 而非
  notify 回调里递减，保证与进程结构生命周期一致。
- **创建者退出**：`Process_NotifyProcess_Delete()` 对**每个**退出进程
  调 `BoxDynamic_OnAnyProcessExit(pid)`——pid 命中某条目 creator_pid
  → 该盒整体 destroy（杀进程 + teardown）。这覆盖
  "句柄持有者死亡未显式 destroy" 的泄漏路径。

锁序（单向，无反转）：`Process_ListLock → BoxDyn_Lock → Conf_Lock`，
且三者从不同持（每个 box_dynamic 函数都在取下一把锁前释放上一把；
唯一的嵌套是 Process_Create 钩子在已持 Process_ListLock 时取
BoxDyn_Lock）。teardown 删节发生在进程通知回调上下文（PASSIVE_LEVEL），
`Conf_Lock` 独占获取合法。`Conf_Read`（reload）对 `is_virtual` 节有
既有保号逻辑，动态节在配置重载后存活。

---

## 4. SbieDll / SbieSvc：零改动论证

- **SbieDll**：`Dll_Init` 起手 `SbieApi_QueryProcess(self)` 拿到的是
  `proc->box->name` = 临时节名；之后所有 `SbieApi_QueryConf`/路径查询
  都以该名字为节名。临时节名对 SbieDll 完全透明，无需任何修改。
- **SbieSvc**：`RunSandboxed` 消息面收 boxname → 建进程 →
  `API_START_PROCESS`。把 `BoxConfig_N` 当 boxname 传入即可工作
  （该 API 走 `Box_CreateEx`+`Conf_IsBoxEnabled`，节里有 Enabled=y）。
  新增 BOX_CREATE/DESTROY 的服务消息**不需要**——CLI/宿主直连 IOCTL 更简
  （与任务书判断一致）。

## 5. 无 ini 化：骨架随盒（`box_dynamic.c`，原"骨架嵌入"方案已废弃）

（历史方案——静态表编译进驱动 + `Conf_InstallEmbeddedSkeleton`——
已被 §0 修订 1 的 config-to-kernel 模型取代并从代码中删除。
现由 `API_BOX_CREATE` 的多节 blob 直接写入
`[TemplateDefaultPaths]` / `[TemplateNetworkPaths]` 虚拟节，
替换语义与 ini 优先规则见 §0。）

## 6. 文件清单

| 文件 | 性质 | 内容 |
|---|---|---|
| `Sandboxie/core/drv/box_dynamic.c` | 新增 | 动态盒管理：句柄表、三 API handler、自动销毁钩子 |
| `Sandboxie/core/drv/box_dynamic.h` | 新增 | 对外接口（process.c/driver.c 挂钩用） |
| `Sandboxie/core/drv/api_defs.h` | 修改 | 枚举末尾 +3 API 码；+3 参数结构 |
| `Sandboxie/core/drv/conf.c` | 修改 | `Conf_CreateTempSection/AddTempSetting/DeleteTempSection/HasSection/MarkSectionTemplate`（嵌入式骨架表已删，见 §0） |
| `Sandboxie/core/drv/conf.h` | 修改 | 上述导出声明 |
| `Sandboxie/core/drv/process.c` | 修改 | `Process_Delete`/`Process_NotifyProcess_Delete` 各 +1 行钩子 |
| `Sandboxie/core/drv/driver.c` | 修改 | `DriverEntry` + `BoxDynamic_Init()`；卸载 + `BoxDynamic_Unload()` |
| `Sandboxie/core/drv/SboxDrv.vcxproj`(+filters) | 修改 | 注册 box_dynamic.c/h |
| `SandboxieOSS/sbie-cli/v3/sbie-rt.cpp` | 新增 | 最小测试工具（create/exec/destroy/status，直调 IOCTL） |
| `SandboxieOSS/sbie-cli/v3/sbie-rt.vcxproj` | 新增 | 单文件 VS 工程（无 SbieDll 依赖） |
| `SandboxieOSS/SandboxieOSS.sln` | 修改 | 挂入 sbie-rt 工程 |
| 本文档 | 新增 | 架构定稿 |

## 7. 安全模型（§0 修订 3 生效）

| API | 授权 |
|---|---|
| BOX_CREATE | 非沙盒进程 + 提权管理员（UAC 感知）/ SYSTEM |
| BOX_EXEC / BOX_DESTROY | 同上，且创建者进程 或 SbieSvc（`Api_ServiceProcessId`） |

生产化前建议（未做，见遗留）：SandboxieUsers 组授权；
句柄加 nonce 混淆；blob 大小上限已在实现中（64KB）。

## 8. 遗留 / 已知边界

1. **EXEC 依赖 SbieSvc 注入**（`SVC_INJECT_PROCESS`→SbieLow）。ValidationOS
   上 SbieRT 必须含 SbieSvc（或提供等价 DriverAssist 端口）。内核自建进程
   不在本次范围。
2. **空盒不自动销毁**：CREATE 后从未 EXEC 的盒，仅在创建者退出或显式
   DESTROY 时清理（创建者退出钩子已覆盖泄漏）。
3. 骨架表是**冻结快照**，Templates.ini 后续演进需人工同步（可接受：
   SbieRT 场景下 Templates.ini 已不存在）。
4. 驱动为测试签发（test-signing /.ValidationOS 部署），未走正式签名链。
5. `BoxConfig_` 前缀节对 `Conf_Api_Update`（API_UPDATE_CONF）未设防——
   服务侧可改写动态节；如需隔离可在 `Conf_Api_Update` 加前缀白名单。

---

## 9. 实机验证（ValidationOS 部署与测试序列）

部署（ValidationOS，测试签发，不覆盖生产驱动路径；一键staging：
开发机跑 `SandboxieOSS\deploy-rt.bat` 产 `dist\sbie-rt\`，整目录拷去）：

```bat
:: 1) 构建产物（= deploy-rt.bat 的 staging 目录内容）
::    Sandboxie\Bin\x64\SbieRelease\SbieDrv.sys          （含动态盒 API）
::    SandboxieOSS\x64\Release\sbie-rt.exe               （测试工具）
::    + SbieSvc.exe/SbieDll.dll/32\对、KmdUtil、服务 stub、VC 运行库
::      （EXEC 注入依赖见 §8.1；SbieLow 内嵌于 SbieSvc.exe 资源）

:: 2) 目标机启用测试签名后加载驱动
bcdedit /set testsigning on
sc create SbieDrv type= kernel binPath= "C:\SbieRT\SbieDrv.sys"
sc start SbieDrv

:: 3) 无 ini 启动验证（注意：骨架两节随盒 blob，create 后才可查）
sbie-rt drv
sbie-rt create C:\SbieRT\examples\standard.kv
sbie-rt query TemplateDefaultPaths OpenIpcPath   -> \Windows\ApiPort
sbie-rt query TemplateNetworkPaths OpenFilePath  -> \Device\NamedPipe\ROUTER
sbie-rt destroy <handle>
```

动态盒生命周期测试：

```bat
:: create: 配置进内核，拿句柄（预设：standard/hardened/privacy/appc.kv）
sbie-rt create C:\SbieRT\examples\dynamic-box.kv
::   -> handle 0x1 / box BoxConfig_1
sbie-rt query BoxConfig_1 FileRootPath           -> 展开后的路径
sbie-rt query BoxConfig_1 Enabled                -> y

:: exec: 认领挂起进程 -> 注入 -> 恢复 -> 等待退出
sbie-rt exec 0x1 C:\Windows\System32\cmd.exe /c echo in-dynamic-box
::   -> exit code 0

:: 自动销毁验证：exec 一个长驻进程（如 notepad），关掉它后
::   DbgPrint 流（DbgView）应出现 "last process gone, auto destroy"

:: 显式销毁（含杀进程）
sbie-rt destroy 0x1
sbie-rt query BoxConfig_1 FileRootPath           -> <unset>（节已删）

:: 创建者退出联动：create 后直接退出 sbie-rt 会话（或 killall 创建者），
::   盒应随之销毁（"creator ... exited, destroying"）
```

开发机（生产驱动在位）已完成的验证（2026-09-30）：

- `sbie-rt drv`：老驱动正确报告 `dynamic-box API: ABSENT`（探测协议工作）；
- `sbie-rt query`：对生产驱动的 `TemplateDefaultPaths`/`TemplateNetworkPaths`
  实读成功（IOCTL 管道、UNICODE_STRING64 读写正确）；
- `sbie-rt create` 对老驱动返回 `0xC0000010`（未注册 API 码的预期行为）；
- 驱动整体编译零告警（`/W4`，SbieRelease x64，WDK 26100）；
- 用户态单元测试 `v3\test_conf_crud.exe`：23/23 PASS（KV 语法 10 +
  临时节 CRUD/重载保号 13）。

第二波（config-to-kernel 定稿，2026-09-30）追加验证：

- 驱动 `/t:Rebuild` 全量重建绿；唯一 warning 为 msgs\Parse 工具链的
  D9025 命令行宏覆盖（既有、非驱动源码）——驱动源码零 C4xxx；
- grep 零残留：`use_security_mode` / `use_privacy_mode` /
  `bAppCompartment` / `Conf_InstallEmbeddedSkeleton` / `Conf_Skel_*`
  在 `core/drv/*.{c,h}` 全灭；
- `sbie-rt create`（新构建）对老驱动：五个 kv
  （standard/hardened/privacy/appc/dynamic-box）全部按预期
  `0xC0000010` 拒绝——文件读取、编码转换、IOCTL 通路正确；
- kv 静态校验：节名白名单全合规、每行 `Key=非空值`、全 ASCII、
  体积 ≤ 21110 字节（uni.Length uint16 上限 65535 字节 = 32767 字符内）；
- `deploy-rt.bat` staging 成功：22 文件（SbieDrv.sys 与 SbieDll.dll
  同目录、32\ 对、examples\5 kv、MANIFEST.txt），本地组 SandboxieUsers
  已创建。

遗留：SbieDrv.sys 含动态 API 的实机加载与 exec 全链路（注入→SbieDll
自举→退出自动销毁）待 ValidationOS VM 部署执行（本机生产驱动不可替换）。
