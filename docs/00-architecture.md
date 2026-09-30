# 00 — server / client 架构

## 1. 总览

```
┌─────────────────────────────── 一个 WTS 登录会话 (session id N) ──────────────────────────────┐
│                                                                                              │
│  sbie-cli <命令> (client, 短生命周期)          sbie-cli --start-server (server, 常驻)        │
│        │                                             │                                      │
│        │  命名管道 \\.\pipe\SbieOSS_Cli_S<N>           ├─ LoadLibrary("SbieDll.dll")           │
│        └──────────────► (多实例, n 个) ◄──────────────┤    GetProcAddress(SbieApi_*/…)        │
│                                    pipe instances     ├─ NtOpenFile(\Device\SandboxieDriverApi)│
│                                                        │    + NtDeviceIoControlFile (IOCTL)    │
│                                                        ├─ NtConnectPort(\RPC Control\SbieSvcPort)│
│                                                        │    (LPC, MSG_HEADER 分块协议)         │
│                                                        ├─ API_SESSION_LEADER 注册会话领导者    │
│                                                        └─ API_GET_MESSAGE / API_MONITOR_GET2  │
│                                                           日志/trace 泵                      │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
                                   ▲                    ▲
                        SbieDrv.sys (内核)      SbieSvc.exe (系统服务, 每机一个)
```

设计要点（对应现版行为）：

1. **一个登录会话一个 server 实例**。Sandboxie 的驱动按会话组织日志与 leader：
   `Api_GetMessage` 要求"非服务进程只能读取自己作为 leader 的会话的日志"
   （`Sandboxie\core\drv\api.c:725-733`）；`API_SESSION_LEADER` 的 "set leader" 路径要求
   调用进程**不在沙箱内**，且同会话已有 leader 时返回 `STATUS_DEVICE_ALREADY_ATTACHED`
   （`Sandboxie\core\drv\session.c:336-360`，`Session_Api_Leader`）。
   因此 server 必须每会话恰好一个，由它成为该会话的 leader 并垄断日志泵。
2. **client 是短生命周期进程**。一次命令 = 连接管道 → 发一条请求 → 收一条回复 → 退出。
   简单查询（如 `sbie version`）允许 client 在 server 缺席时**降级直连**驱动（详见 §5）。
3. **server 不拥有配置文件的写权**。所有 ini 修改走 SbieSvc 的
   `MSGID_SBIE_INI_*` 通道（`Sandboxie\core\svc\sbieiniserver.cpp:115-289`），由 SbieSvc
   完成"写 ini → SbieApi_ReloadConf 热重载"（`sbieiniserver.cpp` `RefreshConf()`）。
   server 自身只在快照、磁盘扫描等纯文件系统操作上直接动手。

## 2. 登录会话识别（WTS session id）

- `ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)`，失败时回退 0。
  参考 LGPL 实现 `SandboxiePlus\QSbieAPI\SbieAPI.cpp:59-60`（`SSbieAPI` 构造）。
- server 与 client 各自独立取值；**管道名携带 session id**（§3），天然隔离多用户会话。
- 不使用 WTS API（`WTSGetActiveConsoleSessionId` 等）确定"自己"——那回答的是"谁的会话在
  console"，不是"我在哪个会话"。

## 3. 会话命名管道命名规范

```
\\.\pipe\SbieOSS_Cli_S<session_id>          例：\\.\pipe\SbieOSS_Cli_S1
```

- 服务端 `CreateNamedPipeW`，`PIPE_ACCESS_DUPLEX`，`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`，
  `PIPE_WAIT`，首次实例 `FILE_FLAG_FIRST_PIPE_INSTANCE`（抢占式防劫持），实例数上限 16，
  `PIPE_UNLIMITED_INSTANCES` 不足以表达"只有本 server 能创建"。
- DACL：仅允许本会话用户 SID + 本地 SYSTEM（`ConvertStringSecurityDescriptorToSecurityDescriptorW`
  构造 `SDDL`:`D:P(A;;GA;;;OW)` 加 OWNER-RIGHTS 方案；实现时以"owner 完全访问 + SYSTEM 完全访问"
  为准）。命名管道对象本身不按会话命名空间隔离，必须靠名称中的 `S<id>` + DACL 双保险。
- client `CreateFileW` 连接；`ERROR_PIPE_BUSY` 时 `WaitNamedPipeW` 重试。

**为何用命名管道而非 LPC**：sbie-cli 自身的 IPC 无需模拟调用方令牌（鉴权由 server 归并到
"会话 id + 用户 SID"校验），命名管道提供 `GetNamedPipeClientProcessId` /
`GetNamedPipeClientSessionId`（Win32 Vista+），可复刻 SbieSvc `PipeServer` 的调用方识别模型
（`Sandboxie\core\svc\PipeServer.cpp`，`GetCallerProcessId/GetCallerSessionId`）。
与 SbieSvc 之间的 LPC 通信（必须 impersonation）仅发生在 server 进程内，见 `03-svc-protocol.md`。

## 4. server 单实例保证

两道锁，任一失败即放弃启动：

1. **命名互斥体**：`Local\SbieOSS_Server_S<session_id>`（`Local\` 前缀 = 每会话独立命名空间，
   无需 DACL）。`CreateMutexW` 后 `GetLastError()==ERROR_ALREADY_EXISTS` → 已有实例，本进程退出 0。
   注意现版 SandMan/SbieCtrl 用全局互斥体 `Sandboxie_SingleInstanceMutex_Control`
   （`SandboxiePlus\QSbieAPI\SbieAPI.cpp:156-166`，`IsSbieCtrlRunning`）——那是**跨会话**单实例；
   我们的需求是**每会话**单实例，语义不同，勿照抄名字。
2. **管道探测**：尝试以 `FILE_FLAG_FIRST_PIPE_INSTANCE` 创建管道；失败（`ERROR_ACCESS_DENIED`
   或 `ERROR_PIPE_BUSY` 且能连通）→ 已有 server，退出 0。

server 崩溃时互斥体随进程句柄关闭而释放（未释放句柄即 abandoned，`WaitForSingleObject` 返回
`WAIT_ABANDONED`，可安全 acquire），无需额外清理逻辑。

## 5. 首命令自动拉起流程（client 侧）

```
client main()
 ├─ 解析命令行；--no-server 跳到 (E)
 ├─ (A) CreateFileW(\\.\pipe\SbieOSS_Cli_S<N>)  ──成功──► 走管道，结束
 ├─ (B) 失败且 ERROR_FILE_NOT_FOUND（无 server）
 │     ├─ CreateProcessW("…\sbie-cli.exe" --start-server, DETACHED_PROCESS|CREATE_BREAKAWAY_FROM_JOB)
 │     │   工作目录 = sbie-cli.exe 所在目录
 │     ├─ 轮询 CreateFileW，间隔 50ms，上限 3000ms（3s）
 │     │     ├─ 连上 ──► 走管道，结束
 │     │     └─ 超时 ──► (C)
 │     └─ (C) 拉起失败或超时：降级路径
 │           ├─ 若命令为"只读且不需会话状态"（status/version/cfg get/proc list）
 │           │     在 client 进程内直接做一次 SbieDll 动态加载 + 驱动查询，输出结果，
 │           │     并在 stderr 提示 "server 未运行，降级直连"
 │           └─ 否则报错退出（退出码 4 = SERVER_UNAVAILABLE，见 04-modules.md §退出码）
 ├─ (D) ERROR_PIPE_BUSY → WaitNamedPipeW(1000) 后重试一次，再失败按 (C) 处理
 └─ (E) --no-server：永不拉起，直接 (C) 的降级/报错逻辑
```

要点：

- 拉起用 `DETACHED_PROCESS`（server 不继承控制台，避免阻塞脚本）+
  `CREATE_BREAKAWAY_FROM_JOB`（若 client 处于 Job，让 server 逃脱，否则 client 退出时
  Job 收尾可能连杀 server）。
- 竞态：多个 client 同时发现无 server 并同时拉起 → 由 §4 的双锁收敛，后启动者静默退出。
- 降级直连的理由：`sbie status` 这类命令在 SandMan 未装/未跑时也必须可用（对齐现版
  `SbieIni.exe`、`Start.exe` 可独立工作的行为）；只读 SbieApi_* 查询不要求 session leader。

## 6. server 生命周期与空闲退出

状态机（单线程 + I/O 完成端口或 `DisconnectNamedPipe` 轮询线程池，二选一，实现期定）：

```
启动 ──► INIT: 双锁 → 创建管道(首实例) → LoadLibrary(SbieDll.dll)
             → NtOpenFile(驱动设备) → API_SESSION_LEADER(set leader)
             → 广播 READY（写入管道首条应答后即视为就绪）
 │
 ├─ RUNNING: 每个已连接 client 实例一个工作线程
 │     请求帧 {u32 magic 'SBOS', u32 version, u32 msgid, u32 payload_len, u8 payload[]}
 │     回复帧同构，msgid 高位置 0x80000000 表示错误，payload 为 JSON 文本(UTF-8)或空
 │
 ├─ 空闲计时：最后一个 client 断开（ConnectNamedPipe 返回且无新连接）后启动定时器；
 │     默认 IDLE_TIMEOUT = 300s（可由 sbie server start --idle <sec> 配置，>=0，0=不退出）；
 │     期间任何新连接取消定时器。超时 → 主动 API_SESSION_LEADER 不反注册（驱动无此操作，
 │     leader 换人时由新 leader 接管，见坑记录 #1），CloseHandle 全部，进程退出 0。
 │
 ├─ 优雅停机：收到 MSG_SHUTDOWN（仅 `sbie server stop`，校验调用方与 server 同用户同会话）
 │     → 停止接受新连接 → 排空在途请求（上限 10s）→ 退出 0。
 │
 └─ 崩溃：无特别处理，见 §7。
```

server **不**常驻写配置：`cfg set` 类请求由 server 转发 SbieSvc（`MSGID_SBIE_INI_SET_SETTING`
等），server 自身不缓存 ini。

## 7. 崩溃恢复

- **client 视角**：管道读写返回 `ERROR_BROKEN_PIPE` / `ERROR_NO_DATA` → server 已死。
  client 依 §5 重新拉起并**重试一次**当前请求（幂等命令：全部只读命令 + `proc kill`；
  非幂等命令——`box create`、`snapshot take`、`cfg set`——不自动重试，报
  `SERVER_RESTARTED_RETRY_SUGGESTED`，退出码 8）。
- **server 视角（自身崩溃）**：无状态可恢复——所有可变状态在驱动/SbieSvc/ini 里，
  server 是纯代理 + 泵。重启后重新 set leader 即可；唯一丢失的是内存中的日志环形缓冲
  （可选功能，缺省关闭，仅 `sbie log watch` 在线订阅时存在）。
- **leader 遗留**：server 崩溃后驱动 `SESSION->leader_pid` 仍指向死 PID，但新 server 重新
  `API_SESSION_LEADER`（set 路径）时驱动只比较当前 leader_pid 是否等于自己
  （`session.c:349-356`），死 PID ≠ 新 PID 但驱动并不校验旧 PID 存活 → 直接接受新 leader。
  （已核对 `Session_Get(TRUE, -1, &irql)` 路径，见坑记录 #1 的验证结论。）
- **SbieSvc 死亡**：server 的 LPC 端口句柄失效；下次调用按 `03-svc-protocol.md` §分块传输
  的错误路径关闭端口、重连（SbieSvc 是 Windows 服务，SCM 自动重启；重连前轮询
  `OpenService(SbieSvc)` + `QueryServiceStatus` 最多 30s）。

## 8. 并发与线程模型（server）

| 线程 | 职责 |
|---|---|
| 主线程 | 生命周期状态机、空闲计时器、信号处理（无） |
| 管道接受线程 | `ConnectNamedPipe` 循环， spawns 工作线程 |
| 工作线程 ×N（≤8） | 每 client 每请求；转译为 SbieApi_* 调用或 LPC 请求 |
| 泵线程 ×1 | 10ms 起步退避（对齐 QSbieAPI `run()` 的 Idle 递增等待，`SbieAPI.cpp:711-758`）：`API_GET_MESSAGE`（日志）→ `MSGID_QUEUE_GETREQ`（interactive queue，仅 leader）→ `API_MONITOR_GET2`（trace，启用时） |

驱动 IOCTL 天然串行化（`sbieapi.c:142-147` 注释：所有请求同步），泵线程与工作线程可并发
调驱动，无需用户态全局锁；LPC 端口绑定单线程（见 `03-svc-protocol.md` §线程亲和）。

## 9. 坑记录

1. **（已验证）驱动无 "unset leader" API**：`API_SESSION_LEADER` 的 set 路径
   （`session.c:336-360`）只接受/拒绝，无反注册。文档初稿曾写"server 退出前反注册 leader"，
   不成立，已改为"直接退出，新 server 重设"。附带发现：set 路径要求 `proc == NULL`
   （调用进程不在沙箱内），否则 `STATUS_NOT_IMPLEMENTED`——即 server 绝不能运行在沙箱里。
2. **（已验证）`Api_GetMessage` 的会话校验**：`api.c:725-733`，非 SbieSvc 进程只能读
   自己是 leader 的会话。server 必须先 set leader 成功再开日志泵，否则一律
   `STATUS_ACCESS_DENIED`。降级直连的 client（§5）因此**拿不到日志**，`sbie log watch`
   在无 server 时必须先拉起 server 而不是降级。
3. `FILE_FLAG_FIRST_PIPE_INSTANCE` 与 `PIPE_REJECT_REMOTE_CLIENTS` 组合在 XP 兼容头文件下
   无定义——本项目最低支持 Win10（对齐 Sandboxie-Plus v5.73 系统要求），直接使用，不做兼容。
4. **（M2 实测）同步管道句柄不支持跨线程并发读写**：LogPump 线程曾直接向订阅连接
   `WriteFile`，而该连接的工作线程同时阻塞在 `ReadFile`——推送稳定失败
   `ERROR_NO_DATA`(232)，即便客户端在读。同步（非 overlapped）句柄上另一线程的
   读未完成时写即报错。修复：连接的**一切读写收敛到其工作线程**——订阅连接
   （log.watch 后）切到 10ms 轮询模式（`PeekNamedPipe` 查新请求 + 排空
   `pushQueue`），泵线程只入队（`Connection::EnqueuePush`，慢消费者丢最旧，
   上限 256）。若未来需要更低推送延迟，正解是 IOCP/overlapped（对齐 SbieSvc
   `PipeServer`），当前 10ms 粒度够 `log watch` 用。
5. **（M2 实测）会话 leader 被 GUI（SandMan）持有时**：server 的
   `API_SESSION_LEADER`(set) 返回 `STATUS_DEVICE_ALREADY_ATTACHED`(0xC0000038)。
   设计行为：server 继续服务读 op，日志泵禁用（diag 一行），`log.*` op 答
   `SERVER_UNAVAILABLE`(4)。反向注意：我们的 server 一旦成为 leader，后启动的
   SandMan 抢 leader 会失败——测试后应停 server 再启动 GUI。
6. **（M2 实测）唤醒阻塞在 `ConnectNamedPipe` 的 accept 线程**：无 overlapped 时
   无法异步取消，采用**自连即断**（停机路径 `WakeAcceptLoop`：CreateFile 管道 →
   立即 CloseHandle；accept 返回后先查 stop 标志再交付，唤醒连接被直接清理，
   不产生工作线程）。探测型连接（`ProbeRunning`）同路径：worker 按 EOF 收尸，
   开销可忽略。
7. **（M2 实测）多 client 同时拉起的竞态收敛**：并发 5 个 `--start-server`，
   互斥体 + 管道首实例双锁下恰好 1 个存活（后启动者静默退出 0）。
   `CREATE_BREAKAWAY_FROM_JOB` 在 Job 不允许 breakaway 时 CreateProcess 报
   `ERROR_ACCESS_DENIED`——client 拉起路径做了无标志重试回退。
8. **（M2 设计登记）SbieSvc LPC 线程亲和的 server 侧解法**：`server/SvcProxy.cpp`
   专职线程 + 任务队列（`SvcCall(std::function<SbieStatus()>)` 阻塞等待）；
   Dispatcher 内一切触到 `svc::SvcClient` 的 handler（status/version 的
   IniGetVersion、proc.info 的 GetProcInfo、cfg.path 的 IniGetPath）经此串行，
   首次 `NtConnectPort` 落在专职线程上（03 §1）。


## 10. 验收记录（M2 server 波次，2026-09-27）

环境：Win10 x64（session 1），SbieDrv 5.73.5 alive，SbieSvc connected，
SbieDll.dll 同目录。构建：`build_oss.bat`（/W4 /WX Release）0 error 0 warning。

实现清单：`sbie-cli\ipcc\SbieIpc.h/.cpp`（帧编解码 + PipeClient）；
`sbie-cli\server\{ServerMain, ServerState, Dispatcher, LogPump, SvcProxy}`；
`sbie-cli\cli\ServerConnect.h/.cpp`（(A)-(E) 状态机 + `Call` 往返 API）；
`sbie-cli\main.cpp`（`--start-server` 接线 + `--idle-timeout <sec>`）。

1. **手动起 server**：`sbie-cli --start-server --idle-timeout 5`（后台）→
   stderr：`sbie-cli server: ready (session 1, pid 56540, idle 5s, leader 0)`
   （当时 SandMan 持 leader，泵按设计禁用；停 SandMan 后重启 server 得
   `leader 1`，`log dump` 可取出真实驱动日志）。
2. **首命令自动拉起**：无 server 时 `sbie-cli version`：
   ```
   sbie-cli 0.1.0
   driver 5.73.5 (abi 0x57230, alive)
   svc 5.73.5 (abi 0x57230)
   ```
   无降级提示；`sbie-cli.exe --start-server` 进程驻留（DETACHED）。
   `sbie-cli --no-server version` 首行 stderr：
   `server not running - degraded to direct driver connection`，输出其余一致。
3. **IPC 路径与直连一致**（协议级原始管道客户端，帧
   `{"op":…,"params":{}}` → envelope）：`version` / `box.list` / `proc.list` /
   `status` 的 `data` 与 `--no-server --json` 直连输出逐字段一致（例：
   `box.list` 的 `active_procs=3` 与直连相同、`proc.list` 三行 PID/STARTED
   全同）。未实现 op（`box.create`）→
   `{"ok":false,"error":{"code":1,"message":"op 'box.create' not implemented
   in this server build (planned wave)"}}`。`status` 的 server 行：
   `"server":{"running":true,"pid":2800,"uptime_sec":6,"clients":1,
   "idle_remaining_sec":null,"log_pump":true}`。
   （命令层"连上即走 IPC"的切换点在 cli 命令 handler，属接线波次——
   `srvconn::Call(op, params)` 已就绪，见其头注。）
4. **`server status`**：命令注册属接线波次（`sbie server status` 当前为桩，
   退出 4）；等价探测 `srvconn::ProbeRunning` / 管道存在性已实测：server 在跑
   时 `SbieOSS_Cli_S1` 可连通并回 `server.shutdown`，不在跑时
   CreateFile → `ERROR_FILE_NOT_FOUND`。
5. **空闲退出**：`--idle-timeout 5`：连接静止 8s 后进程消失（tasklist 0）。
   `--idle-timeout 0` 不退出（配合 shutdown op 测试）。
6. **优雅停机**：IPC 发 `{"op":"server.shutdown"}` → 回
   `{"ok":true,"data":{"stopping":true}}`，进程 2s 内退出 0。
7. **kill server 后 client 自愈**：taskkill server → 下一条 `sbie-cli version`
   自动重新拉起（(B) 路径重走），命令成功；`--no-server` 则降级直连+提示。
   并发 8 client（16 连接）混合 box/proc list 全部成功，server 存活。
8. **日志泵**：成为 leader 后 `log.dump` 回真实事件（例：
   `{"msg_num":16,"msgid":1090651511,"pid":58896,"time":"05:20:33","text":
   "\\Device\\HarddiskVolume3\\Windows\\System32\\cmd.exe, DefaultBox, …"}`，
   即驱动 1399 进程启动通知）；`log.watch` 订阅后 `log.event` 帧（msgid=0）
   实推到订阅连接（沙箱内起 cmd 触发，10ms 轮询粒度即时送达）。
9. **测试副作用恢复**：验收后 server 已 shutdown，SandMan 已重启并复任 leader。
