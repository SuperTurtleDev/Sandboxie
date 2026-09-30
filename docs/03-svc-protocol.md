# 03 — SbieSvc 管道协议（LPC 端口）

来源（均已实读）：

- `Sandboxie\core\svc\msgids.h` — 全部 MSGID 与 `MSG_HEADER`
- `Sandboxie\core\svc\sbieiniwire.h` — SBIE_INI 系列请求/回复结构
- `Sandboxie\core\svc\sbieiniserver.cpp` — 服务端处理流程/鉴权/落盘
- `Sandboxie\core\svc\ProcessWire.h`、`queuewire.h`、`InteractiveWire.h` — 进程/队列结构
- `Sandboxie\core\svc\PipeServer.cpp` — 服务端端口创建/线程模型/调用方识别
- `SandboxiePlus\QSbieAPI\SbieAPI.cpp`（LGPL）— **客户端连接与分块协议的参考实现**
  （`SbieAPI.cpp:447-585`）、`SandboxiePlus\QSbieAPI\SbieDefs.h:8` — 端口名

## 1. 端口与连接方式（核实结论）

**端口名：`\RPC Control\SbieSvcPort`**（`QSbieAPI\SbieDefs.h:8`，`#define SBIESVC_PORT`）。

**连接方式：不是 CreateFile 命名管道，而是 NT LPC 端口 —— `NtConnectPort`**：

- 服务端：`PipeServer::Start()` 用 **`NtCreatePort`** 创建（`PipeServer.cpp:263-274`），
  名字取自 `SbieDll_PortName()`（同文件 :263），DACL 置 NULL（任意进程可连，:250-257），
  消息上限 `MAX_PORTMSG_LENGTH = 328`（`Sandboxie\common\win32_ntddk.h:1681`）。
- 客户端（本项目照此实现）：`CSbieAPI__ConnectPort`，`SbieAPI.cpp:447-473`：

```cpp
SECURITY_QUALITY_OF_SERVICE QoS = {
    sizeof(QoS), SecurityImpersonation, SECURITY_DYNAMIC_TRACKING, TRUE };
UNICODE_STRING PortName; RtlInitUnicodeString(&PortName, L"\\RPC Control\\SbieSvcPort");
NTSTATUS st = NtConnectPort(&PortHandle, &PortName, &QoS,
                            NULL, NULL, &MaxDataLen /*out: 每消息最大数据字节*/, NULL, NULL);
// Wow64 时 SizeofPortMsg = sizeof(PORT_MESSAGE) + 4*sizeof(ULONG)（x64 客户端不需要）
MaxDataLen -= sizeof(PORT_MESSAGE);   // 数据区容量
```

- QoS 的 `SecurityImpersonation` 是**必须**的：SbieSvc 端 `PipeServer::ImpersonateCaller`
  靠它拿到调用方令牌做鉴权与 ini 写入（`sbieiniserver.cpp:142-143`）。
- **线程亲和**：LPC 客户端端口与发起线程绑定；SbieSvc 端 `PortDisconnectByCreateTime`
  按连接时间清理。QSbieAPI 的结论（`SbieAPI.cpp:764-772` 注释）：5.50.5 起 SbieSvc 能处理
  重连，但为队列机制仍由专职线程发起所有请求。**本项目规范：server 进程内固定一个
  "SvcClient 线程"串行发出全部 LPC 请求**（复刻 `CallServer` 的状态机，
  `SbieAPI.cpp:764-800`：SvcLock 六状态转交）。

## 2. MSG_HEADER 与消息布局

`msgids.h:157-165`：

```c
typedef struct _MSG_HEADER {
    ULONG length;                 // 整条消息字节数（含本头 8 字节）
    union { ULONG msgid;          // 请求方向：消息号
            ULONG status; };      // 回复方向：NTSTATUS（个别消息为 win32 错误）
} MSG_HEADER;                     // 共 8 字节
```

- 请求：`h.msgid = MSGID_*`；`h.length` = 结构体全长（变长部分计入）。
- 回复：`h.status` = NTSTATUS（`SBIE_INI_GET_WAIT_HANDLE` 例外，为 win32 error，
  `sbieiniwire.h:63` 注释）。服务器短回复 = 仅 8 字节头（`SHORT_REPLY`）；
  长回复 = 头 + 变长数据（`LONG_REPLY`），`h.length` 决定总长。
- 变长字段约定：`value[1]`（WCHAR 尾数组）+ `value_len`（WCHAR 个数）；
  总长 = `sizeof(结构体) + value_len*sizeof(WCHAR)`（见 `SBIE_INI_SETTING_REQ` 用例，
  `SbieAPI.cpp:1272-1286`）。
- 尺寸上限：`PIPE_MAX_DATA_LEN = 0x00FFFFFF`（`PipeServer.h:39`）；
  `SbieIniServer::CheckRequest` 拒绝 `value_len*sizeof(WCHAR) > PIPE_MAX_DATA_LEN`
  （`sbieiniserver.cpp:438-440`）。

### 分块传输（大消息 > MaxDataLen）

`CSbieAPI__CallServer`，`SbieAPI.cpp:475-585`，必须逐行为复刻：

1. **发送**：把 `req` 按 `MaxDataLen` 切块，逐块
   `NtRequestWaitReplyPort(PortHandle, RequestBuff, ResponseBuff)`；
   `ReqHeader->u1.s1.DataLength = send_len`，`TotalLength = sizeof(PORT_MESSAGE)+send_len`。
2. **序号**：首块把**数据第 4 字节**（即 `MSG_HEADER.length` 的最高字节，
   `ResData[3]`）覆盖为 `CurSeqNumber = (UCHAR)CallSeqNumber++`；末块后服务器回执
   首块回复，其 `ResData[3]` 必须等于该序号，否则 `mismatched reply` 断连。
   读取方把回复首块的该字节清零后才能得到真实 `h.length`。
3. **接收**：末次请求的回复携带首块；剩余块由**空请求**（`TotalLength=sizeof(PORT_MESSAGE)`，
   DataLength=0）继续 `NtRequestWaitReplyPort` 拉取，直到收满 `h.length`。
4. **错误恢复**：任何 `NtRequestWaitReplyPort` 失败 → `NtClose(PortHandle)` 置空，
   下次调用重新 `NtConnectPort`；"null reply"（回复 length==0）同断连处理。
5. 非 WOW64 的 x64 进程 `SizeofPortMsg = sizeof(PORT_MESSAGE)`（x64 上为 0x28=40 字节，
   x86 为 24）——**用 `sizeof(PORT_MESSAGE)` 取值，勿硬编码**；Wow64 时再加
   `sizeof(ULONG)*4`（`SbieAPI.cpp:467-470`）。

## 3. SBIE_INI 系列（配置读写，`sbieiniwire.h`）

### 3.1 消息号（`msgids.h:89-103`）

| MSGID | 值 | 方向 | 结构 |
|---|---|---|---|
| `MSGID_SBIE_INI_GET_USER` | 0x1801 | req 空 → rpl | `SBIE_INI_GET_USER_RPL` |
| `MSGID_SBIE_INI_GET_PATH` | 0x1802 | req 空 → rpl | `SBIE_INI_GET_PATH_RPL` |
| `MSGID_SBIE_INI_TEMPLATE` | 0x1806 | req → **短回复**（仅头） | `SBIE_INI_TEMPLATE_REQ` |
| `MSGID_SBIE_INI_SET_PASSWORD` | 0x1807 | req → 短回复 | `SBIE_INI_PASSWORD_REQ` |
| `MSGID_SBIE_INI_TEST_PASSWORD` | 0x1808 | req → 短回复 | `SBIE_INI_PASSWORD_REQ` |
| `MSGID_SBIE_INI_GET_SETTING` | 0x1810 | req → rpl | `SBIE_INI_SETTING_REQ → _RPL` |
| `MSGID_SBIE_INI_SET_SETTING` | 0x1811 | req → 短回复 | `SBIE_INI_SETTING_REQ` |
| `MSGID_SBIE_INI_ADD_SETTING` | 0x1812 | req → 短回复 | 同上（追加值） |
| `MSGID_SBIE_INI_INS_SETTING` | 0x1813 | req → 短回复 | 同上（插到首位） |
| `MSGID_SBIE_INI_DEL_SETTING` | 0x1814 | req → 短回复 | 同上（删值；value 空=整 setting） |
| `MSGID_SBIE_INI_GET_VERSION` | 0x18AA | req 空 → rpl | `SBIE_INI_GET_VERSION_RPL` |
| `MSGID_SBIE_INI_GET_WAIT_HANDLE` | 0x18AB | req 空 → rpl | `SBIE_INI_GET_WAIT_HANDLE_RPL` |
| `MSGID_SBIE_INI_RUN_SBIE_CTRL` | 0x180A | （沙箱内允许；本项目不用） | |
| `MSGID_SBIE_INI_RC4_CRYPT` | 0x180F | req → rpl | `SBIE_INI_RC4_CRYPT_REQ/_RPL` |

### 3.2 结构体（逐字段，行号 = sbieiniwire.h）

```c
// :97-112  当前用户/节名
struct SBIE_INI_GET_USER_RPL { MSG_HEADER h;   // status
    BOOLEAN admin;                    // 调用方是否管理员（TokenIsAdmin，含 UAC 判定）
    WCHAR section[BOXNAME_COUNT];     // 用户节名 "UserSettings_%08X"（Adler32(小写用户名)）
    ULONG name_len; WCHAR name[1]; }; // 用户名（空格/反斜杠已翻译为 '_'）
// 服务端：sbieiniserver.cpp:357-390；节名规则 :487-559（UserSettings_Portable 优先，:499-509）

// :76-89  ini 文件路径
struct SBIE_INI_GET_PATH_RPL { MSG_HEADER h; BOOLEAN is_home_path; WCHAR path[1]; };
// 服务端 :398-424（RevertToSelf 后取，路径为系统/家目录二选一）

// :35-48  SbieSvc 版本
struct SBIE_INI_GET_VERSION_RPL { MSG_HEADER h; ULONG abi_ver; WCHAR version[1]; };
// 服务端 :297-314：version = MY_VERSION_STRING（"5.73.5"），abi_ver = MY_ABI_VERSION(0x57230)

// :56-68  等待 SbieSvc 退出（升级用）
struct SBIE_INI_GET_WAIT_HANDLE_RPL { MSG_HEADER h /*status=win32错误*/; HANDLE hProcess; };
// 服务端 :322-349：把 SbieSvc 自身进程句柄以 SYNCHRONIZE 复制进调用方

// :120-136 读写设置（四个变体共用）
struct SBIE_INI_SETTING_REQ {
    MSG_HEADER h;
    WCHAR password[66];      // 明文密码；EditPassword 未设时留空
    BOOLEAN refresh;         // TRUE: 服务端写盘后调 SbieApi_ReloadConf（RefreshConf）
    WCHAR section[66];       // 空 => 服务端替换为 "GlobalSettings"（:447-448）
    WCHAR setting[66];
    ULONG value_len;         // WCHAR 数；尾部 value[value_len] 必须 L'\0'（:444）
    WCHAR value[1]; };
struct SBIE_INI_SETTING_RPL { MSG_HEADER h; ULONG value_len; WCHAR value[1]; };  // 仅 GET 用

// :147-157  模板变量（[TemplateSettings] 节的 varname）
struct SBIE_INI_TEMPLATE_REQ { MSG_HEADER h; WCHAR password[66]; WCHAR varname[66];
                              BOOLEAN user; ULONG value_len; WCHAR value[1]; };

// :165-172  设置/测试密码
struct SBIE_INI_PASSWORD_REQ { MSG_HEADER h; WCHAR old_password[66]; WCHAR new_password[66]; };

// :180-195  RC4 加解密（GlobalSettings EditPassword 的哈希存储辅助等）
struct SBIE_INI_RC4_CRYPT_REQ  { MSG_HEADER h; ULONG value_len; UCHAR value[1]; };
struct SBIE_INI_RC4_CRYPT_RPL  { MSG_HEADER h; ULONG value_len; UCHAR value[1]; };
```

### 3.3 服务端处理流程与鉴权（`sbieiniserver.cpp:96-289`）

1. `PipeServer::GetCallerProcessId()` 取调用方 PID → `SbieApi_QueryProcess(pid,…)`
   检测调用方是否在沙箱内；**沙箱内调用者一律 `STATUS_NOT_SUPPORTED`**
   （:139-140），例外仅 `RUN_SBIE_CTRL` 与 `RC4_CRYPT`。
2. `PipeServer::ImpersonateCaller(&msg)` —— 失败即返回。
3. GET_VERSION / GET_WAIT_HANDLE / GET_USER / GET_PATH 不需要缓存 ini。
4. 其余先 `CacheConfig()`（读 ini 进内存）。
5. SET/ADD/INS/DEL_SETTING 走 `CheckRequest`（:432-474）：
   - 长度/越界/尾零校验（:435-445）；
   - `section[0]=='\0'` → `GlobalSettings`；
   - 前缀 `UserSettings_`（:456-462）→ **替换为调用者真实用户节**（防伪造他人节）；
   - 其余节 → `IsCallerAuthorized(hToken, password, section)`（:466，实现 :826+）：
     `EditPassword` 已设 → 校验密码（`STATUS_WRONG_PASSWORD`）；未设 → 要求管理员
     （非管理员 `STATUS_LOGON_NOT_GRANTED`；即只有管理员可以改非自身节）。
6. `RevertToSelf()` 后以 SbieSvc 身份写 ini（SetSetting/AddSetting/DelSetting），
   `req->refresh==TRUE` → `RefreshConf()`（写临时盘→备份→SaveIni→
   **`SbieApi_ReloadConf(m_session_id, 0)`** →恢复属性，见 02 §3.4）。
7. `status==STATUS_INSUFFICIENT_RESOURCES` 时向调用方会话发 SBIE 2305。
8. 回复一律**短回复**（仅 `MSG_HEADER.status`），GET_SETTING 例外（带 value）。

QSbieAPI 侧的密码重试语义（`SbieAPI.cpp:1231-1257`）：`STATUS_LOGON_NOT_GRANTED` /
`STATUS_WRONG_PASSWORD` → UI 提示重输。CLI 对应行为：`sbie cfg set --password` /
读 `SBIE_PASS` 环境变量；无 TTY 时直接报 6 号退出码。

## 4. PROCESS 系列（`ProcessWire.h`，消息号 `msgids.h:40-51`）

| MSGID | 值 | 结构（ProcessWire.h 行号） |
|---|---|---|
| `MSGID_PROCESS_KILL_ONE` | 0x1203 | `PROCESS_KILL_ONE_REQ {h; ULONG pid;}`（:35-39）→ 短回复 |
| `MSGID_PROCESS_KILL_ALL` | 0x1204 | `PROCESS_KILL_ALL_REQ {h; ULONG session_id; WCHAR boxname[BOXNAME_COUNT];}`（:49-56）→ 短回复。`session_id=-1` 全会话 |
| `MSGID_PROCESS_RUN_SANDBOXED` | 0x1205 | `PROCESS_RUN_SANDBOXED_REQ {h; WCHAR boxname[40]; ULONG cmd_ofs,cmd_len,dir_ofs,dir_len,env_ofs,env_len,si_flags,si_show_window,creation_flags;}`（:93-106）+ 变长区（offset 相对结构体起点，WCHAR 计数）→ `PROCESS_RUN_SANDBOXED_RPL {h /*win32错误*/; ULONG64 hProcess; ULONG64 hThread; ULONG dwProcessId; ULONG dwThreadId;}`（:108-115）。句柄已复制进调用方，**用完须 CloseHandle**（`SbieAPI.cpp:2017-2018`） |
| `MSGID_PROCESS_RUN_UPDATER` | 0x1208 | `{h; ULONG cmd_ofs,cmd_len,elevate;}`（:126-132），rpl 同 RUN_SANDBOXED（本项目不用） |
| `MSGID_PROCESS_GET_INFO` | 0x1209 | `PROCESS_GET_INFO_REQ {h; ULONG dwProcessId; ULONG dwInfoClasses;}`（:148-153）。`dwInfoClasses` 位：`SBIE_PROCESS_BASIC_INFO=1`（父pid/flags）、`EXEC_INFO=2`（suspended）、`PATHS_INFO=4`（镜像/命令行/工作目录）（:142-146）。回复 `PROCESS_INFO_RPL {h; ULONG dwParentId; ULONG dwInfo; BOOLEAN bSuspended; ULONG app_ofs,app_len,cmd_ofs,cmd_len,dir_ofs,dir_len;}` + 变长 WCHAR 串（:155-170）。用法 `SbieAPI.cpp:1696-1728` |
| `MSGID_PROCESS_SUSPEND_RESUME_ONE` | 0x120A | `{h; ULONG pid; BOOLEAN suspend;}`（:181-186） |
| `MSGID_PROCESS_SUSPEND_RESUME_ALL` | 0x120B | `{h; ULONG session_id; WCHAR boxname[40]; BOOLEAN suspend;}`（:196-202） |

## 5. QUEUE 系列（`queuewire.h`，`msgids.h:140-147`）

会话级请求/应答队列（SbieSvc 内实现，`queueserver.cpp`）。`QUEUE_NAME_MAXLEN=64`。

| MSGID | 值 | 结构（queuewire.h 行号） |
|---|---|---|
| `MSGID_QUEUE_CREATE` | 0x1E01 | `QUEUE_CREATE_REQ {h; WCHAR queue_name[64]; ULONG64 event_handle;}`（:44-49）——`event_handle` 是**调用方先 `CreateEvent` 再传值**，SbieSvc 有请求时 SetEvent；→ `QUEUE_CREATE_RPL {h;}`（:51-54） |
| `MSGID_QUEUE_GETREQ` | 0x1E02 | `QUEUE_GETREQ_REQ {h; queue_name[64];}`（:65-69）→ `QUEUE_GETREQ_RPL {h; ULONG client_pid, client_tid, req_id, data_len; UCHAR data[1];}`（:71-79） |
| `MSGID_QUEUE_PUTRPL` | 0x1E03 | `QUEUE_PUTRPL_REQ {h; queue_name[64]; ULONG req_id; ULONG data_len; UCHAR data[1];}`（:90-97） |
| `MSGID_QUEUE_PUTREQ` | 0x1E04 | `{h; queue_name[64]; ULONG64 event_handle; ULONG data_len; UCHAR data[1];}`（:113-120）→ `QUEUE_PUTREQ_RPL {h; ULONG req_id;}` |
| `MSGID_QUEUE_GETRPL` | 0x1E05 | `{h; queue_name[64]; ULONG req_id;}`（:137-142）→ `{h; ULONG data_len; UCHAR data[1];}` |

## 6. Interactive queue（会话队列机制）

驱动/SbieSvc 把需要**用户态 UI 决策**的请求投到会话队列，由会话 leader 程序（我们的
server）取出、决策、回填：

- 队列名：`*MANPROXY_%08X`（%08X = session id，十六进制）——
  `wsprintfW(QueueName, L"*%s_%08X", INTERACTIVE_QUEUE_NAME, sessionId)`
  （`SbieAPI.cpp:62`）；`INTERACTIVE_QUEUE_NAME = L"MANPROXY"`（`InteractiveWire.h:35`）。
  前缀 `*` 保留给系统命名习惯（同文件无额外语义，照抄即可）。
- 流程（QSbieAPI 参考，`SbieAPI.cpp:587-709`）：
  1. `CreateEvent(NULL, FALSE, FALSE, NULL)` → `MSGID_QUEUE_CREATE("*MANPROXY_XXXXXXXX", hev)`；
  2. 泵线程 `WaitForSingleObject(hev, 0)==0` 时循环 `MSGID_QUEUE_GETREQ`；
  3. `data[0..3]`（ULONG）分派请求类型（`InteractiveWire.h:37-38`）：
     - `MAN_FILE_MIGRATION=1`：`MAN_FILE_MIGRATION_REQ {ULONG msgid; ULONGLONG file_size; WCHAR file_path[256];}`（:46-51）→ 沙箱内大文件复制超过阈值，问是否迁出到真实路径；
     - `MAN_INET_BLOCKADE=2`：`MAN_INET_BLOCKADE_REQ {ULONG msgid;}`（:67-70）→ 网络封锁提示；
  4. 决策后 `MSGID_QUEUE_PUTRPL`：`MAN_FILE_MIGRATION_RPL/MAN_INET_BLOCKADE_RPL {ULONG status; ULONG retval;}`（:53-57, :72-76）——注意这里 `{status, retval}` **裸结构**（不带 MSG_HEADER，是 data 载荷）。
- server 的 CLI 策略（默认无人值守）：`FILE_MIGRATION` → 按沙箱 `FileMigrationEnabled`
  配置自动答（默认拒绝，retval=0）；`INET_BLOCKADE` → 记日志 retval=0。
  `sbie log watch --interactive` 在线时改为打印并等待stdin。**TODO-VERIFY**：retval 的
  具体取值含义（0/1 对应允许/拒绝）未见源码常量，实现期用 `MAN_FILE_MIGRATION_RPL`
  在 `fileserver.cpp`/`file_copy` 调用侧回读核实。

## 7. 其他消息组（本项目不实现，记录在案防止误用）

- `MSGID_SERVICE_*`(0x1300+)：沙箱内服务代理；`MSGID_TERMINAL_*`(0x1400+)；
  `MSGID_NAMED_PIPE_*`(0x1500+)；`MSGID_FILE_*`(0x1700+)：SbieDll 内部（沙箱进程）用；
  `MSGID_NETAPI/COM/IPHLP`：同上；`MSGID_IMBOX_*`(0x1D00+)：加密盘沙箱
  （SandboxieTools/ImBox，**许可证禁用**，不做）；`MSGID_EPMAPPER_*`(0x1F00+)。
- 这些 msgid 的服务端也全部挂在同一个 `\RPC Control\SbieSvcPort` 上，由
  `PipeServer::Register(serverId, …)` 分派（`PipeServer.cpp:220-233`，
  serverId = 消息组基号 0x1100/0x1200/…）。**发错基号会命中别的 handler**，
  结构错位 —— client 侧必须用 vendored 头文件中的常量，禁止手写数值。

## 8. 坑记录

1. **规格书原文之误（已核实纠正）**：任务描述猜"CreateFile 管道"。实为
   **`NtConnectPort` 连 LPC 端口 `\RPC Control\SbieSvcPort`**（客户端
   `SbieAPI.cpp:447-473`；服务端 `NtCreatePort`，`PipeServer.cpp:268`）。
   sbie-cli 与 SbieSvc 之间的通信必须用 LPC/ALPC（`ntdll` 导入），Win32 `CreateFile`
   无法连接该端口。
2. **首块序号覆盖的是 `h.length` 最高字节**：请求总长不得超过 `0x00FFFFFF`
   （PIPE_MAX_DATA_LEN），否则序号机制与 length 冲突 —— 与 SbieSvc 的上限一致，
   client 侧发送前需断言 `req->length <= 0x00FFFFFF`。
3. **`SBIE_INI_GET_WAIT_HANDLE` 的 status 是 win32 错误**（`sbieiniwire.h:63` 注释；
   其余 SBIE_INI 回复是 NTSTATUS）。判错分支要区分。
4. **句柄生命周期**：`RUN_SANDBOXED` 回复中的 `hProcess/hThread` 是复制进本进程的
   句柄，不关会泄漏；QSbieAPI 立即 Close（`SbieAPI.cpp:2017-2018`）。但 `sbie proc start
   --wait` 需要保留 hProcess 等待退出码。
5. **断开重连与线程**：LPC 端口绑定首次 `NtConnectPort` 的线程。QSbieAPI 把所有请求
   交给单一工作线程（`SbieAPI.cpp:764-800`）。本项目 SvcClient 线程模型为强约束，
   不得在命令处理线程里直接 `NtRequestWaitReplyPort`。
6. `SBIE_INI_SETTING_REQ.password` 为 66 WCHAR 定长（非变长）；QSbieAPI 注明
   "fix-me: potential overflow"（`SbieAPI.cpp:1234`）——本项目必须显式截断至 64+WCHAR
   终止符，不重蹈。
