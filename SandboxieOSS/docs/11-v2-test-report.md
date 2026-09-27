# 11 — V2 独立压力测试报告（sbie-cli 2.0.0 真机实测）

- 日期：2026-09-28 01:16 – 02:40（本机）
- 被测对象：`C:\Users\Administrator\Documents\sbie\Sandboxie_unbusiness\Installer\SbieOSS_x64\sbie-cli.exe`
  （`sbie-cli 2.0.0` / `driver 5.73.5 (abi 0x57230, alive)`；生产 Sandboxie-Plus 共存环境）
- 测试方：独立压力测试 agent（只测不改；全部真实执行，无臆测结果）
- 每轮完整生命周期 = `exec --detach cmd /c "ping -n 30-60 127.0.0.1"` → `ps` 可见 →
  `kill <PID>` → 等待 monitor 归零自动注销（缓存 ini 消失 + running.lock 消失 +
  task 文件消失 + `ps` 不再列出该盒），上限 45s。
- 各组沙盒 `sandbox.ini` 统一最小配置：`[<basename>] Enabled=y` +
  `BoxNameTitle=<名>_title`（可辨识键）+ `Template=System\COM`（模板引用一条）。

## 环境注记（影响结果解读的三件事）

1. **测试期间有外部活动**：monitor.log 显示另一操作者在 01:17:18–01:20:31
   反复运行名为 `testbox` 的盒（残留 `testbox.dead` 墓碑，终检时随运行目录清除）；
   `sbie-cli.exe` 二进制在 **01:23 被外部替换**（尺寸相同 663040 字节，mtime
   01:14→01:23）。正式分组测试（A–F）全部在 01:23 版二进制上运行，数据内部一致，
   但与实现方交付时点二进制是否一致需实现方自查。
2. **02:00:41.903 生产 SandMan 自行派生第二实例**（`-autorun`，与本文 blocker-2
   的挂起 monitor 相差 40ms，相关性记录、因果未证）。
3. 本机 D:/E: 均为只读光驱、无网络映射盘 → A 组"D:\ 根 / D:\深\层\目\录\链 /
   映射盘"三项以 `C:\v2t_root`、`C:\d1\…\d5\v2t_deep`（6 层深链）、跳过（无映射盘）替代，
   特此注明。

## 通过率总表

| 组 | 内容 | 通过率 | 结论 |
|---|---|---|---|
| A | 任意位置沙盒矩阵（6 位置 × 10 轮 + 各 1 次退出码透传） | **52/60 轮**（86.7%）；透传 6/6 | 4 个位置 10/10；2 个位置被 blocker-1 打断 |
| B | 相对路径全矩阵（15 变体 × 10 轮） | **140/140**（14 个指向盒的变体 10/10；第 15 个为合法错误路径 10/10 正确报错）；错误注入 4 类 × 10 轮 = **40/40** 明确信封 | 通过；但收尾撞出 blocker-2 |
| C | 盒内再 exec（嵌套退出码透传） | **10/10** | 通过 |
| D1 | 同盒 5 并发 exec | 新盒 **5/5**；teardown 窗口内 **~1-2/5**（隔离复测 3 轮：4/15 个 exec 成功） | **blocker-3** |
| D2 | 5 异盒并发 exec × 10 轮 | 9/10 轮 5/5 盒全部注册可见；瞬态多 monitor 4/10 轮；1 轮 1 盒未出现 | 大体通过，带 minor |
| D3 | kill-box 后立即 exec | **10/10** | 通过（S6→S0 重注册路径稳） |
| D4 | exec 中 taskkill monitor → 崩溃恢复 | **10/10** | 通过 |
| E1 | 同盒 exec→kill-box→exec × 20 轮 | **20/20**；缓存目录/任务/monitor/ImportBox 行数恒定 | 通过 |
| E2 | sync-config 热改 × 5 + 运行中 sync | **5/5 + 运行中放行+警示正确** | 通过 |
| F | log 冲突场景（SandMan 持 leader） | **6/6** 报错明确指明占用者 pid | 通过（符合设计） |
| F' | log 正路径（临时停 SandMan 后 dump/-w/--json） | 机制 3/3 可跑；**内容 0/3 可用**；--box 过滤泄漏 | **blocker-4** |

---

## A. 任意位置沙盒矩阵

步骤（每位置）：建目录+最小 sandbox.ini → `exec <win路径> cmd /c "exit 0"`（退出码
透传检查）→ 10 轮完整生命周期（见文首定义）→ 抽验缓存内容。

| 位置 | 盒名 | 轮通过 | 退出码 | 缓存抽验 |
|---|---|---|---|---|
| `C:\Users\Administrator\Downloads\v2t_dl` | v2t_dl | **4/10** | 0 | ClosedClsid×2（System\COM 展开）、FileRootPath=盒目录、零 `Template=` 残留 |
| `C:\v2t_root`（替代 D:\ 根，只读光驱） | v2t_root | **8/10** | 0 | 同上 |
| `C:\d1\d2\d3\d4\d5\v2t_deep`（6 层深链） | v2t_deep | **10/10** | 0 | 同上 |
| `C:\Users\Administrator\Documents\v2t_docs` | v2t_docs | **10/10** | 0 | 同上 |
| `C:\Users\Administrator\Documents\My Boxes\v2t_space`（空格） | v2t_space | **10/10** | 0 | 同上 |
| `C:\沙盒中文测试\层\层\v2t_cn`（中文+深层） | v2t_cn | **10/10** | 0 | 同上 |

失败详情（v2t_dl r5、v2t_root r9 起，同型）——**blocker-1（服务残留 wedge，竞态）**：

```
A1 r5: FAIL no_auto_unregister after 45s (cache=Y lock=Y task=Y)
A1 r6: FAIL exec_detach: sbie-cli: box 'v2t_dl' registration state did not
       settle within 10s (STATE_TIMEOUT)     ← r6-r10 连续同错
```

复现序列（最小化）：
1. `sbie-cli exec <盒> --detach cmd /c "ping -n 30 127.0.0.1"`
2. `sbie-cli kill <用户进程PID>`（kill cmd 或 ping 均可）
3. 观察盒内仅剩 `SandboxieRpcSs.exe` + `SandboxieDcomLaunch.exe`（生产 Plus 目录
   镜像）。多数轮次它们在 1–3s 内自灭（linger 自杀），**偶发竞态下不自灭**：
   v2t_dl 观测存活 >3 分钟、v2t_b0 观测 >20 分钟，直至手工 `kill` RpcSs。
4. wedge 期间：`ps <盒>` 恒 PROCS=2-3（服务计入）；monitor `TeardownBox` 删锁前
   重查 `BoxProcessCount>0` 永远 abort → 永不注销；同时后续 `exec` 落 S3/S4
   "濒死窗口"分支（exec 侧排除 RpcSs/DcomLaunch 判 anyUser=false）→ 循环重探 →
   10s STATE_TIMEOUT → **盒既不能用也永不注销**。

机理（代码锚点）：
- 监视侧计数含自举服务：`SandboxieOSS/SbieCore/Model/V2/`（`BoxProcessCount` 全量
  EnumBoxProcesses，无镜像排除）+ `sbie-cli/monitor/MonitorMain.cpp:79-84`
  （`n > 0` 即 abort teardown）。
- exec 侧恰好排除同一批镜像：`sbie-cli/cli/V2Commands.cpp:255-279`
  （`SandboxieRpcSs.exe`/`SandboxieDcomLaunch.exe` 被剔除后 anyUser=false →
  濒死等待）。**两处判据相反 → 死锁**。
- 上游根因（竞态源头，非 OSS 代码）：`Sandboxie/apps/com/RpcSs/linger.c:387-602`
  —— RpcSs 的"盒空即自杀"是**事件驱动**（`WaitForSingleObject(heventRpcSs)` 被
  进程退出事件唤醒后才重估），且豁免项（5 秒内新进程 `SECONDS(5)`、开窗进程等）
  可令 `terminate_and_stop=FALSE` 回睡；最后一名用户进程的退出事件若被合并/错过，
  服务永久滞留。V2 的 monitor/exec 未对该上游竞态做任何兜底。
- 修复方向（供实现方）：monitor 计数采用与 exec 相同的镜像排除；或 teardown
  路径在"仅剩自举服务且持续 N tick"时主动 KillAll 自举服务；exec 的濒死分支
  对"锁在+任务在+仅服务"状态加服务击杀兜底。

恢复手段（实测有效）：`sbie-cli kill <RpcSs PID>` → 盒塌缩 → monitor 正常 teardown。

## B. 相对路径全矩阵

布局：仓库根 `P=C:\Users\Administrator\Documents\sbie\Sandboxie_unbusiness`；
`P\v2t_rel`；`C:\Users\Administrator\Documents\v2t_up`（= P 的 `../..`）；
盒父 `BP=C:\Users\Administrator\Documents\sbie\v2t_relp`、盒 `BP\v2t_b0`、
`B0\a\b` 两条子链。每变体 10 轮 `exec <相对路径> cmd /c "exit 0"` 取退出码。

| # | cwd | 目标串 | 10 轮结果 |
|---|---|---|---|
| B01/B02/B03 | 仓库根 | `./v2t_rel` / `.\v2t_rel` / 裸 `v2t_rel` | 10/10 ×3 exit=0 |
| B04/B05 | 仓库根 | `../../v2t_up` / `.\./..\../v2t_up` | 10/10 ×2 exit=0 |
| B06/B07/B08 | 盒父 BP | `./v2t_b0` / `.\v2t_b0` / 裸 `v2t_b0` | 10/10 ×3 exit=0 |
| B09 | 盒父 BP | `./.././v2t_b0`（规范化后指向 BP 之兄目录，本身不指向盒） | 10/10 **正确报错** exit=5 |
| B10/B11/B12 | 盒目录内 | `.` / `./` / `.\` | 10/10 ×3 exit=0 |
| B13/B14/B15 | `B0\a\b` | `..\..\` / `../..` / `../../v2t_b0` | 10/10 ×3 exit=0 |

错误注入（各 10 轮，要求信封明确、非 GENERIC）：

| 用例 | 输出（首轮原文） | 退出码 |
|---|---|---|
| 不存在目录 `./v2t_nodir` | `sbie-cli: box directory not found: ./v2t_nodir (NOT_FOUND)` | 5 |
| 无 sandbox.ini 的目录 | `sbie-cli: C:\...\v2t_empty\sandbox.ini not found (not a V2 box directory) (NOT_FOUND)` | 5 |
| 文件而非目录 `./v2t_file` | `sbie-cli: box directory not found: ./v2t_file (NOT_FOUND)` | 5 |
| 仓库根不存在盒 | `sbie-cli: box directory not found: ./no_such_box (NOT_FOUND)` | 5 |

背靠背轮次 stderr 恒见 `recent teardown detected; settling ~7.0-8.0s ...`
（墓碑结算退避，设计内行为，见 minor-3）。

**B 组收尾撞出 blocker-2（monitor 挂起，竞态）**：B15 最后一轮 `exec` 返回 0，
但 v2t_b0 的缓存+task+盒内服务（RpcSs/DcomLaunch）**永不清理**：

- 该轮拉起的 monitor（pid 41184，02:00:41.863 创建）**死在主循环之前**：
  monitor.log 在 18:00:34Z 后无任何该实例记录（无 "acquired leadership"、无
  "monitor started"、无 "task added"）；进程存活 25+ 分钟，1 线程、138 句柄、
  CPU 合计 ~1.4s（短暂活动后阻塞）。
- 危害放大：它持有会话互斥体 → 后续任何 exec 的 `EnsureMonitorRunning` 探测到
  互斥体即认为 monitor 健康（`MonitorMain.cpp:291-298` probe 路径直接 return
  true）→ **不补拉新 monitor** → 之后所有盒都不会自动注销，直至手工 taskkill。
- 手工杀 RpcSs 后盒仍剩 DcomLaunch >1 分钟（blocker-1 的又一实证）；taskkill
  monitor + 手删缓存/task + 一次任意盒 teardown 的 ReloadConf 后 ghost 清除。
- 复现：未能在受控小循环中定点复现（140 轮 B 中出现 1 次）；时间轴与 SandMan
  第二实例派生（02:00:41.903，晚 40ms）强相关。嫌疑点（供实现方排查）：
  `RunMonitor` 在互斥体获取后、首条 MLog 前的阻塞（`SbieApi_SessionLeader` SET
  IOCTL / `LoadSbieDll` / `MLog` 首次 CreateFile 追加），1 线程全阻塞形态。

## C. 盒内再 exec（自盒快速路径）

外层：`sbie-cli exec C:\Users\Administrator\Documents\v2t_inner "<sbie-cli全路径> exec . cmd /c exit 3"`
（单参数含空格=整行透传）。内层 CLI 在沙盒内以自身盒为目标。

- 10/10 轮：外层退出码 = **3**（嵌套透传精确），每轮 ~8s（含墓碑退避）。
- 附加观察：盒内 `exec C:\nonexistent_dir cmd /c exit 7` 同样返回 **7**——盒内
  exec 完全忽略目标串（设计如此，V2Commands.cpp:171-211 InSandbox 分支），符合
  docs/10 附录 3 决策，特此记录为既定行为。

## D. 并发与锁竞态

**D1 同盒 5 并发 exec**（10 轮初测 + 3 轮隔离输出精测）：

- 新盒首轮（无墓碑窗口）：**5/5 exec 全部 rc=0、5 个用户进程全部可见、1 注册 +
  1 monitor 收敛** —— 并发协议本身正确。
- 处于 teardown 墓碑窗口（上一代刚注销，settle ~7s 后同时放行 5 个并发）：
  精测 3 轮分别 2/5、1/5、1/5 成功，失败者输出：

```
sbie-cli: cannot create C:\Users\Administrator\Documents\v2t_d1x\running.lock.tmp (GENERIC)
rc=1
```

  另观测 1 次 `warning: cannot create ...\monitors\v2t_d1x.task.tmp`（AfterStart
  警告路径，该 exec 仍 rc=0，自恢复）。
- 根因（blocker-3）：`SbieCore/Model/V2/V2Common.cpp:252-276
  WriteTextFileAtomic` 以 `CreateFileW(GENERIC_WRITE, 0 /*零共享*/,
  CREATE_ALWAYS)` 打开**固定名** `<path>.tmp`；并发者共享冲突直接失败，而
  `V2Commands.cpp:322-329`（S0 放锁）对 `WriteLock` 失败**立即 Fail 退出**，
  没有走"他人正在注册(S2)→重探"的回退。修复方向：tmp 名加 pid 后缀 / 开
  FILE_SHARE_DELETE / 失败时按 S2 回循环。

**D2 五异盒并发 exec × 10 轮**：9/10 轮 5 盒全部注册可见（cache=5 task=5）；
4/10 轮观测瞬态 monitor 2-3 实例并存（互斥体仲裁 ~2s 内收敛回 1，设计声称
"每会话至多一个"，见 minor-1）；r6 有 1 盒的 exec 超过测试 60s 上限被杀、
无错误输出捕获（1/50 异常，见 minor-2）。

**D3 kill-box → 立即 exec × 10**：10/10，立即 exec 经"等注册消失/墓碑 settle"
路径 rc=0，每循环 ~17s。**D4 exec 中 taskkill monitor × 10**：10/10，monitor
自动重建（mon=1）、exec rc=0、后续归零注销正常——崩溃自愈成立。

## E. 注册/注销时序压力

**E1 同盒 20 轮 exec→kill-box→exec**：20/20 全 rc=0；每轮后 cache/task 计数
恒定收敛；终态 wait_gone ok、monitor 数 0、残留仅 `<box>.dead` 墓碑（设计内）；
ImportBox 行恒 1 条（注：脚本内 grep 因系统 ini 为 UTF-16 计 0，属**测试工具
伪影**；交互式复核 1 条、文件 mtime 自 01:17:18 起未变，特此更正性说明）。

**E2 sync-config 热改 × 5**（以 `OpenFilePath=C:\v2t_probe\*` 有无为探针，盒内
`echo probe > C:\v2t_probe\w.txt` 是否落宿主为判据）：

```
E2 r1: sync=0 mode=open  host_write=Y cache_has_open=1 PASS
E2 r2: sync=0 mode=closed host_write=N cache_has_open=0 PASS   （×5 交替全对）
E2-running: sync_rc=0 out=[warning: box has 4 running process(es);
            reload affects new processes|config re-synced for box 'v2t_sync'|]
```

驱动侧真实生效（写入可见性随配置翻转），运行中放行+警示符合 OPEN-3 拍板。

## F. log 子命令

**冲突场景（生产默认态，SandMan 持 leader）× 6**：`log` / `log -w` / `--box` /
`--pid` / `--json` / 复核 —— 全部：

```
sbie-cli: session leader already held by pid <N> (SandMan or the v2 monitor;
stop it to read logs) (ACCESS_DENIED)     rc=6
```

信封明确、指明占用者 pid（含 `--json` 变体完整 V1 信封）——**符合设计预期**。

**正路径**（临时 `taskkill /IM SandMan.exe`（发现双实例 58000+41460 一并终杀）
→ 无人持 leader → 测毕以 `-autorun` 复启）：

- `log --last 5` rc=0 返回 5 条；`log -w` 10s 收 247 条流；`--json` 10/10 行均为
  `{"seq","msgid","pid","text"}` NDJSON 形状 —— 机制层可跑。
- **内容全部不可读（blocker-4）**：每条 msgid=1090651511（0x41020577）。低 16 位
  =0x0577=**1399**（进程启动消息），高 16 位 0x4102 为未剥除的打包位；格式化
  恒走 SbieDll 兜底串 `err=41020577 ... str1= ... str2=`（`Sandboxie/core/dll/
  support.c:885-906`），字符串区渲染为空 → **没有任何可辨识的 SBIE13xx 事件文本**
  （10s 内两次盒 exec 的启动事件 0 条可读）；`--type 13` 类段过滤因此永不命中；
  `--box v2t_log2` 过滤泄漏 7 条不含盒名的条目（`LogCommand.cpp Matches()` 对
  该类条目判真，机制待查）。`--pid` 过滤未获有效 PID（测试时序缺陷）标记
  **未充分验证**。锚点：驱动侧条目布局 `Sandboxie/core/drv/api.c:770-790`
  （`[session_id 4][process_id 4][error_code 4][strings…]`），读侧
  `sbie-cli/cli/LogCommand.cpp FetchOne/FormatOf/Render`。

## G. 环境恢复终检（对照 01:16 基线）

| 项 | 终态 | 判定 |
|---|---|---|
| `C:\WINDOWS\Sandboxie.ini` | 与基线**字节级一致**（cmp 通过；7 节；ImportBox=0；UTF-16LE+BOM 编码保留） | ✅ |
| `%LOCALAPPDATA%\SandboxieOSS` | 整目录删除（基线时不存在） | ✅ |
| sbie-cli / monitor 进程 | 0 个 | ✅ |
| SbieSvc（68832 Services + 54452 Console，PID 未变）/ SbieDrv | 均 RUNNING，状态未动 | ✅ |
| 生产安装目录 `C:\Program Files\Sandboxie-Plus` | 清单 diff 零 | ✅ |
| 被测安装目录 `Installer\SbieOSS_x64` | 唯一 diff：`sbie-cli.exe` mtime 01:14→01:23（**外部替换**，同尺寸；非本测试写入，见环境注记 1） | ⚠️ 已注明 |
| SandMan | 运行中（pid 133388；基线 58000 于 02:00:41 被系统以 `-autorun` 自行替换，F 组测试临时终杀后已复启并确认稳定 30s+） | ✅（PID 变化已注明） |
| 测试目录（Downloads/Documents/My Boxes/C 根深链/中文链/仓库内/v2t_probe） | 全部清除 | ✅ |
| 生产盒（DefaultBox/New_Box*） | ini 原文保留，未触碰 | ✅ |

**过程事故披露**：02:36:49 本人一条 PowerShell 单行命令因引号转义损坏产生非法
正则，意外将系统 ini 写空（2 字节），02:37:12 立即自 01:16 基线副本字节级还原；
期间 SandMan（133388）存活未重启。该事故未影响任何测试数据（发生在全部测试
结束之后），但暴露本人工具链失误，如实记录。

## Blocker 清单（必须修）

| # | 标题 | 证据/锚点 | 最低复现 |
|---|---|---|---|
| **B1** | **自举服务滞留 → 双判据死锁**：monitor 计数含 RpcSs/DcomLaunch 永不 teardown；exec 濒死分支排除它们永不直启 → 盒不可用且注册永挂（观测 >3min / >20min，直至手工杀服务） | A1 r5、A2 r9 全程记录；`MonitorMain.cpp:79-84` vs `V2Commands.cpp:255-279`；上游竞态 `apps/com/RpcSs/linger.c:387-602` | A 组步骤 1-4（出现率约每 5-9 个 kill 周期 1 次，非确定 → 竞态） |
| **B2** | **monitor 主循环前挂起（持有互斥体）**：任务永不收编、零日志；且令后续 exec 误判"monitor 健康"而不再补拉 → 全会话注销停摆直至手工 taskkill | B15 尾轮残留（cache+task+服务 25min+）；pid 41184 1 线程阻塞；`MonitorMain.cpp:291-298` probe 盲区 | 未定点复现（140 轮 1 次）；时间轴与 SandMan 派生强相关 |
| **B3** | **同盒并发 exec 的 lock.tmp 独占创建竞态**：失败者 rc=1 GENERIC 直接退出，无 S2 回退；teardown 窗口内 5 并发仅 1-2 成功 | `V2Common.cpp:252-276`（零共享 CREATE_ALWAYS 固定 tmp 名）+ `V2Commands.cpp:322-329`；隔离复测 4/15 | 新盒连续两轮 exec 间隔 <10s 时 5 并发 |
| **B4** | **log 输出内容不可读**：msgid 未剥包位（0x41020577≡1399）、字符串区空、--type/--box 过滤失效；正路径形同虚设 | F' 全记录；`api.c:770-790` vs `LogCommand.cpp FetchOne/FormatOf/Render`；`support.c:900` 兜底串 | 停 SandMan 后 `sbie-cli log --last 5` |

## Minor 清单

1. 瞬态多 monitor：5 异盒并发时观测 2-3 实例并存 ~2s（互斥体仲裁收敛），与
   "每会话至多一个"表述存在窗口差（D2，4/10 轮）。
2. D2 r6 异常：1/50 并发盒 exec 超测试 60s 上限被杀且无错误输出（无缓存无 task
   生成）；未复现，疑与 B2/B3 同族。
3. 背靠背 exec 恒定 ~7-8s 墓碑结算退避（设计决策的 UX 代价，量化备案）。
4. `Diag()` 无条件写 stderr（`Output.cpp:103`）——非 `--verbose` 限定，脚本消费
   stdout/stderr 判错时产生噪音（本人 B 组初判受其干扰）。
5. `kill` 被杀子进程后 exec 透传退出码 4（TerminateProcess 语义），文档未言明。
6. 盒内自举服务为生产 Plus 镜像（`C:\Program Files\Sandboxie-Plus\SandboxieRpcSs.exe`
   等）——版本混跑当前 ABI 兼容（5.73.5），记录为部署事实。
7. （环境）测试窗口内存在外部操作者活动（testbox 循环、01:23 二进制替换、
   02:00:41 SandMan 自派生）——建议测试窗口独占机器。

## 结论

单盒串行主链路（exec/ps/kill/kill-box/归零注销/register/sync-config/盒内 exec/
相对路径解析/错误信封）**质量扎实**（B/C/E 组 100%）；但**生命周期鲁棒性不达标**：
B1-B3 三个竞态 blocker 均出现在"正常使用几分钟内"的窗口（kill 单进程、并发启动、
背靠背重启），且失败形态都是"盒永久卡死需手工救援"，与 V2"无 server 自治"目标
直接冲突；log（B4）正路径内容不可用。**建议状态：不予通过（blocked），修复 4 个
blocker 后优先回归 A/D1/F' 三面。**
