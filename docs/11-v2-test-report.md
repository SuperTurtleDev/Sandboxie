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

---

# 复测节 — commit 4067579（构建 03:00:46，2026-09-28 03:04–03:27）

复测范围：仅回归面（4 个原 blocker 专项 + A/D/F 抽样 + R1/R2/R4 + 遗留三项独立
判断），不做全量。被测二进制 mtime 03:00:46（尺寸 706048）。复测窗口内无外部
活动（无 testbox/无二进制替换/无 SandMan 自派生）。

## 1. B 组竞态专项复测

| 项 | 轮次 | 结果 | 判定 |
|---|---|---|---|
| B1 复现序列（exec --detach ping → kill 单 PID → 归零自动注销） | 15 | **14/15**；每轮 ~30s 全自动注销（= 用户进程口径 + 15s KillAll 兜底触发路径），无 STATE_TIMEOUT 连败、无手工救援 | **修复确认** |
| B3 teardown 窗口 5 并发 | 新盒 1 + 窗口 3（=20 exec） | **20/20 rc=0**、每轮 5/5 用户进程可见、cache/task/monitor 收敛 1/1/1、零 lock.tmp 报错 | **修复确认** |
| B2 心跳接管（注入） | 1 次注入 | 挂起 monitor（NtSuspendProcess）→ 心跳 5s 陈旧（>3s 阈）→ 下一次 exec **自动击杀挂起实例（142604）→ 拉起替代（3952）→ rc=0**，替代实例完成后续 teardown | **修复确认（注入验证）** |
| B4 log 正路径 | dump/--type/--box/--pid/--json 各 1；-w ×2 | 内容**全部可读**（真 1399 + 真实字段串，掩码修复）；--type 13 全命中 0 泄漏；--box 7/7 全含盒名（原泄漏消失）；--pid 单一 pid 精确；--json 合法 NDJSON（msgid+raw_id 双字段）。**但 `-w` follow 死循环：2/2 复现 follower 存活 14s 投递 0 条新事件**（事后 --box 转储证明环内当时新增 7 条 v2t_w2 事件未被投递；dump 段正常） | **部分修复：-w follow 不工作（新发现 N2）** |

## 2. 回归面

| 项 | 结果 |
|---|---|
| A 组抽 2 位置 × 5 轮（Downloads + 中文深链） | **10/10**（每轮 ~30s KillAll 兜底路径） |
| D1 ×1（新盒 5 并发） | 5/5 rc=0、5 进程、monitor=1 |
| D4 ×1（taskkill monitor → 恢复） | 下一 exec rc=0、monitor 重建（78492）、teardown 正常 |
| F' 冲突 ×1（monitor 持 leader） | rc=6 并指明占用者 pid 93192（文案正确） |
| R1 exec 即退 | `exec cmd /c "exit 7"` 默认 **rc=0**（不等待不透传）✓ |
| R2 --wait 透传 | `--wait` + exit7/exit3 → **rc=7 / rc=3** 精确透传；`--settle 30` 正常；旧式 `--wait 5` 干净报 USAGE(2) "unknown command: 5"（语义变化已文档化）✓ |
| R4 --password/SBIE_PASS/info | `--password pw` ✓、`SBIE_PASS=pw` ✓、缺值 USAGE=2 ✓；`info` 表格（cli/driver/sbiesvc/main ini/ImportBox present/monitor healthy + 盒表）与 `info --json`（合法对象，cli_version/monitor 字段在）双轨 ✓。注：真实认证拒绝路径未验证（本机生产 SbieSvc 未设 EditPassword，不应为测试去改生产服务配置） |

## 3. 新发现（本次复测撞出，非原 blocker 回潮）

- **N1（确定性 3/3，major）净 ini 首次 exec 必败**：
  ```
  sbie-cli: registration not visible after reload: ImportBox line MISSING;
  cache rolled back (10s) (GENERIC)     rc=1
  ```
  复现：删除 `[GlobalSettings] ImportBox=` 行 → 全新盒第一次 `exec` 必失败；
  第二次 `exec` 成功（行此时已写入）。3/3 确定性。首次部署（EnsureImportBoxLine
  → ADD_SETTING → ReloadConf → 探测）存在一次写入-可见性时序缺陷；实现方 8/8
  自测未覆盖（其基线已部署 ImportBox 行）。影响：全新机器/净 ini 的**第一次
  exec 体验必失败**，自愈于重试。
- **N2（2/2 复现，major）`log -w` follow 不投递**：follower 存活但 dump 段后
  0 条新事件（14s 窗口内两次盒 exec 的事件事后在环内可见）。伴随现象：部分
  条目尾部字符串被截断（`…e"`、`…xieRpcSs.exe"`——环缓冲读取边界疑似与
  follow 死点同源）。-w 在本次复测范围内为显式测试项 → 计 FAIL。

## 4. 实现方遗留三项的独立判断

1. **R3 tty 分支**：本环境无交互 tty 且生产 SbieSvc 未设 EditPassword，认证
   提示路径不可触发（非 tty 快速失败文案在代码中存在，V2Commands.cpp:92-160，
   静态确认）。**标注：待人工**（需真实控制台 + 设密服务）。
2. **log 文案兜底可读性**：可读性达标——真 msgid + 字段串管道拼接，未知
   msgid 时回退 `SBIE<id> <strings>` 纯文本；轻微瑕疵为尾部截断串（见 N2）。
   **可接受（带备注）**。
3. **-w 长跑内存备案**：代码面 `FormatOf` 按 msgid 缓存格式串（无 free 但
   有界：不同 msgid 数百级 ≈ 百 KB），follow 循环每条目临时分配即释放，无可见
   无界增长；14s 实测无异常。**备案设计可接受**——但在 -w follow 修复前该项
   实际不生效。

## 5. 环境恢复确认（对照复测起始快照 baseline2）

| 项 | 终态 |
|---|---|
| `C:\WINDOWS\Sandboxie.ini` | 与 baseline2 **字节级一致**（cmp 通过；ImportBox=0；注：本人清理脚本曾给每行多写一个 \r，已按基线字节级还原） |
| `%LOCALAPPDATA%\SandboxieOSS` | 整目录删除（复测起始亦不存在） |
| sbie-cli/monitor 进程 | 0 |
| SbieSvc / SbieDrv | RUNNING，未动 |
| SandMan | 未运行（= 复测起始状态；实现方测试遗留的停止态，如需常驻请自行 `-autorun` 拉起） |
| 生产/被测安装目录 | 双双 diff 零 |
| 测试目录（含中文深链） | 全部清除 |

## 6. 复测总结论

**四个原 blocker（B1/B2/B3/B4 核心）全部修复确认**（B1 14/15 压测 + KillAll
兜底实测触发、B2 注入验证接管、B3 20/20、B4 内容/过滤/dump/json 全对）；
回归面（A/D/F/R1/R2/R4）全部通过；遗留三项判断如上。

但复测范围内出现 **两个新 major**：N1 净 ini 首次 exec 确定性失败（3/3）、
N2 `log -w` follow 不投递（2/2）——后者为本次显式测试项。二者改动面均小
（N1=EnsureImportBoxLine 写入后可见性探测时序；N2=FetchOne 序列推进/环读取
死点）。

**最终结论：FAIL → 退回**（修 N1+N2 后仅需针对这两点微复测，无需全量）。

---

# 复测节·第二轮 — commit 4c20782（构建 04:00:20，2026-09-28 04:05–04:08）

微复测范围：仅 N1/N2 两点 + 附带加固（濒死 wedge 自愈）1 轮。被测二进制 mtime
04:00:20（706048 B）。复测窗口无外部活动。基线快照 baseline3（起始即净：无
运行目录、ini 无 ImportBox、SandMan 未运行、SbieSvc/SbieDrv RUNNING）。

## T1 — N1 净 ini 首次 exec（≥5 轮）

方法（保证驱动真同步，非仅文件层）：`register` 一次性盒 → 删 ini ImportBox 行
→ **`unregister` 该盒（此路径只 ReloadConf、不写 ImportBox 行）** → 驱动重读
ini 后行真正消失 → 全新盒首次 `exec` 验证。

```
T1 r1: first_exec_rc=0 dur=0s importbox_in_ini=1 out=[started pid ... in box v2t_n1]
…（r2–r5 同型）
T1: 5/5 直接成功，ImportBox 行每轮自动重部署
```

**N1 = PASS（5/5，确定性场景消除；写后校验回读 + 驱动侧重载重试按设计生效）。**

## T2 — N2 dump 路径回归（B4 面不回退）

盒生命周期归零、leadership 空闲后测：

| 子项 | 结果 |
|---|---|
| `log --last 10` | rc=0 ✓ |
| `log --type 13 --last 30` | rc=0，非 13xx 行 0 ✓ |
| `log --box v2t_n2b --last 40` | rc=0，7 行全部含盒名，0 泄漏 ✓ |
| `log --pid <盒内PID> --last 80` | rc=0，distinct pid 唯一且正确 ✓ |
| `log --json --last 5` | rc=0，全 `{…}` NDJSON ✓ |
| `skipped N oversized` 诊断 | 本轮环内未触发（计数为 0 时不打印——条件诊断，代码文案合理；未获实证样本，如实注明） |

**N2 dump 面 = PASS（B4 无回退）。**

## T3 — `-w` 行为

- **T3a 快进语义：PASS** —— 启动 4s 时输出仅 2 行（`following (Ctrl+C to
  stop)` + 一条调试行），**不再回放历史整环**（修复前首启即 dump 242 行），
  直接进入跟随态。
- **T3b 等待中新增：残留仍在（如实记录，标注"已知残留待方案拍板"）**——
  follower 存活（Y），12s 内投递 **0** 条新事件；事后 `--box` 转储证明环内同
  窗口新增 ≥3 条本盒事件。取证（实现方新增的 V2LOGDBG 调试面，有效）：

  ```
  follow rc=0x8000001A seq=5981      ← 唯一一条且不再变化
  ```

  `0x8000001A = STATUS_NO_MORE_ENTRIES`（win32_ntddk.h:49；api.c:759 在
  `log_buffer_get_next` 返回 NULL 时置此）——**seq 冻结在 5981 且驱动对
  后续新条目持续返回"无更多"**，实现方"内核侧游标行为"的怀疑被实测证实。
  驱动冻结（5.73.5）无法内核级取证；替代方案（经 SbieSvc `session_id=-1`
  中继）交用户拍板，不阻塞评审。

## T4 — 附带加固：exec 濒死窗口自愈（1 轮）

场景构造：盒运行中 → taskkill monitor（残留 stale cache+task+lock）→
kill-box（进程全灭且无人 teardown）→ 立即 `exec`。

```
T4: death-window exec rc=0 dur=6s（新 monitor 49780 收编 stale task →
    teardown → S0 重注册 → spawn）；终态 teardown ok
```

**自愈加固 = PASS**（该场景在修复前为 STATE_TIMEOUT 死等 wedge）。

## 环境恢复确认（对照 baseline3）

ini 与 baseline3 **字节级一致**（ImportBox=0；本轮清理复用基线副本还原，无上
轮的 \r 事故）；`%LOCALAPPDATA%\SandboxieOSS` 整目录删除；无 sbie-cli/monitor
进程；SbieSvc/SbieDrv RUNNING 未动；生产/被测安装目录双双 diff 零；测试目录全
清；SandMan 未运行（= 复测起始态）。

## 最终判定：达到"进入挑刺评审"门槛（PASS）

依据：
1. 四个原 blocker（B1/B2/B3/B4 核心）经上一轮压测/注入验证全部修复确认；
2. N1 确定性首跑失败已消除（本轮 5/5，驱动真同步场景）；
3. N2 主消费路径（dump + 过滤 + JSON）零回退，`-w` 已收敛为**单一、边界清晰、
   取证完整**的残留（内核游标 STATUS_NO_MORE_ENTRIES 冻结；快进 tail 语义与
   follower 存活行为正确），且备选方案（SbieSvc 中继）已成型待拍板——按
   "残留备案不阻塞"的交付约定，不再构成退回理由；
4. 附带加固（濒死自愈）1/1 验证通过。

**带入挑刺评审的清单**：① `-w` 等待中新增残留（待 SbieSvc 中继方案拍板，
建议作为评审首项）；② R3 tty 交互认证仍待人工（需真 tty + 设密服务）；
③ 小疵：LogCommand 快进上限注释写"≤750ms"而代码为 5000ms（注释/实现不一）；
oversized 诊断文案未经实证样本（本轮环内无巨型条目）；④ 上轮 minor 清单中
未随本轮处理的项（Diag 无条件 stderr、瞬态多 monitor 窗口差等）顺延。

---

# 挑刺评审节 — V2 全量（4067579+4c20782 累积差异；评审于 4c20782 之后）

- 评审对象：`sbie-cli\**` 与 `SbieCore\` 的 V2 相关部分（两 commit 累积 diff：
  +5488/-22937 行，104 文件）；对照 docs\10（设计/决策）、docs\02/03（冻结接口）。
- 评审面：①代码不规范 ②注释风格 ③过度实现（KISS）④忽视 SVC/驱动接口。
- 全部修复经 `cmd //c build_oss.bat`（/W4 /WX）构建 0 error 验证（BUILD SUCCEEDED）。
- 冲突核对：SBIE_INI 写路径/进程操作/IOCTL 旗标姿势逐点核对 docs\02/03 与
  vendor 驱动源码，未发现"绕开 SbieSvc 自造轮子"类接口误用（详见"接口面核对"）。

## Blocker 修复清单（已修 + 构建验证）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| F1 | `sbie-cli/cli/V2Commands.cpp:300-310` | **exec S3/S4 濒死判据与 monitor 分叉**（B1"单一判据"违背复发）：手写枚举循环只排除 RpcSs/DcomLaunch 两个镜像（`BoxUserProcessCount` 排除五个），且 `QueryProcessById` 瞬态失败按"非用户进程"处理（V2Common 同场景按"用户进程"保守计）——两处口径漂移正是 docs\11 blocker-1 双判据死锁的同类隐患（仅剩 BITS/WUAU/Crypto 滞留时 exec 会误判 anyUser=true 直启） | 改调 `BoxUserProcessCount`（枚举失败也保守视为有用户进程），与 monitor 归零判定同源 |
| F2 | `sbie-cli/cli/V2Commands.cpp`（exec 尾部 + 盒内分支） | **`--wait` 退出码 TOCTOU**：先 `CloseHandle(rr.hProcess)` 再按 pid `OpenProcess` 重开——子进程在两步之间退出则句柄取不到，退出码恒回 0（真实码丢失）；且重 spawn 成功路径（早夭自愈分支的两个 return）**泄漏 rr.hProcess** | 全部路径直接等待已持有句柄后再关闭；respawn 分支补关句柄 |
| F3 | `SbieCore/Model/V2/V2Registry.cpp:45-58` | **AliasMutex 超时后假持有**：`WaitForSingleObject` 返回值被丢弃，`held()` 只看句柄非空——3s 超时后无锁读-改-写 aliases.json，且析构在未持有的互斥体上 `ReleaseMutex` | 记录等待结果，`held()` 按 `WAIT_OBJECT_0/WAIT_ABANDONED` 判定 |
| F4 | `sbie-cli/cli/LogCommand.cpp:75-88, 278` | **带入项③之一 + 同类**：快进注释"≤750ms"与代码 5000ms 矛盾；上一段 N2 注释仍在描述**已删除的**"连续 3 次 TOO_SMALL → seq+1 强制解卡"机制与"缓冲扩至 64KB"（实际 32760 WCHAR/65520B）——历史修复叙事残留、与现行三态逻辑自相矛盾 | 注释重写为现行行为（两层真身 + ≤5s 快进） |
| F5 | `SbieCore/SvcClient/SvcClient.{h,cpp}`（-438 行） | **KISS 红线·死代码**：ImBox/MountManager 五函数组（~190 行）零调用方——V1 img 命令组在本 diff 中删除后成孤儿；且 docs\03 §7 明文 MSGID_IMBOX_\* "许可证禁用，不做"。另 IniGetUser/IniGetSetting/TestPassword/SuspendResume/SuspendResumeAll/GetProcInfo 六个便捷函数同样零调用方（docs\10 §12.1 冻结面="仅 RunSandboxed"+附录 3/4 扩展） | 全部删除；头注释从 V1"给 server 波次 agent"叙事改为 V2 实际调用面清单（vendor wire 头保留不动） |
| F6 | `sbie-cli/cli/V2Commands.cpp:544` + `sbie-cli/monitor/MonitorMain.cpp:107` | **只写不读的 `<box>.dead` 墓碑协议**：R1 改反应式自愈时删除了唯一的读取方（S0 结算退避），两处写入与"供下一个 exec 读取"的过期注释成为 write-only 死协议，且 monitors\ 目录无限累积墓碑 | 删除两处写入与过期注释，留一行说明指向现行机制 |
| F7 | `SbieCore/Model/V2/V2Cache.cpp:90-110` | **`DeleteBoxCache` 的 tmp 清扫已死**：仍删固定名 `<box>.ini.tmp`——B3 修复后 `WriteTextFileAtomic` 改用 `<path>.<pid>.<tick>.tmp` 唯一名，旧名永不存在；docs\10 §6.2 设计的"顺手清 >10min 的 \*.tmp 残骸"实际未实现（崩溃残骸永不清） | 改为 `FindFirstFileW(<box>.ini.*.tmp)` 模式清扫，mtime>10min 才删（不碰并发写者在途文件） |
| F8 | `SbieCore/Model/V2/V2Template.cpp:96-110` | **类别扫描漏过 "."/".."**：裸名模板解析枚举 `<root>\*` 时未排除 `.`/`..` 目录项——`root\.\name.ini`（根级文件）与 `root\..\name.ini`（**模板根的上级目录**）会被当作合法候选，违背 §3.1"类别=一级子目录"且把解析面扩到根外 | 枚举循环跳过 `.`/`..` |
| F9 | `sbie-cli/monitor/MonitorMain.cpp:31-42` | **MLog 死脚手架**：`static HANDLE sFile` 永不保持非空（每行日志重复尺寸检查），超限时先 CREATE_ALWAYS 截断创建再 DeleteFileW 再 OPEN_ALWAYS 重建——三步冗余且静态句柄具误导性 | 化简为"超 1MB 直接 DeleteFileW，追加打开自会重建"（行为等价） |
| F10 | `SbieCore/Model/V2/V2Cache.cpp`（ValidateCacheFile/WriteBoxCache）+ `V2Registry.cpp:300` | **退出码 12（CACHE_INVALID）为死码**：Status.h 定义、StatusName 有名、`--help` 文案承诺"12 cache/registration failure"、docs\10 §9.2 冻结表有此码——但全树无一处产生（自检失败报 7、缓存写失败报 1），契约三处承诺落空 | 缓存自检/缓存写失败改报 CACHE_INVALID(12)；reload NTSTATUS 失败保留原 NTSTATUS 映射（更精确，见 style-S9） |
| F11 | `sbie-cli/cli/Cli.cpp:149-150` + `Cli.h`/`Commands.h`/`V2Commands.cpp` 头注释 | **过期注释与代码矛盾（同类清扫）**：Cli.cpp 沙箱自检处两行重复注释（首行"CLI 不得在沙箱内运行"与次行"仅 exec 允许"直接矛盾）；Cli.h/Commands.h/V2Commands.cpp 仍写"命令面仅五命令"（附录 2/4 后实为 9） | 删矛盾行、更新为实际命令面 |
| F12 | `SbieCore/Model/V2/V2Common.cpp:332` | 命名不规范：`kBootStrapImages` → `kBootstrapImages`（拼写） | 改正 |

## Style 清单（建议，未改或仅记录）

- S1 `Cli.cpp:165-171` + `Commands.cpp`：命令注册表仍是 V1 的两级 map
  （command→sub→handler，sub 恒空串）+ `kV2Commands` 数组二次抄写同一清单
  ——平铺命令面单层 map 即可，新增命令要改两处（V1 残留框架，建议下轮收敛）。
- S2 `Route()` 以 -1/-2 哨兵混入命令退出码值域（`Run` 再翻译）——可读性味。
- S3 `V2Commands.cpp EnsureImportBoxLine 调用侧`："我们的 boxes 目录是否在
  ImportBox 值列表"的 6 行判定在 V2Registry×2 / InfoCommand / RegisterBox 诊断
  共 4 处重复，可提一个 `ImportBoxLineVisible()` helper。
- S4 `docs/10 §9.1`"alias 缺省 = 盒名小写"未实现（`register PATH` 不带 alias
  参数时不建默认别名）——实现是合理简化（别名纯显式），但与冻结设计文档矛盾，
  建议 docs\10 补一句偏差记录（本次未改行为，避免影响已测行为面）。
- S5 `docs/10 §4.5` aliases.json 示例为对象映射形态，实现为
  `{"aliases":[…]}` 数组形态（理由注释在 `V2Registry.cpp LoadAliasesNoLock`），
  决策散在代码而非 docs——建议 docs\10 附录补记（形态差异对消费者不可见，纯文档事）。
- S6 `V2Template.cpp ResolveTmplVar` 深度上限 4、`TemplateRoots` env 缓冲 2048、
  `NormalizeDirPath` 缓冲 MAX_PATH\*2——均为静默截断/失败边界，量级无害，记录在案。
- S7 `TemplateMigrate.cpp WriteUtf8File` 与 `WriteTextFileAtomic` 近重复
  （无 BOM、非原子）——一次性迁移工具可接受。
- S8 `TemplateMigrate.cpp:12` 注释笔误"Maxthus2"（应为 Maxthon2）。
- S9 退出码分类学残留：`RegisterBox` 的"registration not visible after reload
  （10s）"仍报 GENERIC(1)——按 docs\10 §9.2 字面更像 STATE_TIMEOUT(10)；因该
  路径 N1 修复后实际不可达且测试已记录 rc=1 信封，未动，记录备查。
- S10 `CmdPs`/`CmdKillBox` 对多余参数静默忽略（不报 USAGE）。

### 隐藏功能/内部旗标判定（评审面③专项）

- **`--set-password` / `--ini-del`：判定 = 留**。理由：docs\10 附录 4 已
  备案（R2/R3 测试与运维用）；全部经 SbieSvc 官方通道（SET_PASSWORD /
  DEL_SETTING），非旁路；各 ~20 行、main.cpp 截获不进命令注册表；无它们则
  EditPassword 机器上 ImportBox 行部署的手工指引无法机内执行。属"有备案的
  最小运维面"，不违 KISS。
- **`V2LOGDBG`：判定 = 留**（见 note-N4，-w 残留问题的取证面）。
- **`--monitor` 调参四旗标（--poll-ms 等）：判定 = 留**——docs\10 §8.1 规格
  内（手动调参调试），OPEN-5 拍板项。
- **`--sbie-dll-path`：判定 = 留**——docs\02 §1 加载规范的一部分（显式路径
  解析依赖），测试面亦在用。
- 未发现清单外的隐藏入口（`--migrate-templates` 有 M5 备案）。

## Note 清单（记录，不动）

- N1 `V2Template g_rootsOverride` 全局可变无锁——CLI/工具均单线程使用，安全；
  若未来多线程化需先收口。
- N2 monitor 就绪事件：旧实例死亡到新实例创建之间，若有 exec 持有打开的事件
  句柄，`readyOrWait` 可能读到残留 signaled 状态（窗口极窄，B2 心跳判定已兜底）。
- N3 `EnsureMonitorRunning`：挂起 monitor 的 status 文件损坏（pid 读不出）时
  不会击杀、直接拉新实例（新实例 2s 互斥体超时退出）→ 该会话仍停摆；触发条件
  为"挂起+文件损坏"双重小概率，维持现状。
- N4 `V2LOGDBG` 隐藏调试环境变量（follow 循环 rc 取证）：保留——-w 残留问题
  的唯一取证面，测试 agent 实际使用过；已在此备案。
- N5 USHORT/WORD 强转全扫：仅 SvcClient 的 PORT_MESSAGE 字段（≤288B，固有
  USHORT 位宽）与常量端口名——无 N2 同类截断隐患。
- N6 `FileReadAll` 对奇数字节的 UTF-16LE 静默截尾、`ParseIniFile` 容忍节头
  行尾杂字符（"[a]b]"取"a"）——解析器宽松边界，无实际危害。
- N7 `LockCreatorDead` 以 `ERROR_INVALID_PARAMETER` 近似"PID 从未存在"——
  注释已声明近似语义，仅诊断用途。

## 接口面核对（评审面④，逐点过堂结论）

逐点核对 docs\02/03 与 vendor 源码，**未发现需要修复的接口误用**：

1. **ini 写路径**：ImportBox 行部署走 `SbieSvc MSGID_SBIE_INI ADD_SETTING`
   （EnsureImportBoxLine），组包对齐 `CheckRequest`（h.length 下限/value 尾零/
   password≤64 显式校验）——正路，无用户态三层探测复辟。
2. **进程操作**：kill/kill-box → `MSGID_PROCESS_KILL_ONE/ALL`；exec →
   `MSGID_PROCESS_RUN_SANDBOXED`（env 继承、变长区 ofs/len 布局对齐 QSbieAPI、
   hThread 即收 hProcess 交调用方）；无自造 IOCTL 杀进程。
3. **IOCTL 旗标**：注册探测 `QueryConf(NO_GLOBAL|NO_TEMPLS|NO_EXPAND)` 与
   docs\10 §6.4 一致；GlobalSettings\ImportBox 枚举不带 NO_GLOBAL（本节即全局
   节）正确；`ReloadConf((ULONG)-1, 0)` 符合 docs\02 §3.4。
4. **LPC 线程亲和**：全部请求经 SvcClient 单 mutex 串行（CLI/monitor 均单
   线程调用），满足 docs\03 §1；分块收发/序号机制与 §2 逐条对齐。
5. **GetMessage/领导权**：先查后设姿势正确（set 传 (0,nullptr) 对齐封装版
   语义，docs\02 坑 2）；缓冲 65520B 避开 USHORT 截断。
6. **挂载/IMBOX**：无任何调用（死客户端已删，见 F5）。
7. **冻结组件语义**：ReloadConf 拒绝盒内调用者已在 Cli.cpp 自检正确处理
   （仅 exec 放行，走"自身盒"快速路径，CallerInSandbox 语义）。

## 带入项结论

### ① `-w` 等待中新增残留 → SbieSvc session_id=-1 中继方案：**搁置（不建议采纳）**

实读冻结驱动源码定论（`Sandboxie/core/drv/api.c:721-735` + `log_buff.c:120-141`）：

- "服务特权"= `PsGetCurrentProcessId() == Api_ServiceProcessId`（且
  `session_id==-1` 收全部会话条目）——**该特权只属于 SbieSvc 进程自身**，
  非任何客户端可借用的"通道"；非服务调用者只能读"自己是 leader 的本会话"。
- 冻结 SbieSvc 协议（msgids.h 全量盘点）**不存在任何日志中继 MSGID**
  （0x1100-0x1F00 全组核对）——"经 SbieSvc 中继"必然要求修改冻结的 SbieSvc
  （新增 handler），违背"冻结组件不可改"约束。工作量估计因此无意义
  （若约束解除：SbieSvc 新 handler + 泵 + 客户端 ≈ 2-3 天 + 回归，但即属
  改冻结件，需用户先改政策而非改代码）。
- 根因定性（供后续拍板）：follower 死点是 `log_buffer_get_next` 的"最新条
  ==游标 → 空"判定与会话过滤跳过（`continue` 丢弃步进进度、`*msg_num` 仅在
  Delivered/TOO_SMALL 时写回）的组合——游标可滞留在环已弹出水位之下，
  客户端对同一游标重试永远得到 NO_MORE_ENTRIES。
- **更廉价的客户端侧备选（建议采纳方向，待拍板后实施）**：饥饿重锚——follower
  连续 N 次 NO_MORE_ENTRIES 后把游标重置为 0 重扫（`get_next(0)` 对陈旧游标
  会退回"返回最旧条目"分支，这正是客户端唯一能撬动游标的杠杆），按 seq 去重
  补投递；LogCommand 内约 20 行，不动任何冻结组件。当前交付维持"-w 备案残留
  + dump 路径可用"现状。

### ② R3 tty 交互认证：**实现完备，达到可人工验证状态**

`V2Commands.cpp ReadPasswordFromTty/RegisterBoxWithAuth` 链路逐项核对：
`GetConsoleMode` 探测 tty（管道/重定向正确落非 tty 失败分支）✓、回显关闭并在
失败路径同样恢复模式 ✓、CR/LF 剥离与空密码拒绝 ✓、重试恰一次 ✓、触发条件
WRONG_PASSWORD→ACCESS_DENIED 折叠（Status.cpp:43 实读）✓、非 tty 失败信封带
`--password`/`SBIE_PASS` 指引 ✓。维持"待人工"（需真实控制台 + 设密 SbieSvc），
无代码缺口。

### ③ 两小疵：**均已处理**

- 750ms/5000ms 注释矛盾：已修（F4，`LogCommand.cpp:278`，注释改"≤5s"并引
  docs\10 附录 5）。
- oversized 诊断无实证样本：代码面复核——计数>0 才打印（条件诊断）、文案
  "(>64KB strings)"与 65520B 缓冲语义相符，**代码无需改**；样本缺口属环境
  事实（本轮环内无巨型条目），如实维持"未经实证"备案，不视为缺陷。

### ④ 上轮 minor 逐项判定

| 项 | 判定 | 理由 |
|---|---|---|
| minor-4 `Diag()` 无条件 stderr | **留** | 调用面全部是异常事件告警（teardown 警告/早夭自愈/monitor 拉起失败），stderr 告警是 CLI 正道；噪音敏感处（"following…"）已判 `!quiet`。测试方受扰的根因是 B 组墓碑告警高频出现，F6/R1 后该告警源已消失 |
| minor-1 瞬态多 monitor（2-3 实例 ~2s） | **留** | CreateMutex+2s 等待的接管窗口固有代价，落败实例静默退出 0、无副作用；B2 心跳机制已覆盖"挂起实例"真危害面。属设计内行为，与"每会话至多一个"表述的窗口差建议 docs\10 一句话备注 |
| minor-2 D2 r6 单发超时 | **闭环** | 疑因族（B2 挂起/B3 tmp 独占）均已修复且复测通过；未复现，不再追踪 |
| minor-3 背靠背 ~7-8s 墓碑退避 | **留** | R1 已把守卫从阻塞改为反应式（exec 立即返回），残余代价仅早夭重 spawn 的 4s 结算等待，是上游注入结算期的物理约束 |
| minor-5 kill 后 exec 透传 4 未文档化 | **备案** | TerminateProcess 语义（被杀子进程退出码=4）；kill 命令输出已提示"may trigger auto-unregister"，此处补记于本节即视为文档化 |
| minor-6 盒内自举服务为生产 Plus 镜像 | **留** | 部署事实（同 ABI 5.73.5）；OSS dist 自带服务镜像属打包议题，非代码问题 |
| minor-7 测试窗口外部活动 | **不适用** | 评审窗口无外部活动 |

## 评审结论

四类评审面过堂完毕：**blocker 12 项全部修复并构建验证**（其中代码行为修复
F1/F2/F3/F7/F8 各有真实 bug 风险；F5/F6 为 KISS 红线死代码清除 -438 行）；
style 10 项、note 7 项记录在案；SVC/驱动接口面零误用。带入四项：①中继方案
搁置（附驱动源码级依据 + 更廉价的客户端重锚备选）、②tty 链路完备待人工、
③两小疵闭环、④minor 逐项判定如上。

**V2 代码面达到交付标准**；遗留事项均属"拍板类"（-w 后续方案、docs\10 两处
文档偏差补记）而非代码缺陷。


---

# 加密盒复测节 — commit fcd69f3（构建 14:50:20，2026-09-28 14:54–15:11）

范围：加密盒专项 + 模板面回归 + 明文盒快速回归。基线快照 baseline4（起始：
SandMan 运行中、无运行目录、ini 无 ImportBox、SbieSvc/SbieDrv RUNNING）。复测
窗口无外部活动。

## E1 — 加密盒专项（4/4 位置×模式全周期 PASS）

| 轮 | 场景 | create | exec(挂载) | 二次 exec(已挂载,写) | 宿主读回 | teardown |
|---|---|---|---|---|---|---|
| r1 | 新目录 + `--mount-password` | rc=0 | rc=0 | rc=0 | `encmark4110` 精确 | junction 摘除 + data.box 保留 + waitgone ok |
| r2b | 父目录含空格（`Enc Boxes\`） | rc=0 | rc=0 | rc=0 | `encmarksp19210` | 同上 ok |
| r3 | 相对路径（create 与 exec 均裸名） | rc=0 | rc=0 | rc=0 | `encmark7x21500` | 同上 ok |
| r4 | `SBIE_BOX_PASSWORD` 环境变量路径（无旗标） | rc=0 | rc=0 | rc=0 | `encmark18634` | 同上 ok |

- 容器工件：4/4 均为 **268435456 B（256MiB 精确）**；sandbox.ini 含
  `UseFileImage=y` + `FileRootPath=<dir>\data`；**头 2KB 熵抽查 4/4：
  distinct=256/256 字节值全出现，gzip(2048B)=2079B（不可压缩）**。
- 负路径：exec 无密码非 tty → `rc=6 "encrypted box requires the image
  password; non-interactive stdin - use --mount-password <pw> or the
  SBIE_BOX_PASSWORD environment variable"`（信封完美）；create 无密码非
  tty → 同款指引且**不留目录残留**；错密码 → `rc=1 mount data.box failed
  for '<box>'`（可定位；"(GENERIC) (GENERIC)" 双重后缀为文案小疵）。
- 盒目录基名含空格/连字符被正确拒绝（rc=7 INVALID，§4.2 盒名=基名约束，
  非缺陷——空格仅允许父目录）。
- **R3 tty 交互分支：本环境无 tty 不可测，标注待人工（不阻塞判定）。**
- minor（新）：**错密码/无密码失败后的快速重试会撞 STATE_TIMEOUT**——失败
  的 exec 已完成注册，残留代际需 monitor teardown（>默认 10s settle）；自愈，
  一次交错负路径测试中观察到 waitgone 卡需 recover。建议失败路径复用
  CleanupAfterSpawnFailure 语义尽早回收。

## E2 — 模板面回归

- **Basic.ini 变量**：`%Tmpl.Firefox%` 正确冻结展开（→
  `%AppData%\Mozilla\Firefox\Profiles\*`）；盒级 `[TemplateVars]` 覆盖生效且
  值逐字节透传（`D:\zz\gg` → `D:\zz\gg\logins.json`）。测试中一度疑似的
  `\f` 吞噬经隔离复验为**本人 printf 转义假象**（JSON 传输折叠 `\` 后 printf
  做 C 转义），产品无辜，如实披露。
- **六型 create-box**：
  - **dist 树（默认解析）：1/6 可用**——standard 全过；hardening /
    hardened-plus / standard-plus / app / app-plus 五型 exec 全部
    `rc=11 template: not found: BoxTypes\<X>`。**根因：dist
    `Installer\SbieOSS_x64\templates\BoxTypes\` 只部署了 Standard.ini（旧
    版），5 个新模板文件未随构建部署**（仓库 `SandboxieOSS\templates\
    BoxTypes\` 六件齐全且 Standard.ini 为新版）。
  - **仓库树（SBIE_TEMPLATE_DIR 指向 repo）：6/6 exec PASS**，型键正确
    （hardened_plus 含 UsePrivacyMode+UseSecurityMode 等）→ **代码无恙，
    纯部署缺口**。
- **三模板展开抽查**：`Misc\RpcPortBindings`（RpcPortBinding×6）、
  `System\WindowsExplorer`（FakeAdminRights 等）、`Print\
  AdobeAcrobatReader`（OpenPipePath×2 + NoRenameWinClass）——3/3 展开
  正确、**零 `Template=` 残留**；不存在模板名给 rc=11 明确信封（正确）。
- dist `Templates.ini`（当时为 446 节全量）——本报告原文按 docs/05
  §8.2 当时的"冻结运行时需全量"结论判定"dist 保留 446 为正确行为"。
  **该结论已于 2026-09-28 被干净 A/B 矩阵推翻并修订**（docs/05 §8.2
  修订 2）：全量不必须，真正的载荷是骨架节（`[TemplateDefaultPaths]`
  的 `\KnownDlls\*` 开放路径等）；现 dist 部署源树裁剪版
  `SandboxieOSS\Templates.ini`（951 行/45 节/0 引用），`--verify`
  以与源字节一致为门。此段保留原判仅作历史记录，以 docs/05 现版为准。

## E3 — 明文盒快速回归（全过）

exec 默认即退 rc=0；`--wait` 退出码 9 精确透传；ps 可见；kill-box → 归零
teardown ok；log dump rc=0 8/8 可读 + `--box` 过滤 9/9 全命中——**加密盒钩子
未误伤明文盒路径**。（log 测试短暂停 SandMan 后已 `-autorun` 复启。）

## 环境恢复确认（对照 baseline4）

ini 与 baseline4 字节级一致（ImportBox=0）；`%LOCALAPPDATA%\SandboxieOSS`
整目录删除（含 4×256MiB 测试容器）；无 sbie-cli/monitor 进程；SbieSvc/SbieDrv
RUNNING 未动；生产/被测安装目录双双 diff 零；全部测试目录（含 `Enc Boxes`、
相对路径盒）清除；SandMan 运行中（复启后 pid 169036，= 起始运行态）。

## 最终判定：FAIL → 退回（仅一项：dist 部署缺口；修复动作极小）

- 加密盒功能本体（容器/挂载/密码三路径/读写回环/自动卸载/熵）**全部通过**，
  模板引擎与明文盒回归零回退——代码层面已达"待 push"质量。
- **唯一拦路项**：`Installer\SbieOSS_x64\templates\BoxTypes\` 缺 5 个新模板
  文件且 Standard.ini 为旧版 → 按默认部署路径 **六型中 5/6 在 exec 时
  TEMPLATE_ERROR**。修复 = 重跑打包/同步 6 文件（仓库源已齐）。
- 微复测建议：补部署后仅测 dist 树六型 exec（6 行命令级验证），其余面无需
  重跑。带去后续：wrong-pw 文案双后缀、失败后快速重试 STATE_TIMEOUT、
  R3 tty 待人工、-w 等待新增残留（前轮已备案）。
