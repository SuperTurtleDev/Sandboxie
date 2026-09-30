# 10 — V2 设计：无 server 的 CLI 直驱架构（模板/注册/锁/monitor/命令面）

状态：设计冻结稿（待用户确认 §13 OPEN 决策点后进入实现波次）。

> **实现状态（2026-09-27 收官）**：V2 已按本文实现并端到端实测通过
> （`build_oss.bat` /W4 /WX 0 警告；exec/ps/kill/kill-box 归零自动注销/
> register/sync-config 全链真机验证；V1 命令面与 server 已删除）。
> 代码落点：`SbieCore\Model\V2\{V2Common,V2Template,V2Cache,V2Registry,V2Task}`、
> `sbie-cli\monitor\MonitorMain`、`sbie-cli\cli\{V2Commands,TemplateMigrate}`。
> V2 模板树（400 文件，迁移自 install\Templates.ini）在 `SandboxieOSS\templates\`。
> 实测补充决策：exec 子进程启动必须**继承调用方完整环境块**（空 env 块会令
> 盒内进程在 SbieDll 初始化期早夭）；exec 前台默认等待子进程并透传退出码
> （`--detach` 不等）；命令行单参数含空格 = 整行透传，多参数 = 逐 token 拼接。
>
> **实现状态附录 2（2026-09-28 部署层+竞态修复收官）**：
> 1. 命令面追加 `kill-box`（SvcClient KillAll）/ `kill <PID>`（KillOne，PID
>    须属已注册沙盒）/ `log`（dmesg 风格：`-w --last --type --box --pid
>    --json`；自任会话 leader 读驱动日志环，他人持 leader 时报错并指明占用
>    pid——leader 无反注册 API，不可抢占）。
> 2. 实测定位并修复的竞态：
>    - 盒进程启动的会话 leader 依赖（MonitorMain.cpp）：SbieDll init ->
>      epmapper 链要求本会话存在活 leader；OSS dist 无 SandMan -> 公共
>      monitor 启动即获取领导权（他人持有时降级纯轮询），并以
>      Local\SbieOSS_MonitorReady_S<sid> 就绪事件让 exec 等领导权就绪
>      而非仅互斥体。
>    - exec 顺序竞态（V2Commands.cpp CmdExec）：原实现先 spawn 后拉
>      monitor——首启盒落在 leader 真空窗即夭折；改为任务文件 -> monitor
>      就绪 -> RunSandboxed。
>    - 上一代盒死亡窗口直启竞态（CmdExec S3/S4 分支）：盒内用户进程全灭
>      后 SandboxieRpcSs/DcomLaunch 仍存活数秒，"锁在+注册在"曾被误判为
>      可直启；现判据排除两个自举服务镜像名，濒死窗口改为循环重探 ->
>      teardown 摘锁后走 S0。12 连发快速 exec 实测全 7（修复前 7/4 交替）。
>    - 注销结算窗口竞态（MonitorMain.cpp TeardownBox + CmdExec
>      AwaitTeardownSettle）：盒注销后约 2-6s 低级注入结算期内重新 spawn
>      会静默夭折（子进程退出码 4/127，驱动日志或见 SBIE2181）；teardown
>      写 <box>.dead 墓碑，S0 路径读到 8s 内墓碑即睡满结算窗口。
>    - S1 分支加 TaskExists 守卫（teardown 摘锁与删缓存之间的窗口不当接管）。
> 3. 部署层定论：五个沙箱内模拟服务全带（SCM 硬编码重定向非模板门控，
>    scm.c:1064-1109 / scm_create.c:972-1009）；安装目录 Templates.ini
>    保留完整原文件（最小 stub 在冻结 5.73.5 运行时上实测令进程启动确定性
>    失败，A/B/A 两轮证据，docs/05 §8.2 修订）；V2 模板树 400 文件随 dist
>    （templates\ = SBIE_TEMPLATE_DIR 初始内容 + exe 目录兜底根）。>
> **实现状态附录 3（2026-09-28 用户实测反馈修复）**：
> 1. **注册超时根因**：主 ini 无 ImportBox 通配行（部署前提从未自愈）→
>    register 放文件永远不可见。修复：`EnsureImportBoxLine()`
>    （V2Registry.cpp）register 前自动经 SbieSvc ADD_SETTING 幂等部署
>    `<AppData>\SandboxieOSSoxes\` 行（refresh=true；EditPassword 设置
>    时失败并给手工指引）；主 ini 位于 Home/\SystemRoot/IniPath 三层任何
>    位置均自动适配（服务侧解析活动 ini 路径，CLI 不感知位置）。
> 2. **幽灵节 heal**：上次注册失败残留的缓存文件会在后续 reload 被导入，
>    或缓存已删但驱动内存留有旧节——(a) RegisterBox 超时路径现在**回滚
>    删除自己的缓存**（不再留残骸）；(b) exec 状态机 S6 分支（无锁有注册
>    无缓存）首次进入触发一次 ReloadConf heal（目录=注册集合真理源，
>    重扫即散幽灵）。
> 3. **同名碰撞检查换源**：驱动内存侧 Conf 查询（易见幽灵）→ 缓存文件
>    FileRootPath（CacheBelongsToOtherDir；文件名=盒名=节名唯一）。
> 4. **相对路径矩阵**（./ .\ . ../..\ ../../ ./.././ .\./..\../xxx 及
>    裸相对名）：统一经 NormalizeDirPath 的 GetFullPathNameW 规范化
>    （fwd/back 斜杠、. .. 混合皆可），目录不存在/无 sandbox.ini 给
>    NOT_FOUND 明确文案。
> 5. **盒内 exec**（`exec ./ cmd.exe` 从沙盒内）：盒内 cwd 为虚拟路径、
>    宿主 sandbox.ini 不可见、ReloadConf 拒绝盒内调用者——决策：盒内
>    exec 走"自身盒"快速路径（驱动 QueryProcess(self) 反推盒名 +
>    RunSandboxed 的 CallerInSandbox 同盒语义，ProcessServer.cpp），不做
>    宿主解析/注册；锁/任务/monitor 归盒外首个 exec。CLI 总门放行
>    exec 进沙盒（其余命令仍拒绝）。>
> **附录 3 补记（用户纠正：ini 定位零用户态探测，IOCTL/SbieSvc 语义）**：
> 主 ini 生效位置由内核三层搜索决定（conf.c:256-269：IniPath 注册表值
> → Home → \SystemRoot），用户态不重新实现探测。ImportBox= 只能来自主
> ini 的 [GlobalSettings]（Conf_Import_AllIncludes 仅读 Conf_Data 该节，
> conf.c:1121-1153；IniPath 重定向只是换位置读"主 ini"本身，不提供第二
> 来源）——**路径 b（绕开主 ini）查证不可行，路径 a（SbieSvc SBIE_INI
> 写通道）为唯一正路**，EnsureImportBoxLine 已按此实现并带打点链：
> SvcClient 写行返回（含 StatusName）→ 缓存文件在约定目录 →
> SbieApi_ReloadConf IOCTL 返回码（NtStatusText）→ 驱动 QueryConf
> 可见性探测（超时则回滚缓存并输出 ImportBox 行 present/MISSING 分解）。>
> **附录 4（2026-09-28 合并修复清单实施，对照 docs/11 四 blocker + 四需求）**：
> - **B1**：归零判定统一"用户进程"口径（BoxUserProcessCount，排除五个自举
>   服务镜像——MonitorMain 与 exec S3/S4 同源判据，消灭双判据死锁）；
>   上游 linger.c 事件驱动自杀漏事件的滞留服务由 monitor 侧宽限
>   （serviceGraceMs=15s）后 KillAll 清场兜底。自测 10/10（kill 用户进程
>   后 2s 内自动注销）。
> - **B2**：monitor 心跳状态文件（monitors\monitor.status，每 tick 原子
>   重写）；健康判定=互斥体∧心跳新鲜(<3s)；陈旧→按 status pid 击杀挂起
>   实例→重拉。自测：人为老化 mtime → 旧 pid 68408 被杀、新 pid 130480
>   接管、exec 正常。
> - **B3**：WriteTextFileAtomic tmp 名唯一化（.pid.tick.tmp）；exec S0 放锁
>   失败不再 Fail——落回循环按 S2/S3 语义重探。自测：teardown 窗口 5 并发
>   ×3 轮 = 5/5、5/5、5/5（修复前 1-2/5）。
> - **B4**：msgid 剥 SBIE 打包位（真值=低16位，0x41020577→1399）；文案经
>   SbieDll_FormatMessage 数组变体（ins[1..5]=条目字符串；FormatMessage0
>   恒落 err=… 兜底——support.c:882-906 实读），兜底签名时自拼接
>   "SBIE<id> | s1 | s2…"。--type/--box 过滤随之恢复。注意：该导出无
>   free，逐行一次小额分配不回收（dump 量级可忽略，-w 长跑备案）。
> - **R1**：exec 默认 spawn 成功即退 rc=0（实测 0.33s）；--wait 等待并
>   透传（42→42/7→7/3→3/5→5）；--detach=历史别名。原前置墓碑等待删除，
>   改为 spawn 后 300ms 探活的**反应式自愈**（子进程死于注入结算窗口→
>   4s 后重 spawn 一次）——守卫不再阻塞 exec 返回。
> - **R2/R3**：--password 旗标 > SBIE_PASS 环境 > 自动交互（无参数，用户
>   拍板）：ACCESS_DENIED 且未供密码时 tty 提示（回显关闭）重试一次；
>   非 tty 直接失败并指引。实测：错密码 rc=6 / --password rc=0 /
>   SBIE_PASS rc=0 / 非 tty 无密码=完整指引信封（tty 分支待人工）。
>   新增内部工具 --set-password / --ini-del（经 SbieSvc 官方通道）。
> - **R4**：sbie-cli info = CLI/驱动(版本/abi/alive)/SbieSvc(版本/连接)/
>   主 ini 生效路径（IniGetPath，零用户态探测）/ImportBox 行状态/
>   monitor 心跳/注册盒表（别名/路径/用户进程数/锁/任务），表格+--json。
> - 旗标整理：全局 --wait（exec 布尔）与 --settle <sec>（状态机收敛超时）
>   拆分，消除同名互斥。>
> **附录 5（2026-09-28 复测 major N1/N2）**：
> - **N1（净 ini 首次 exec 必败）已修**：EnsureImportBoxLine 写行后新增
>   校验回读（驱动 QueryConf 枚举 GlobalSettings\ImportBox 直至目录在
>   列；退避 50→200ms + 至多 8 次驱动侧 ReloadConf 兜底，上限 10s），
>   可见后才进缓存写入/注册。净 ini（文件+驱动同步无行）首轮 exec 自测
>   5/5（删行→unregister 同步→exec：行自动重部署 + rc 精确透传）。
> - **N2（log -w 投递死点）定位出三层真身并修其二**：
>   1. **USHORT 截断（根因）**：SbieApi_GetMessage 的 DLL 包装把 Length
>      截为 USHORT（sbieapi.c:312）——缓冲 65536 字节截断为 0 ⇒ 一切
>      条目 BUFFER_TOO_SMALL ⇒ 永无投递。修：缓冲 32760 WCHAR
>      （65520B < 65536）。
>   2. **巨型条目（>64KB 单串，环内实测约 240 条存量）**：驱动返回
>      TOO_SMALL 但仍推进游标（api.c:807）——旧实现把 rc!=0 一律当
>      "空"且 150ms 睡眠 ⇒ 每条 150ms 爬行。修：FetchOne 三态
>      （Delivered/Empty/TooSmall），TooSmall 立即重试不睡眠。
>   3. **-w 历史回放**：-w 先前执行完整 dump（含巨型条跳过与整环渲染）
>      耗时不可控 ⇒ follow 迟迟不启动。修：-w 纯 tail 语义（跳过 dump，
>      有界 5s 快进到环头后跟随）。
>   **残留（未关闭，如实记录）**： follower 在环头等待时，新生成的
>   session-1 事件（事后 dump 证实已入环，序号连续）仍不被投递——
>   疑 get_next 游标语义在"等待中新增"场景的内核侧行为，冻结驱动无法
>   追踪。dump 路径（B4 验收面）完全正常。-w 复测 0/5，交测试 agent
>   以本节取证复核；如确认仍死，建议后续用 SbieSvc 会话（session_id=-1
>   通道）做日志中继的替代方案。>
> **附录 6（2026-09-28 加密盒 + Basic.ini 分离 + BoxTypes 六型）**：
> - **create-encbox / UseFileImage=y**：布局 = sandbox.ini(明文) +
>   data(盒根 junction 目标) + data.box(容器)。关键决策——服务端镜像名约定
>   file_root+".box"（MountManager.cpp GetImageFileName），故取
>   FileRootPath=<dir>\data 使容器恰为 <dir>\data.box（满足用户布局）。
>   创建 = IMBOX_CREATE（SbieSvc 挂起 ImBox.exe 建卷+格式化+卸载，
>   wire image_size 单位 KB）；exec 前置 IMBOX_MOUNT(regRoot=KeyRootPath,
>   autoUnmount=true)——冻结链路 DriverAssistInject AcquireBoxRoot 只复用
>   已有挂载（其自动挂载不带密码：BoxPassword 查询在 5.73.5 注释掉，
>   MountManager.cpp:1147）；盒终止 DriverAssist:841 AutoUnmount 自动摘
>   除，monitor teardown 幂例 unmount 兜底。密码链（用户拍板统一）：
>   --mount-password(exec)/--password(create-encbox) > SBIE_BOX_PASSWORD
>   环境变量 > R3 tty 交互（非 tty 报错指引）；与 ini EditPassword 的
>   --password/SBIE_PASS 平行两套。ImBox 函数组自 845f92c git 找回
>   （评审 pass 曾误清）。
> - **Basic.ini 分离**：官方 [TemplateSettings] 33 个 Tmpl.X 全量迁
>   templates\Basic.ini；V2 展开器变量优先级 = 内置表 → Basic.ini
>   （各模板根扫描序第一个命中）→ 盒 sandbox.ini [TemplateVars]。
>   **仓库 Templates.ini 精简**（决策）：删 [TemplateSettings] + 400 个
>   已迁移节；保留 [DefaultTemplates]（内核给 V1 遗留盒的全局合并源）
>   + 37 个未迁死壳（401 节删，445→45 行数级瘦身）。安装目录/布局的
>   Templates.ini 仍随 dist 全文件分发（冻结运行时实证依赖，
>   docs/05 §8.2）——仓库这份仅作残余/兼容源与迁移对照。
> - **BoxTypes 六型**：Standard(既有) + Hardened/HardenedPlus/
>   StandardPlus/AppBox/AppBoxPlus（键集对齐 docs/04 §12：UseSecurityMode/
>   UsePrivacyMode/NoSecurityIsolation + Template=Misc\RpcPortBindingsExt；
>   均嵌套引用 BoxTypes\Standard 作基础集 = "Template=Basic 引用基础集"
>   的落地形态）。新命令 create-box --type 六值 + create-encbox。>
> **附录 7（2026-09-28 加密盒波次收官 + exec 旗标泄漏根因钉死）**：
> - **根因钉死（用户纠偏后的完整定位）**：加密盒 exec 全链
>   "RunSandboxed GENERIC (win32 2)" 的真凶 = **exec 自身旗标泄漏**：
>   --mount-password 抽取块位于 rest 收集之后——旗标与密码漏进命令行，
>   服务端 CreateProcessAsUser 尝试执行名为 "--mount-password" 的可执行
>   文件 → ERROR_FILE_NOT_FOUND(2)（[44/2] 日志形态）。逐步复刻二分
>   （--raw-run2 系列诊断，已移除）+ SvcClient 入参打点实锤：exec 的
>   cmd=[--mount-password pw123 "cmd /c exit 3"] vs 手工 cmd=[cmd /c
>   exit 0]。修复 = 抽取前置到 rest 收集之前。
>   全链 E2E 通过：exec（--mount-password）→ 盒内写文件（junction 读回
>   "final123"）→ 进程退出 → 自动 teardown+unmount（junction 消失、
>   ps 清空）→ data.box 头 2KB 熵 256/256。密码三路径：--mount-password
>   ✓ / SBIE_BOX_PASSWORD ✓ / 非 tty 无密码=明确报错 ✓（tty 待人工）。
> - **密码链统一（用户补充）**：--mount-password(exec) / --password
>   (create-encbox) > SBIE_BOX_PASSWORD 环境变量 > R3 tty 交互；与
>   ini EditPassword 的 --password/SBIE_PASS 平行两套凭据体系。
> - **附带修复**：--wait 快死子进程退出码丢失（earlyExit 直接透传，
>   规避句柄重开 TOCTOU）；自检期望根泛化为 sandbox.ini 显式
>   FileRootPath（任意盒可声明子目录根，非加密盒专属）。
> - 附带加固：exec 濒死窗口自愈（monitor 被外杀后残留 task/锁不再死等
>   STATE_TIMEOUT——首次进入即 EnsureMonitorRunning 收编 stale task）。
前置阅读：`00-architecture.md`（V1 server 模型，V2 将其废除）、`02-driver-api.md`、
`03-svc-protocol.md`（仅 proc-start 一项保留 SbieSvc）。
代码锚点基准：`Sandboxie\core\drv\{conf.c, box.c, api.c, session.c, process.c, conf_expand.c}`、
`Sandboxie\common\{map.c, map.h, defines.h}`、`Sandboxie\core\svc\sbieiniserver.cpp`、
`Sandboxie\install\Templates.ini`（437 节全量盘点，见 §11）。

已由用户拍板并固化的方向（本文按此展开，不再列为权衡项）：

| # | 拍板内容 |
|---|---|
| D1 | monitor 纯轮询，不做驱动日志过滤/leader 仲裁 |
| D2 | 注册走 ImportBox 目录通配 + 文件放删 + `SbieApi_ReloadConf` IOCTL 直调，运行时零主 ini 写入 |
| D3 | 运行缓存零 `Template=` 残留（生成后自检，硬性校验项） |
| D4 | V1 命令面整体删除，V2 命令面是唯一面（不留 v1 子命令/编译开关） |
| D5 | GUI 影响降级为一句话备注（后续波次适配） |
| D6 | monitor 为公共单进程多任务模型（每登录会话一实例，任务文件约定通信） |

---

## 1. 目标与原则（KISS）

1. **摆脱 sbie 历史包袱**：CLI 命令面收敛为 5 个用户命令 + 1 个内部入口
   （`exec / register / unregister / sync-config / ps` + `--monitor`）。
2. **用户态模板体系**：`SBIE_TEMPLATE_DIR` 环境变量驱动的目录化模板，
   `类别\名字.ini` 组织，用户态递归展开，完全不用内核态模板合并
   （那是兼容官方用户空间组件的权宜之计）。
3. **无 server**：删除 V1 的会话 server（命令枢纽/日志泵/Guardian 全废）。
   监视职责收敛为一个**公共 monitor 进程**（每会话单实例、纯轮询、无常驻中枢职能）。
4. **注册即文件**：主 ini 只有一条部署期写死的目录通配 `ImportBox=`；
   注册/注销 = 在该目录放/删一个缓存 ini 文件 + 一次 ReloadConf IOCTL。
   文件系统操作替代全部主 ini 键级写竞争。
5. **盒目录即真理源**：`PATH\TO\box` 三件套（`drive\ user\ sandbox.ini`）
   是配置与数据的唯一持久所在；AppData 下全部是可再生运行件。

---

## 2. 总架构（无 server：exec / register / 公共 monitor 进程模型）

```
┌──────────────────────── 一个 WTS 登录会话 (session N) ────────────────────────┐
│                                                                              │
│  sbie-cli exec|register|unregister|sync-config|ps        sbie-cli --monitor  │
│  （短生命周期，直接调驱动 IOCTL + 文件操作）              （公共监视器，单实例）│
│        │                                                        │           │
│        │  ① 解析 sandbox.ini → 展开模板 → 写缓存 ini           │  轮询任务目录 │
│        │  ② SbieApi_ReloadConf（IOCTL，无 SbieSvc ini 通道）    │  枚举进程数   │
│        │  ③ 放 running.lock → 写 <box>.task 任务文件            │  归零→teardown│
│        │  ④ SbieSvc RunSandboxed 启动盒内进程（唯一保留的       │               │
│        │     SbieSvc 依赖，见 §9 exec）                         │               │
│        └──────► 任务目录 %LOCALAPPDATA%\SandboxieOSS\monitors\ ◄┘（文件约定）  │
└──────────────────────────────────────────────────────────────────────────────┘
        ▲                                   ▲
   SbieDrv.sys（内核）                   SbieSvc.exe（系统服务）
   - Conf_Read：主 ini                    - RunSandboxed（进程启动）
     → ImportBox=<AppData>\boxes\ 通配     （MSGID_SBIE_INI_SETTING 之外的
     → 逐文件导入缓存 ini                  ProcessServer 通道，03 §4）
   - ReloadConf / QueryConf / EnumProcs
     IOCTL 对任意非沙盒进程开放

磁盘布局（三个区域）：
  PATH\TO\box\                     ← 用户盒目录（真理源）
    sandbox.ini  drive\  user\  running.lock（生命周期内）
  %LOCALAPPDATA%\SandboxieOSS\     ← 运行件（可再生）
    boxes\<box>.ini                ← 展开缓存（ImportBox 通配目录 = 注册工件）
    monitors\<box>.task            ← monitor 任务文件
    aliases.json                   ← alias → 盒目录索引
  主 Sandboxie.ini                 ← 部署期写一行 [GlobalSettings] ImportBox=…\boxes\
```

进程模型要点：

- **exec/register 是短生命周期进程**：直接 `NtDeviceIoControlFile(\Device\SandboxieDriverApi)`
  （SbieCore/DriverApi 绑定表，02 §1），无 IPC、无管道、无 leader 要求。
- **公共 monitor**：每会话至多一个，由 exec 顺手拉起（`DETACHED_PROCESS |
  CREATE_BREAKAWAY_FROM_JOB`，失败回退无标志，复用 00 §5 的拉起经验）；
  单例用命名互斥体 `Local\SbieOSS_Monitor_S<sid>`（abandoned 可安全接管）。
  **它不是 server 还魂**：不监听管道、不代理命令、不做日志泵；唯一输入是任务
  目录文件，唯一输 out 是 teardown（删锁→删缓存→ReloadConf→删任务）。
- **SbieSvc 仅剩一项依赖**：`RunSandboxed`（盒内进程启动需要服务侧令牌协助，
  03 §4）。ini 读写的 `MSGID_SBIE_INI_*` 通道 V2 完全不用。

---

## 3. V2 模板体系规格

### 3.1 目录与发现

- 环境变量 `SBIE_TEMPLATE_DIR`，可含多个路径，`;` 分隔，**按序扫描，先命中先用**。
- 组织：`<模板根>\<类别>\<名字>.ini`，例 `BoxTypes\Standard.ini`、`Games\MC.ini`。
- 类别 = 一级子目录名；模板引用名 = `类别\名字`（如 `Games\MC`）。
  同目录风格引用允许省略类别（仅当名字在扫描序中唯一；歧义 = 错误）。
- 模板文件必须恰含一个 `[Template]` 节；BOM/UTF-8/UTF-16 均支持（解析器同
  sandbox.ini，见 §5.1）。

### 3.2 模板文件格式（用户规格原文语义固化）

```ini
[Template]
Tmpl.Title=Standard Box           ; 元数据：标题（必填，非空）
Tmpl.Class=BoxTypes               ; 元数据：类别（必须等于所在目录名，校验项）
Tmpl.Url=https://…                ; 可选元数据
Tmpl.ScanProduct=…                ; 可选元数据（安装探测提示；V2 不消费，保留迁移面）
OpenFilePath=%USER%\…             ; 内容键 = 任意沙盒配置键（原样透传给缓存）
ClosedFilePath=…                  ;   同上
ProcessGroup=<组>                 ; 进程组定义（透传）
Template=WebBrowserDefaults       ; 嵌套引用（用户态递归展开）
Template=Games\MC                 ; 嵌套引用（带类别路径）
```

规则：

1. `Tmpl.*` 前缀键 = 元数据，**不进入缓存**（对齐驱动合并时跳过 `Tmpl.*` 的行为，
   conf.c:1291）。
2. 内容键键名不做白名单校验（驱动/官方用户态组件自行消费，见 §11.4 键面盘点），
   但展开引擎校验：键名非空、不含 `=`/换行、值不含换行。
3. `Template=` 嵌套深度上限 **8**；环检测 = 引用路径栈出现重复即报
   `TEMPLATE_LOOP`（退出码 11）。
4. 元数据 `Tmpl.Class` 与目录名不符 → `TEMPLATE_CLASS_MISMATCH`（加载期校验，
   防目录被挪动后引用名失效）。

### 3.3 变量语义（两段式）

| 变量族 | 例子 | 在哪展开 | 时机 |
|---|---|---|---|
| 模板变量 | `%Tmpl.Firefox%` | **V2 展开引擎（用户态）** | 生成缓存时冻结为字面值 |
| 系统变量 | `%USER% %SystemDrive% %AppData% %LocalAppData% %ProgramFiles% …` | 驱动（conf_expand.c:332-700） | 盒初始化时，按盒的用户 SID 上下文 |

理由：`%Tmpl.X%` 在驱动侧查全局 `[TemplateSettings]` 节（conf_expand.c:631-636、
`Conf_Expand_Template`），该节来自驱动自动加载的安装目录 `Templates.ini`
（conf.c:353-364）——V2 不依赖它，故模板变量必须在用户态冻结。
系统变量留给驱动展开，缓存保持用户/机器无关的可读形态（用户规格示例
`OpenFilePath=%USER%\…` 即此意图）。

### 3.4 与 V1（内核态模板）的关系

- V2 缓存**零 `Template=` 行**（D3）：驱动侧 `Conf_Merge_Template`
  （conf.c:1267-1310）对每个 `Template=Xxx` 找 `[Template_Xxx]` 节，找不到只记
  `MSG_CONF_MISSING_TMPL` 日志不报错——留着会产生每次 reload 一条噪音且语义误导，
  故剥除（自检规则见 §5.4）。
- 驱动每次 reload 仍会读安装目录 `Templates.ini`（conf.c:353-364）并把它
  `[DefaultTemplates]` 的 `Template=` 清单合并进 GlobalSettings（conf.c:1167-1173）。
  V2 **不修改该文件**：这 10 条机器级兼容默认（RpcPortBindings/SpecialImages/COM/
  WindowsExplorer/ThirdPartyIsolation/BlockSoftwareUpdaters/BlockWinRM/
  OpenWinInetCache/CredentialUIBroker/MSI_Lite）对 V2 盒继续生效，是白赚的兼容层。
  V1 模板"掏空"指 V2 资产不再引用任何 `[Template_*]` 节；迁移映射见 §11。

---

## 4. 盒目录与缓存布局

### 4.1 盒目录（真理源，用户任意位置）

```
PATH\TO\box\                        ← 盒目录，名字 = 盒名（驱动节名/缓存文件名同源）
  sandbox.ini                       ← 单盒 KV + Template= 引用（用户手编）
  drive\   user\                    ← FileRootPath 实际承载（驱动创建）
  running.lock                      ← 生命周期锁（exec 放、monitor 删；仅存在期间）
```

`sandbox.ini`（用户规格示例语义）：

```ini
[MC]
Enabled=y
BlockNetworkFiles=y
RecoverFolder=%Personal%
RecoverFolder=%Desktop%
BorderColor=#00FFFF,ttl
Template=Games\MC
Template=WebBrowserDefaults
ConfigLevel=9
UsePrivacyMode=y
AutoRecover=n
```

- 允许多值键（`RecoverFolder=`、`Template=` 各多行），保序。
- 节名必须等于盒目录基名（写入缓存时校验；否则 `INVALID`）。

### 4.2 盒名约束（硬校验，复用 V1 ValidateName 语义）

- ≤38 WCHAR（BOXNAME_COUNT=40，defines.h:54；驱动 API 路径取 38，api.c:1022；
  ImportBox 通配文件名上限 BOXNAME_COUNT+4=44，conf.c:924）。
- 仅 `[A-Za-z0-9_]`；**不得含点**（ImportBox 注入值按文件名首个 `.` 截断，
  conf.c:1002）；非保留名（GlobalSettings/UserSettings_/Template_ 前缀等）。
- **盒名全局唯一**（驱动节名空间是全机一个）：register 时若 `QueryConf` 已有同名
  节且其 `FileRootPath` 指向别的目录 → `NAME_COLLISION` 报错（见 §6.4）。
- 盒名 = 盒目录基名（不提供"目录名非法但用别名注册"的映射——KISS，OPEN-1）。

### 4.3 AppData 运行区（全部可再生）

```
%LOCALAPPDATA%\SandboxieOSS\
  boxes\                        ← ImportBox 通配目录（主 ini 部署期一行指向这里）
    <box>.ini                   ← 展开缓存 = 注册工件（register 放 / unregister 删）
    <box>.ini.tmp               ← 原子写中转（通配扫描只认 .ini，天然不可见）
  monitors\
    <box>.task                  ← monitor 任务文件（JSON，见 §8.2）
  aliases.json                  ← alias 索引（见 §4.5）
  monitor.log                   ← monitor 追加式诊断日志（可选，环回上限 1MB）
```

缓存 ini 内容（**单节** `[<box>]`，节名=文件基名，conf.c:1037-1046 强制）：

```ini
[MC]
FileRootPath=C:\Users\me\Boxes\MC     ← 显式写盒目录（见下：它在驱动侧真实生效）
Enabled=y
BlockNetworkFiles=y
…（sandbox.ini 直键 + 全部模板展开键，序 = §5.3 合并序）
```

**为什么显式 FileRootPath 会生效（而非被 ImportBox 自动注入值压掉）**——代码实证：

- `Conf_Import_Include` 在读完全部文件行后追加一条自动注入的 `FileRootPath`
  （值 = 缓存文件路径剥扩展名，conf.c:1080-1089，`Conf_Add_Setting(…, insert=TRUE)`）。
- `insert=TRUE` 只把该条插到**链表头**（conf.c:681-684 `List_Insert_Before`）；
  但按名取值走的是哈希表：`Conf_Add_Setting` 统一 `map_append`（conf.c:685）→
  `map_add(append=TRUE)` → **挂在桶尾**（map.h:53、map.c:111-119）。
- `Conf_Get_Helper` 的按名迭代从桶头开始（map.c:333-355，`map_getref`→首个匹配），
  即**插入序 = 文件行序**，index0 = 文件里第一条。
- `Box_InitPaths` 取 `Conf_Get(box, L"FileRootPath", 0)`（box.c:299）→
  **读到的是缓存文件里显式写的那条**；自动注入值退居 index1，无人消费。
- 结论：`缓存路径剥扩展名`的注入值只在缓存**未写** FileRootPath 时兜底生效。
  V2 恒显式写 → FileRootPath 恒 = 盒目录，**无需 junction/软链接**。
- 值格式：DOS 绝对路径（驱动 `Conf_Expand` 走 `File_TranslateDosToNt`，
  conf_expand.c:405-416）；exec/register 负责把用户相对路径规范化为绝对路径后写入。

### 4.4 主 ini（部署期一次性）

```ini
[GlobalSettings]
ImportBox=C:\Users\me\AppData\Local\SandboxieOSS\boxes\
```

- 尾部 `\`（或 `*`）触发目录通配扫描（conf.c:1141-1149）；每次 reload 全目录重扫
  （conf.c:348-350）。非 `.ini`/无名后缀/超长文件被静默跳过（conf.c:1003-1012、924），
  故 `.tmp` 中转与任务目录互不干扰。
- 部署工具/`maint install`（V2 砍单后由安装脚本承担）写这一行；运行期零主 ini 写入。
- 驱动以内核句柄读该目录，ACL 需对 SYSTEM 可读——用户 `%LOCALAPPDATA%` 默认满足。
  多用户同机限制见 OPEN-2。

### 4.5 alias 索引

`aliases.json`（UTF-8，原子替换写：tmp + `MoveFileEx(REPLACE_EXISTING)`）：

```json
{ "mc":  { "box": "MC", "box_path": "C:\\Boxes\\MC",
           "cache": "C:\\Users\\me\\AppData\\Local\\SandboxieOSS\\boxes\\MC.ini",
           "added": "2026-09-27T12:00:00Z" } }
```

- 读-改-写全程持命名互斥体 `Local\SbieOSS_AliasLock`（毫秒级临界区，避免并发
  register 丢更新）。
- `*alias` 解析规则：先查本索引；未命中 → `NOT_FOUND`（退出码 5）。
  alias 与盒名同名时 `*` 前缀消歧（`*mc` 恒走索引，裸 `MC` 恒走路径/盒名）。

---

## 5. 解析-展开引擎规格

### 5.1 sandbox.ini 解析

- 行格式与驱动一致：`键=值`；**注释只认 `#` 行首**（conf.c:834，`;` 不是注释符——
  sandbox.ini/缓存里禁止用 `;` 注释）；行去首尾空白；BOM 检测。
- 多值键保序保重复；节外行 = `INVALID`。
- 引用名解析：`Template=Games\MC` → 扫描序中找 `Games\MC.ini`；纯名 `WebBrowserDefaults`
  → 在全部类别中找唯一同名，多处命中 = `TEMPLATE_AMBIGUOUS`。

### 5.2 变量处理

- `%Tmpl.X%`：查内置变量表（迁移时由 V1 `[TemplateSettings]` 固化而来，§11.3），
  支持用户在 sandbox.ini 里 `[TemplateVars]` 节覆盖/新增；未定义 = `TEMPLATE_VAR_MISSING`。
- 系统变量（`%USER% %SystemDrive% %AppData%` 等）**原样透传**（驱动展开，
  conf_expand.c:332-700 支持的全部变量族）。
- `%%` 转义为字面 `%`。

### 5.3 合并序（冲突序，可执行级精确）

1. 以 sandbox.ini 的 `[<box>]` 节为初始 KV 序列（文件序）。
2. 自上而下扫描；遇 `Template=<ref>`：
   a. 递归展开被引模板（先depth-first 展开其自身的 `Template=` 嵌套）；
   b. 把该模板的**内容键**（非 `Tmpl.*`）按模板文件序**追加**到结果序列尾部
      （`Template=` 行本身不入结果）。
3. **同名单值键冲突**：先出现者优先（与驱动 index0=文件首条、盒自身键先于模板键的
   读取序一致，见 §4.3 论证）——即 **sandbox.ini 直键 > 先引用的模板 > 后引用的模板**；
   冲突时后到值丢弃并记 warning（`--verbose` 可见）。
4. **多值键**（`RecoverFolder/OpenFilePath/ProcessGroup/Template 以外的一切天然多值键，
   按驱动读取语义即 index 递增枚举）**：不判重、保序连接**（直键在前，模板按引用序）。
5. 展开完成后追加显式 `FileRootPath=<盒目录绝对路径>`（若 KV 中已有直键
   FileRootPath，以 sandbox.ini 的为准——用户显式优先，引擎只补缺）。

### 5.4 缓存生成与硬性自检

- 写缓存：`<box>.ini.tmp` → 全部字节落盘 → `MoveFileExW(REPLACE_EXISTING)` 原子替换。
- 生成后**自检（D3，硬性）**：
  1. 文件中除 `[<box>]` 外无任何节头；
  2. 无任何 `Template=` 行（零残留）；
  3. `FileRootPath` 存在且 == 规范化盒目录；
  4. 节名 == 文件基名 == 盒名；文件名 ≤44 WCHAR 且含 `.ini` 后缀。
  任一失败 = `CACHE_VALIDATION_FAILED`（退出码 12），tmp 不替换、缓存不动。

---

## 6. 注册协议（ImportBox 生命周期）

### 6.1 注册（register 语义，exec 内部同路径）

```
1. 解析目标（路径或 *alias）→ 盒目录绝对路径 → 盒名
2. 名校验（§4.2）；读 sandbox.ini（缺文件 = NOT_FOUND）
3. 冲突预检：QueryConf(<box>, "FileRootPath", 0, NO_GLOBAL)（§6.4 探测法）
   - 命中且值 != 本盒目录           → NAME_COLLISION（退出码 7 附现有值）
   - 命中且值 == 本盒目录            → 幂等：跳到 6
   - 未命中                          → 继续
4. 展开引擎生成缓存（§5）+ 自检 → 原子落位 boxes\<box>.ini
5. SbieApi_ReloadConf(session, 0)   ← IOCTL 直调；无 admin/leader 要求
   （Conf_Api_Reload 仅拒沙盒内调用者，conf.c:1701-1702）
6. 等待注册可见：轮询探测（§6.4），≤10s（--wait 覆盖）；
   超时 → REGISTRATION_TIMEOUT（退出码 10）
```

### 6.2 注销（unregister 语义，monitor teardown 同路径）

```
1. 盠除任务文件（若在）：monitors\<box>.task
2. 盒内有进程 → BOX_BUSY（退出码 9；V2 无 kill 命令，用户自行终止，OPEN-6）
3. 删 running.lock（若在）
4. 删 boxes\<box>.ini（+ 顺手清 >10min 的 *.tmp 残骸）
5. SbieApi_ReloadConf
6. 轮询"注册消失"（§6.4 探测返回未命中）≤10s
7. 删 alias 条目（若有）
```

### 6.3 生效机制（为什么文件放删就够了）

- 驱动每次 `Conf_Read` 全量重建配置池（conf.c:215-340：新 pool/新 sections 后整体
  换入），先读主 ini、再扫全部 `ImportBox=`（conf.c:348-350）。
- 目录通配（`Conf_Import_Includes`，conf.c:865-960）逐文件导入：
  - 文件名（含 `.ini`）≤44 WCHAR、≥5（conf.c:924）；
  - 首节名必须 == 文件基名（conf.c:1037-1046）；
  - 自动注入 FileRootPath=缓存路径剥扩展名（conf.c:1080-1089，被显式行压制，
    见 §4.3）；额外节容忍但 V2 不产出（conf.c:1074-1077）。
- 原子性：`.tmp` 中转 + 扩展名过滤 ⇒ 通配扫描**永不见半写文件**；
  删除即注销，不存在键级读-改-写竞争——这是 D2 的全部意义。
- **并发语义**：多 exec 同时放各自缓存文件 = 天然并行（不同文件）；ReloadConf
  并发 IOCTL 由驱动内部 `Conf_Lock` 串行（conf.c:2205-2212 同款互斥在 reload 路径）。
  最后一次 reload 决定可见集合，且集合由目录内容唯一决定 ⇒ 收敛、无竞态。
- **被否决的替代**：`API_UPDATE_CONF`（conf.c:2134-2151）可在线改配置但要求调用者
  是会话 leader 或 SbieSvc，且 reload 后即失——不可用、不稳定，弃。

### 6.4 注册探测（"注册尚在/已消失"的判定）

- `注册可见` = `SbieApi_QueryConfAsIs(<box>, L"FileRootPath", 0)` 返回成功且值非空
  （带 `CONF_GET_NO_GLOBAL` 语义防止 GlobalSettings 兜底假阳）。
- 由于恒显式写 FileRootPath，命中值应 == 盒目录；不等 = 名字冲突或缓存陈旧，
  exec 状态机按 §7 的 UNREGISTERING/FOREIGN 分支处理。
- 轮询节奏：50ms 起，200ms 封顶（指数退避），总限 `--wait`（默认 10s）。

---

## 7. 锁协议状态机（exec / monitor / 手动命令全视角）

### 7.1 状态变量

- `L` = `running.lock` 存在（盒目录内，§7.4 格式）
- `R` = 注册可见（§6.4 探测）
- `P` = 盒进程数 > 0（SbieApi_EnumProcesses 过滤盒名）
- `T` = 任务文件存在（monitors\<box>.task）
- `M` = 本会话公共 monitor 存活（互斥体探测）

### 7.2 全状态迁移表

盒的世界状态（观测组合）与进入 exec / monitor 时的动作：

| # | L | R | P | T | 语义 | 观察者动作 |
|---|---|---|---|---|---|---|
| S0 | – | – | – | – | 空闲 | **exec**：放锁(L=1) → 注册（§6.1，缓存生成+reload）→ 等 R → 启动进程(P=1) → 写任务(T=1) → 确保 M → 退出 0 |
| S1 | – | R | – | – | 已注册静止（register 过 / monitor 已收尾） | **exec**：放锁 → 幂等注册（跳过生成，直接 reload 探测或重生成——缓存 mtime 旧于 sandbox.ini 时重生成，等价 sync-config 内联）→ 启动 → 任务 → monitor |
| S2 | L | – | – | – | **注册中**（锁已放、reload 未生效/探测未中） | **exec**：等 R（≤--wait）→ 转 S3 动作；超时 = REGISTRATION_TIMEOUT(10) |
| S3 | L | R | – | – | 已锁注册、进程未起（exec 两步之间） | **exec**：直接启动 → 任务 → monitor（用户规格"直接启动"分支） |
| S4 | L | R | P | T | 运行中 | **exec**（新 cmdline）：直接启动（追加进程）；monitor 正常跟踪 |
| S5 | L | R | – | T | 归零去抖中（进程刚结束，monitor 未到阈值） | **exec**：直接启动（zero_streak 被清零，取消 teardown）；monitor：继续观测 |
| S6 | – | R | – | – | **注销中**（monitor 已删锁删缓存、reload 后注册尚未消失的窗口，或探测时延） | **exec**：等注册消失（轮询 §6.4，≤--wait）→ 回 S0 流程放锁注册启动；超时 = REGISTRATION_TIMEOUT(10)。此即用户规格"等注册消失 → 放锁 → 注册 → 启动 → monitor"分支 |
| S7 | L | ? | – | – | 孤儿锁（exec 中途崩溃：无任务文件、无进程） | **exec**：视为可收养——探测 R：R 且 FileRootPath==本盒 → S3 直启；否则清锁走 S0 全流程；**ps**：标注 `orphan-lock` |
| S8 | L | R | P | – | 运行中但任务文件丢失（monitor 重建窗口/手删） | **exec**：补写任务文件 + 确保 M（自愈）；ps 标注 `no-monitor` |

**monitor 视角**（对每个任务，§8.3 详述）：`(P: 1→0 边沿 && 去抖 K=2)` 或
`(任务年龄>grace && 从未见过 P)` → teardown：删 L → 删缓存 → ReloadConf →
等注册消失(内部 ≤10s) → 删 T。任何时刻 P 变 1 → 清零去抖计数、取消进行中的
teardown（teardown 第一步删锁前重查 P）。

### 7.3 exec 决策主干（伪码）

```
target = argv[0]                    # PATH 或 *alias
box   = resolve(target)             # alias→索引；路径→规范化+基名
loop (≤ --wait, 默认 10s):
    R = probe(box)
    if lock exists:
        if R and root(R) == boxdir: start(); add_task(); ensure_monitor(); exit 0
        if not R:                   # S2 注册中或 S7 孤儿
            wait_probe(); continue
        if R and root(R) != boxdir: fail(NAME_COLLISION, 7)
    else:
        if R:
            # S1 或 S6 尾巴：注册在但无锁。
            # 区分：S6=注销中 → 等注册消失；S1=静止已注册 → 直接接管。
            if cache_file(box) exists: take_lock(); start(); add_task(); ensure_monitor(); exit 0
            else: wait_register_gone(); continue        # S6
        else:
            take_lock(); register(box); wait_probe(); start(); add_task(); ensure_monitor(); exit 0
fail(REGISTRATION_TIMEOUT, 10)
```

（`cache_file` 存在性用于区分 S1/S6：注销流程删缓存先于删注册可见性，故
"无锁 && R && 无缓存"必是注销尾巴；静止已注册盒缓存恒在。）

### 7.4 running.lock 格式与 stale 判定

`PATH\TO\box\running.lock`（ini 风格，UTF-8）：

```ini
v=2
box=MC
alias=mc
box_path=C:\Boxes\MC
cache=C:\Users\me\AppData\Local\SandboxieOSS\boxes\MC.ini
creator_pid=4124
created=2026-09-27T12:00:03Z
```

- 放锁 = `CreateFileW(CREATE_ALWAYS)` + 立即写全量内容（内容小，单次写原子足够）；
  删锁 = `DeleteFileW`。
- **stale 判定**（仅用于诊断/收养决策，不做强依赖）：`creator_pid` 亡
  （`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` 失败）且 P=0 且 T=–
  → `ps` 标 `orphan-lock`，exec 按 S7 收养。
- 盒目录三件套语义下锁与 `sandbox.ini` 同级；FileRootPath 根下的杂散文件对驱动无害
  （驱动只认 `drive\`、`user\` 子树）。

---

## 8. monitor 规格（公共单进程多任务，纯轮询）

### 8.1 进程模型（D6）

- 入口：`sbie-cli --monitor [--poll-ms 250] [--grace-sec 5] [--empty-ticks 4]`，
  由 exec/register 结尾顺手拉起（`DETACHED_PROCESS|CREATE_BREAKAWAY_FROM_JOB`，
  失败无标志重试，00 §5 拉起经验复用）；**不在用户命令面出现**。
- 单例：`Local\SbieOSS_Monitor_S<sid>` 命名互斥体；`ERROR_ALREADY_EXISTS` →
  本实例退出 0（abandoned 可安全接管，无需清理逻辑）。
- 生命周期：任务表空且持续 `empty-ticks`（默认 4 tick ≈1s）→ 退出 0
  （给背靠背 exec 留复用窗口）；退出前不删任何任务文件。
- **无 IPC**：不监听任何管道/端口；唯一输入 = 任务目录；`--monitor` 进程内也拒绝
  一切非监视参数（防误用为命令枢纽）。

### 8.2 任务文件（方案 a：文件约定，D6 推荐已采纳）

`%LOCALAPPDATA%\SandboxieOSS\monitors\<box>.task`（JSON，UTF-8，原子替换写）：

```json
{ "v": 1, "box": "MC", "box_path": "C:\\Boxes\\MC",
  "cache": "C:\\…\\boxes\\MC.ini", "alias": "mc",
  "lock": "C:\\Boxes\\MC\\running.lock",
  "created": "2026-09-27T12:00:03Z", "creator_pid": 4124 }
```

- CLI 创建/注销任务 = 写/删该文件；monitor 每 tick 重扫目录对账（新增→入表，
  文件消失→出表）。与 ImportBox 目录通配同构（D2 风格统一），零 IPC、崩溃自愈。
- 备选 b（会话命名管道 add/remove/shutdown）被否：引入第二套通信机制、需处理
  管道生命周期与权限，违背 KISS；文件方案对崩溃/竞态的容错严格更优。

### 8.3 轮询循环与边沿语义（纯轮询，D1）

```
每 tick（默认 250ms）：
  1. 扫任务目录 → 对账任务表（新任务 zero_streak=0, born=tick, seen_proc=false）
  2. 一次 SbieApi_EnumProcesses（全量）→ 按盒名计数（多任务一次 IOCTL，O(1) 驱动往返）
  3. 对每个任务：
       P = count(box)
       if P > 0: zero_streak=0; seen_proc=true
       elif seen_proc: zero_streak++
       elif age > grace: zero_streak = K     # 从未起过进程的孤儿任务，宽限后直接判死
       if zero_streak >= K(=2) and not teardown_in_progress:
           if P > 0: continue                # 删锁前重查（竞态保险）
           teardown(box)
  4. 任务表空计数 → empty-ticks 阈值 → 退出 0
```

`teardown(box)`（= §6.2 注销的 monitor 侧实现）：

```
删 running.lock → 删缓存 ini → SbieApi_ReloadConf →
轮询注册消失（50..200ms 退避，≤10s；超时只记 monitor.log，不重试不阻断其他任务）
→ 删任务文件
```

- 边沿语义总结：**非零→零需连续 K=2 次 tick 确认**（500ms @250ms），
  期间任何 P>0 取消；teardown 首步（删锁）前重查 P，杜绝"重启窗口误杀"。
- monitor 自身崩溃：任务文件仍在，下一次任意 exec/register 发现互斥体可取 →
  拉起新 monitor → 首扫全部收编（自愈，无需持久状态）。

### 8.4 明确不做（D1）

- 不做驱动日志泵（`API_GET_MESSAGE` 要求会话 leader，api.c:728-732；多 monitor
  必然单 leader，弃之最简）。
- 不做 trace 泵、interactive queue、Guardian 触发器、AutoDelete/AutoRemove、
  `Temp_` 一次性沙盒语义（§12.4 砍单）。

---

## 9. 命令面规格（V2 唯一面，D4）

### 9.1 命令表

| 命令 | 形式 | 语义 |
|---|---|---|
| exec | `sbie-cli exec PATH\TO\box [cmdline]`<br>`sbie-cli exec *alias [cmdline]` | 相对路径按 cwd 规范化；无 cmdline = 盒默认 shell（cmd.exe）。走 §7 状态机；stdout 直通子进程 |
| register | `sbie-cli register PATH\TO\box [alias]` | §6.1 注册；**无锁、无任务文件、无 monitor**（持久注册）；alias 缺省 = 盒名小写 |
| unregister | `sbie-cli unregister *alias` 或 `PATH\TO\box` | §6.2；盒忙拒绝 |
| sync-config | `sbie-cli sync-config *alias 或 PATH\TO\box` | sandbox.ini 变更 → 重展开 → 重写缓存（原子）→ ReloadConf。盒运行中：默认放行 + stderr 警示（驱动 reload 本就支持运行中热更，OPEN-3） |
| ps | `sbie-cli ps [*alias 或 PATH\TO\box]` | 无参 = 全部 V2 盒（注册可见 ∧ 缓存目录中有其 ini）+ L/T/P 状态列；有参 = 该盒进程明细（PID/镜像/启动时间） |
| kill-box | `sbie-cli kill-box *alias 或 PATH\TO\box` | SvcClient KillAll 终止盒内全部进程；杀空触发 monitor 归零边沿 → 自动注销（与 exec 的竞态由 §7 状态机收敛，S6→S0 重注册路径已实测） |
| kill | `sbie-cli kill <PID>` | SvcClient KillOne；PID 须属已注册沙盒（驱动 QueryProcess 判定），否则 NOT_FOUND(5) |

公共旗标：`--json`（V1 信封 `{ok,data|error{code,message,ntstatus?}}` 沿用，
04 §5/§6 契约保留）、`--quiet`、`--wait <sec>`（默认 10）、`--verbose`。
导出/复制**不做**（原地 zip/拷即可，用户规格 #6）。

### 9.2 退出码（V1 表的 V2 收敛版）

| 码 | 常量 | 含义 |
|---|---|---|
| 0 | OK | 成功 |
| 1 | GENERIC | 未分类失败 |
| 2 | USAGE | 语法错误 |
| 3 | DRIVER_UNAVAILABLE | 驱动未装/未跑 |
| 5 | NOT_FOUND | alias/盒目录/sandbox.ini/模板不存在 |
| 6 | ACCESS_DENIED | 本进程在沙盒内 / SVC_RUN 失败 |
| 7 | INVALID / NAME_COLLISION | 盒名非法；或同名节已归属其他目录（附现有 FileRootPath） |
| 9 | BOX_BUSY | unregister 遇活动进程 |
| 10 | STATE_TIMEOUT | 等注册可见/消失超时（--wait） |
| 11 | TEMPLATE_* | 模板缺失/环/歧义/变量缺失/类目不符 |
| 12 | CACHE_VALIDATION_FAILED / REGISTRATION_FAILED | 自检失败；ReloadConf 返回失败 NTSTATUS |
| 13 | SPAWN_FAILED | RunSandboxed/进程启动失败（exec 清理锁与缓存后退出） |

（V1 的 4=SERVER_UNAVAILABLE、8=RETRY_SUGGESTED 随 server 一并消亡。）

### 9.3 exec 的进程启动通道

沿用 SbieSvc ProcessServer `RunSandboxed`（03 §4；SbieCore/SvcClient 保留的唯一
运行期用途）：盒名 + 命令行 + 工作目录 → 服务侧起 `Start.exe` 链路。
启动失败（13）时 exec 负责回收：删锁、删缓存、ReloadConf（不留半注册）。

---

## 10. 关键坑与决策（a–h 定论，带锚点）

**a. ImportBox 的 FileRootPath 自动注入冲突** —— 实读结论与直觉相反，**显式行胜出**。
注入点 conf.c:1080-1089（读完全部行后 `Conf_Add_Setting(…, insert=TRUE)`，值 =
缓存路径剥首个 `.` 前缀）。`insert=TRUE` 只影响链表头插入（conf.c:681-684）；
按名取值走哈希表：`map_append`→`map_add(append=TRUE)`→挂桶**尾**（map.h:53、
map.c:111-119），`Conf_Get_Helper` 桶头起迭代 = 插入序 = 文件行序（conf.c:1318-1359、
map.c:333-355），`Box_InitPaths` 取 index0（box.c:299）→ **缓存里显式写的
FileRootPath 真实生效**，注入值退 index1 无人读。解法 = 恒显式写
`FileRootPath=<盒目录>`（§4.3）；注入值仅在缺省时兜底。junction/AppData 盒根等
补救方案全部不需要。注意文件名不得含 `.`（首个 `.` 截断语义，conf.c:1002）。

**b. 驱动日志过滤的 leader 唯一性** —— 已拍板弃用日志通道（D1）。备查锚点：
`Api_GetMessage` 非服务进程仅可读自己为 leader 的会话（api.c:725-733）；
`Session_Api_Leader` set 路径要求非沙盒且当前 leader 是自己（session.c:334-360）；
leader 进程退出时 `Session_Cancel` 连 SESSION 块一起拆（process.c:1504-1511、
session.c:244-275）。另实证：**驱动无进程退出日志**（启动=MSG_1399，
process.c:1399；2198/2199 是 dll 侧文件复制/恢复事件，dll/file_copy.c:887、
file_recovery.c:992）——即便做日志校准也只能即时感知"增"不能感知"减"，
纯轮询（§8.3）是正确且必要的选择。

**c. 注册/取消注册的并发写** —— D2 以文件放删 + ReloadConf IOCTL 根治：主 ini
运行期零写入；缓存目录即注册集合，天然幂等收敛（§6.3）。SbieSvc ini 通道的
`m_critsec` 串行（sbieiniserver.cpp:96-107）与值级 `AddValue/RemoveValue`
（1060-1091）不再被 V2 触碰。`Conf_Api_Reload` 无 admin/leader 门槛
（conf.c:1696-1702），并发 reload 由驱动内部锁串行。

**d. 缓存 ini 的节名与 Template= 残留** —— 单节强制（首节名==文件基名，
conf.c:1037-1046；多节虽被容忍 conf.c:1074-1077 但 V2 不产出）；**零 Template=
残留**为生成自检硬项（D3，§5.4）：残留行会令驱动每次 reload 记
`MSG_CONF_MISSING_TMPL`（conf.c:1304-1307，找不到 `Template_Games\MC` 节）且
语义误导。溯源信息改记于 `#` 注释行（驱动解析器认 `#` 不认 `;`，conf.c:834），
如 `# expanded from: Template=Games\MC (2026-09-27T12:00:03Z)`。

**e. V1→V2 映射表** —— 见 §11（437 节全量盘点：9 类 390 个有类模板全迁 +
10 个无类默认模板迁 `System\`，46-10-2（DefaultTemplates/TemplateSettings 两节
非模板）≈34 个死壳丢弃）。

**f. 旧命令面/V1 模式处置** —— D4：删。现有 ~60 命令（box 20+/proc 10/cfg 10/
template 6/log 3/force 3/maint 5/img 7/usb 2/trace 2/doctor/server 3/status/version，
注册面见 `cli/Commands.cpp:564-610` 及 `cli/Commands/*` 各 `Register*`）全部删除；
server/、ipcc/、ServerConnect 同删。Guardian（server/Guardian.cpp 空箱守护）的
"注销"职责由 monitor 承接，其触发器/AutoDelete/AutoRemove/Temp_ 语义**砍掉**
（§12.4 砍单）。快照/恢复/磁盘映像/USB/trace/log 面：砍。

**g. GUI 影响** —— GUI 为 sbie-cli 薄壳（docs/09），命令面变化后需后续波次适配
（一句备注，D5）。

**h. 布局细节** —— §4（AppData 三目录 + 盒目录锁）与 §7.4（锁内容 = pid+路径+
时间戳，stale = pid 亡 ∧ 无进程 ∧ 无任务）。

---

## 11. V1→V2 模板迁移映射表

### 11.1 总量与分流（Templates.ini 437 节实盘）

| V1 Tmpl.Class | 数量 | V2 类别目录 | 处置 |
|---|---|---|---|
| WebBrowser | 165 | `WebBrowser\` | 全迁 |
| Desktop | 72 | `Desktop\` | 全迁 |
| Security | 58 | `Security\` | 全迁 |
| Misc | 25 | `Misc\` | 全迁 |
| MediaPlayer | 22 | `MediaPlayer\` | 全迁 |
| Print | 17 | `Print\` | 全迁 |
| EmailReader | 17 | `EmailReader\` | 全迁 |
| TorrentClient | 9 | `TorrentClient\` | 全迁 |
| Download | 5 | `Download\` | 全迁 |
| （无类，默认集） | 10 | `System\` | 全迁（RpcPortBindings、SpecialImages、COM、WindowsExplorer、ThirdPartyIsolation、BlockSoftwareUpdaters、BlockWinRM、OpenWinInetCache、CredentialUIBroker、MSI_Lite；V1 里由 `[DefaultTemplates]` 引用） |
| （无类，结构节） | 2 | — | `[DefaultTemplates]`/`[TemplateSettings]` 非模板：前者迁移为 `BoxTypes\Standard.ini` 的嵌套引用清单，后者固化为 V2 变量表（§11.3） |
| （无类，死壳） | ~34 | — | **丢弃**：空键或过时（Neon、Maxthon2、Outlook_Express、DefenseWall、OnlineArmor、StickyPassword、ActiveSync、Windows2000Internat、FreeDownloadManager、WindowsFontCache、Windows10CoreUI、Fix_for_Win7、PlugPlayService、IExplore_Credentials、VirtualDesktopManager、DeviceSecurity、VPNTunnel、Edge_Win11Fix、InternetDownloadManager、BullGuard、Bsecure、CyberPatrol 等——远古软件/远古系统/无内容壳） |

净结果：**400 个模板文件**（390+10）+ `BoxTypes\Standard.ini`（新建）+ 变量表。

### 11.2 迁移改写规则（键面原样，仅组织形式变）

1. `[Template_<名>]` → `<类别>\<名>.ini` 的 `[Template]` 节；`Tmpl.Title/Tmpl.Class/
   Tmpl.Url/Tmpl.Scan*` 原样保留（Scan* 为惰性元数据，供未来 doctor 类命令用）。
2. 内容键**逐行原样复制**（OpenFilePath/OpenIpcPath/OpenWinClass/ApproveWinNtSysCall/
   LingerProcess/ClosedFilePath/SpecialImage/ForceProcess/ProcessGroup/…，完整键频表
   见 §11.4）——消费方横跨 drv/dll/svc，V2 不改不审。
3. `%Tmpl.X%` 引用替换为变量表冻结值或 `%Tmpl.X%` 保留由引擎解析（§11.3）。
4. V1 模板相互不引用（Templates.ini 内 `Template=` 仅出现在 `[DefaultTemplates]`），
   迁移零嵌套特例；`BoxTypes\Standard.ini` 是 V2 唯一新增嵌套模板：
   `Template=System\RpcPortBindings` … 十连引用 + `Enabled=y` 等基线键。

### 11.3 变量表（源自 `[TemplateSettings]`，Templates.ini:44-100）

引擎内置（可被 sandbox.ini `[TemplateVars]` 覆盖）：
`Tmpl.Firefox / Waterfox / PaleMoon / SeaMonkey / LibreWolf / Zotero / Chrome / Edge /
Vivaldi / Brave / Opera / Yandex / Ungoogled / Iron / Maxthon_6 / Dragon / Osiris /
Slimjet / Office_Outlook / Windows_Vista_Mail / Windows_Live_Mail / Thunderbird /
RoboForm / FinePrint / eDocPrinter / eM / Incredimail / KasperskyDataRoot / TheBat …`
（值 = 原 `Tmpl.X=%AppData%\…` 右侧原样，保留 shell-folder 变量供驱动展开）。

### 11.4 模板键消费面盘点（迁移安全性依据）

| 键（频次） | 消费方 |
|---|---|
| OpenIpcPath(494) / OpenFilePath(361) / OpenWinClass(109) / OpenPipePath(76) / ClosedFilePath(39) / NormalFilePath(24) / OpenKeyPath(23) / NormalKeyPath(13) / ReadIpcPath(2) / ClosedKeyPath(3) / WriteKeyPath(2) / OpenClsid(20) / ClosedClsid(4) | 驱动（drv 各规则模块） |
| DefaultFolder(192) / RecoverFolder / AutoRecover / AutoRecoverIgnore(15) / CopyAlways(7) / DontCopy(38) / BoxNameTitle(7) / SpecialImage(43) / RpcPortBinding(12) / RpcPortBindingIfId(8) / RpcPortFilter(7) / SoftwareUpdater(22) / SandboxService(6) / SkipHook(6) / DelayLoadDll(4) / DisableWinNtHook(7) / NoRenameWinClass(7) / FakeAdminRights(3) / DisableBoxedWinSxS(2) / DenyHostAccess(2) | SbieDll（盒内 dll，经驱动 Conf_Query 读盒配置） |
| ApproveWinNtSysCall(90) | 驱动（syscall 审批） |
| LingerProcess(42) / ForceProcess(35) | Linger=官方服务侧/Force=驱动 process_force.c |
| ProcessGroup(15) / NetworkAccess(4) / RunServiceAsSystem(6) | 驱动 + svc |

结论：全部为活配置键，原样迁移安全；无需逐键重审。

---

## 12. 模块划分与波次计划

### 12.1 V2 目标文件面（多 agent 并行切分）

```
SandboxieOSS/
  SbieCore/Model/V2/                 ← 新增（纯用户态，可单测，不碰 IPC）
    TemplateDir.h/.cpp               §3  目录扫描/解析/引用解析/类目校验
    Expander.h/.cpp                  §5  合并序/变量冻结/嵌套环检测/冲突策略
    BoxCache.h/.cpp                  §5.4 缓存生成+原子落位+自检（零 Template=）
    Registry.h/.cpp                  §6  注册/注销/探测/ReloadConf 封装
    AliasIndex.h/.cpp                §4.5 索引读写（互斥+原子替换）
    TaskDir.h/.cpp                   §8.2 任务文件写/删/枚举
  sbie-cli/monitor/                  ← 新增（替代 server/）
    MonitorMain.h/.cpp               §8  单例互斥/任务表/轮询循环/teardown
  sbie-cli/cli/
    V2Commands.h/.cpp                §9  五命令 + 状态机（§7）接线
    Commands.cpp / Cli.cpp / main.cpp 改：注册表只挂 V2 面 + --monitor 入口
  删除：sbie-cli/server/ 全部、sbie-cli/ipcc/ 全部、cli/ServerConnect.*、
        cli/Commands/ 全部 V1 命令文件、SbieCore/Model/{Snapshots,Recovery,
        Templates,BoxUsage,Triggers,Queue 相关}
  保留：SbieCore/DriverApi（枚举/QueryConf/ReloadConf）、SbieCore/SvcClient
        （仅 RunSandboxed）、SbieCore/Util
```

### 12.2 波次（建议 7 波，W1/W5 并行起步，W2/W3 并行）

| 波 | 内容 | 依赖 | 并行面 |
|---|---|---|---|
| W0 | 契约冻结：§12.1 全部头文件签名 + 本文 OPEN 项拍板 | — | 半天，阻塞后续 |
| W1 | 模板引擎三件（TemplateDir/Expander/BoxCache）+ 单测（合并序/环/变量/自检） | W0 | 与 W5 并行 |
| W2 | Registry + AliasIndex + 探测轮询 + ReloadConf 封装（真机验证 §6.3 全链） | W0,W1 接口 | 与 W3 并行 |
| W3 | 公共 monitor（MonitorMain + TaskDir）：单例/对账/边沿/teardown/自愈 | W0 | 与 W2 并行 |
| W4 | 命令面：V2Commands（五命令 + §7 状态机）+ main.cpp `--monitor` + **删除 server/ipcc/V1 命令/SbieCore 死模块**（§12.3 清单）+ 构建脚本收敛 | W1-W3 | — |
| W5 | 模板迁移：按 §11 规则从 Templates.ini 脚本化生成 V2 模板目录（400 文件）+ 变量表 + BoxTypes\Standard.ini | W0 | 与 W1-W4 并行 |
| W6 | 端到端验收：锁状态机全路径（S0-S8）、并发 exec 冲锋、monitor 崩溃自愈、KISS 面巡检（无残留 V1 入口）；docs 更新 | 全部 | — |

### 12.3 W4 删除清单（文件面）

- `sbie-cli/server/`：ServerMain、ServerState、Dispatcher、LogPump、TracePump、
  SvcProxy、Guardian（Guardian 的注销职责由 monitor 承接；OnBoxTerminate/
  OnBoxDelete 触发器、AutoDelete/AutoRemove、`Temp_` 一次性沙盒语义——**砍**，
  不迁移）。
- `sbie-cli/ipcc/`：SbieIpc、TmplHide。
- `sbie-cli/cli/`：ServerConnect.*、Commands/ 全部（box_*/proc_*/cfg_*/template_*/
  log_*/force_*/maint_*/disk_img_*/usb_*/trace_*/doctor_*/server_cmd/Commands_D1/
  Commands_D3/IpcRoute）。
- `SbieCore/Model/`：Snapshots、Recovery、Templates(V1 TemplateRegistry)、
  BoxUsage、RunBoxTriggers；`SbieCore/QueueClient/`（交互队列）。
- `main.cpp` 的 `--start-server/--no-server/--idle-timeout` 接线。
- GUI 不在本波（D5 一句备注：sbie-gui 调用的命令面已变，后续波次适配）。

### 12.4 无 server 模型下明确砍掉的功能（用户可见面）

快照（box snapshot *）、文件恢复（box recover *）、盒复制/导出/导入、
force/maint/img/usb/trace/log/doctor 命令组、交互式队列提示、
OnBoxTerminate/OnBoxDelete 触发器、AutoDelete/AutoRemove、空盒自动清理策略
（monitor 只注销注册，不动盒内容——数据保留是用户目录自己的事）。
未来需要时按 monitor 任务扩展键或独立子命令另行立项。

---

## 13. OPEN 决策点（需用户拍板；已拍板的 D1-D6 不在此列）

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| OPEN-1 | 盒名 = 目录基名，非法名（含空格/点/中文/超长）目录如何处理 | A 报错要求改名；B 允许 alias 派生合法注册名（引入"目录名≠盒名"映射） | **A**（B 破坏 §4.2 唯一性推理链，KISS） |
| OPEN-2 | ImportBox 通配目录位置 | A `%LOCALAPPDATA%`（单用户假设）；B `C:\ProgramData\SandboxieOSS\boxes\<user>_<box>.ini`（多用户共机，需 ACL 放宽给各用户写） | **A 先行**，多用户场景立项时再 B |
| OPEN-3 | sync-config 遇盒运行中 | A 放行+警示（对齐官方 reload 热更语义）；B 拒绝 BOX_BUSY | **A** |
| OPEN-4 | `ps` 无参数行为 | A 列全部 V2 盒+L/T/P 状态；B 报 USAGE | **A**（唯一有用的巡检入口） |
| OPEN-5 | monitor 默认参数 | 250ms / K=2 / grace=5s / empty-ticks=4 | 确认或给出机器规格（CPU 占用可忽略：单 enum IOCTL/tick） |
| OPEN-6 | V2 是否保留 `status`/`--version` 极小查询面 | A 只留 `--version` 旗标；B 加回 `status` 命令（驱动/svc/任务态一览）；C 全无 | **B**（诊断价值高、实现 <50 行，不违 KISS）；严格五命令则 A |
| OPEN-7 | 模板迁移边界 | A §11 全量（400）；B 仅精选常用（如 WebBrowser/Desktop/System ≈250） | **A**（脚本生成成本恒定，全量无维护负担） |
| OPEN-8 | unregister 遇盒忙 | A 拒绝（无 kill 面）；B 附 `--kill` 走 SbieSvc KillAll 后注销 | **A 先行**（B 视使用痛感补） |

---

## 附：与用户规格的条款对照（溯源）

| 用户规格条 | 本文落点 |
|---|---|
| 1 KISS/极简命令面 | §1、§9、D4 |
| 2 SBIE_TEMPLATE_DIR/类别\名字/递归/无内核 IOCTL | §3 |
| 3 盒目录三件套 + sandbox.ini 单盒 KV | §4.1 |
| 4 解析→展开→缓存→ImportBox→启动 流水线 | §5、§6、§7、§9.1 |
| 5 monitor/running.lock/归零注销（纯轮询+公共实例为后续拍板） | §7、§8、D1/D6 |
| 6 五命令 + 相对路径 + *alias + 导出不做 | §9 |
| 7 V1 模板掏空迁移 | §11 |
