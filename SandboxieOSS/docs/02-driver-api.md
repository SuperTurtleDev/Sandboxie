# 02 — 驱动 API（经 SbieDll.dll 导出）

来源（均已实读）：

- `Sandboxie\core\dll\sbieapi.h` / `sbieapi.c` — SbieApi_* 导出实现
- `Sandboxie\core\drv\api_defs.h` — API 码枚举与参数结构体（ABI）
- `Sandboxie\core\drv\api_flags.h` — CONF_*/MONITOR_*/SBIE_FLAG_* 标志
- `Sandboxie\core\drv\api.c`、`conf.c`、`session.c`、`process_api.c` — 驱动端权限/语义
- `SandboxiePlus\QSbieAPI\SbieAPI.cpp`（LGPL，参考用法）
- 导出事实核对：`Installer\SbiePlus_x64\SbieDll.dll`（5.73.5 x64 构建产物）strings 导出表

## 1. 决策：动态加载 SbieDll.dll（LoadLibrary + GetProcAddress）

**本项目不链接 `SbieDll.lib` 导入库，也不自行拼接 IOCTL**，而是运行时动态加载
`SbieDll.dll` 并 `GetProcAddress` 绑定 `SbieApi_*` 导出。理由：

1. **避免链接期依赖与版本耦合**：SbieDll.dll 随 Sandboxie 安装目录分发
   （`Installer\SbiePlus_x64\SbieDll.dll`），版本随驱动更新（冻结策略下为 5.73.5）。
   动态绑定允许：缺失/版本不识别时给出可诊断错误，而不是加载期失败。
2. **复用经过验证的参数编排**：`SbieApi_QueryProcessEx2` 等包装函数正确填充
   `UNICODE_STRING64`、处理缓冲区截断与错误清零（`sbieapi.c:562-635` 等）。
   自行拼 IOCTL（QSbieAPI 的路线，`SbieAPI.cpp:79-83`）需要重复这些细节且无收益。
3. **安全先例**：`Start.exe`、`SbieIni.exe` 在沙箱外加载 SbieDll.dll（链接方式，
   `apps\start\Start.vcxproj:118` `SbieDll.lib`）。SbieDll 的初始化对非沙箱进程自动进入
   "管理进程"模式（不安装 hook），因此 LoadLibrary 到 sbie-cli.exe 是安全的。

加载规范：

```cpp
// 伪代码规范（实现于 SbieCore/DriverApi.cpp）
WCHAR self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
PathRemoveFileSpecW(self);                       // sbie-cli.exe 所在目录
std::wstring path = std::wstring(self) + L"\\SbieDll.dll";
HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_WITH_ALTERED_SEARCH_PATH);   // 只从显式路径解析依赖
// 失败 → 错误码 SBIE_ERR_SBIEDLL_NOT_FOUND；绑定前先取 SbieApi_GetVersionEx 校验 ABI。
```

绑定后立即用 `SbieApi_GetVersionEx(buf, &abi)` 校验：`version_string` 应为
`"5.73.5"`（`MY_VERSION_STRING`，`Sandboxie\common\my_version.h:34-37` + VERSION_MJR/MIN/REV
= 5/73/5），`abi_version` 为 `0x57230`（`my_version.h:39` `MY_ABI_VERSION`）。
ABI 不匹配 → 拒绝运行管理命令（只允许 `sbie version`）。

### GetProcAddress 动态绑定的 C++ 函数指针类型规范

- 所有导出均为 `extern "C"`（`sbieapi.h:28-30`），x64 下仅有一种调用约定（Microsoft x64），
  无需 `__stdcall/__cdecl` 修饰；为可读性统一声明为默认（cdecl）。
- 每个绑定点：`using P_SbieApi_XXX = 返回值 (CALLBACK*)(形参表);`（`CALLBACK` 展开为空，
  保留以自文档）。**类型必须与 sbieapi.h 原型逐参一致**（含 `WCHAR*`/`ULONG*`，不得用
  `void*` 糊过去）；变长参数函数 `SbieApi_Call` 用 `...` 原型绑定（x64 变长调用安全）。
- 绑定表实现为静态结构体 + 一次性初始化函数，缺失导出（老版本 DLL）逐项报名字。

```cpp
// SbieCore/DriverApi.h —— 类型定义规范（节选，完整表见 §3）
using P_SbieApi_GetVersionEx   = LONG (CALLBACK*)(WCHAR* version_string /*[16]*/,
                                                  ULONG* abi_version);
using P_SbieApi_Call           = LONG (CALLBACK*)(ULONG api_code, LONG arg_num, ...);
using P_SbieApi_Ioctl          = LONG (CALLBACK*)(ULONG64* parms);
using P_SbieApi_QueryProcessEx2= LONG (CALLBACK*)(HANDLE ProcessId,
                                                  ULONG image_name_len_in_wchars,
                                                  WCHAR* out_box_name /*BOXNAME_COUNT*/,
                                                  WCHAR* out_image_name,
                                                  WCHAR* out_sid_wchar96,
                                                  ULONG* out_session_id,
                                                  ULONG64* out_create_time);
using P_SbieApi_QueryConf      = LONG (CALLBACK*)(const WCHAR* section /*[66]*/,
                                                  const WCHAR* setting /*[66]*/,
                                                  ULONG setting_index,
                                                  WCHAR* out_buffer, ULONG buffer_len);
using P_SbieApi_UpdateConf     = ULONG(CALLBACK*)(ULONG op, const WCHAR* section,
                                                  const WCHAR* setting, const WCHAR* value);
using P_SbieApi_ReloadConf     = LONG (CALLBACK*)(ULONG session_id, ULONG flags);
using P_SbieApi_GetMessage     = ULONG(CALLBACK*)(ULONG* MessageNum, ULONG SessionId,
                                                  ULONG* MessageId, ULONG* Pid,
                                                  wchar_t* Buffer, ULONG Length);
using P_SbieApi_SessionLeader  = LONG (CALLBACK*)(ULONG session_id, HANDLE* ProcessId);
using P_SbieDll_PortName       = const WCHAR* (CALLBACK*)();

struct SbieApiBindings {          // 成员名与导出名一致；§3 表的机器可读形式
    P_SbieApi_GetVersionEx     SbieApi_GetVersionEx;
    P_SbieApi_Call             SbieApi_Call;
    P_SbieApi_Ioctl            SbieApi_Ioctl;
    /* … */
    P_SbieDll_PortName         SbieDll_PortName;
};
// 初始化：GetProcAddress((PROC&)dst, "SbieApi_GetVersionEx") 形式逐项绑定；
// 全部成功返回 true，任一失败 GetLastError()==127 时输出缺失符号名。
```

## 2. 传输层（理解用，不直接实现）

SbieApi_* 底层经 `SbieApi_Ioctl`（`sbieapi.c:99-189`）：

- 设备：`API_DEVICE_NAME` = `L"\\Device\\SandboxieDriverApi"`（`api_defs.h:57`，
  `SANDBOXIE` 宏 = `L"Sandboxie"`，`QSbieAPI\SbieDefs.h:3`）。
- 打开：`NtOpenFile(FILE_GENERIC_READ, share RWD)`；`STATUS_OBJECT_NAME_NOT_FOUND`/
  `STATUS_NO_SUCH_DEVICE` 映射为 **`STATUS_SERVER_DISABLED` = "驱动未运行"**（`sbieapi.c:131-133`；
  数值见 ntstatus.h，**TODO-VERIFY**：实现错误表时从 SDK 头取常数，勿凭记忆写）。
  → `sbie status` 用它判断驱动在场。
- IOCTL：`API_SBIEDRV_CTLCODE = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_NEITHER, FILE_ANY_ACCESS)`
  （`api_defs.h:60-61`）；输入 = `ULONG64 parms[API_NUM_ARGS=8]`（`api_defs.h:70`），
  `parms[0] = api_code`；请求全部同步串行（`sbieapi.c:142-147` 注释）。

### API 码数值表（由 `api_defs.h:87-171` 枚举顺序推得；API_FIRST = 0x12340000）

仅列本项目用到的（vendor `api_defs.h` 后直接用符号名，不要硬编码数值）：

| API 码 | 偏移 | 用途 | 驱动端实现 |
|---|---|---|---|
| `API_GET_VERSION` | +1 | 版本/ABI | `api.c:190` 注册 `Api_GetVersion` |
| `API_LOG_MESSAGE` | +3 | 写日志（CLI 少用） | `api.c:191` |
| `API_QUERY_PROCESS` | +7 | 进程五元组 | process.c |
| `API_QUERY_BOX_PATH` | +8 | 沙箱三根路径 | box.c |
| `API_QUERY_PROCESS_PATH` | +9 | 进程沙箱路径 | box.c |
| `API_QUERY_PATH_LIST` | +10 | 路径规则列表（`sbie box info --paths` 进阶） | |
| `API_ENUM_PROCESSES` | +11 | 枚举沙箱内 PID | process.c |
| `API_DISABLE_FORCE_PROCESS` | +12 | 临时禁用强制沙箱 | `session.c` `Session_Api_DisableForce`（非沙箱限定，`session.c:418-421`） |
| `API_QUERY_CONF` | +15 | 读 ini（驱动缓存） | `conf.c:1897` 附近 `Conf_Api_Query` |
| `API_RELOAD_CONF` | +16 | 重读 ini | `conf.c:1701` `Conf_Api_Reload`（非沙箱限定） |
| `API_GET_UNMOUNT_HIVE` | +30 | （不用） | |
| `API_GET_FILE_NAME` | +31 | 句柄→NT 路径（路径解析） | file.c |
| `API_START_PROCESS` | +36 | （不直接用；启动走 SbieSvc，见 03） | |
| `API_CHECK_INTERNET_ACCESS` | +37 | （不用） | |
| `API_GET_HOME_PATH` | +38 | Sandboxie 安装目录（NT/DOS 两式） | `api.c:193` |
| `API_OPEN_DEVICE_MAP` | +42 | 无管理员权限时取 `\??` 设备映射 | |
| `API_OPEN_PROCESS` | +43 | 打开沙箱进程句柄 | |
| `API_QUERY_PROCESS_INFO` | +44 | 进程 flags/token/image type | `process_api.c` `Process_Api_QueryInfo`（约 :20-260） |
| `API_IS_BOX_ENABLED` | +45 | 沙箱是否启用 | box.c |
| `API_SESSION_LEADER` | +46 | 会话 leader 查询/设置 | `session.c:336-411` |
| `API_MONITOR_CONTROL` | +27 | trace 开关 | |
| `API_MONITOR_GET_EX` | +63 | trace 单条取（兼容路径） | |
| `API_GET_MESSAGE` | +64 | 日志泵取一条 | `api.c:715-733`（非沙箱 + 会话 leader 限定） |
| `API_PROCESS_EXEMPTION_CONTROL` | +65 | 进程豁免开关 | `api.c:1118`（非沙箱限定） |
| `API_QUERY_DRIVER_INFO` | +68 | 驱动特征位/版本 | `api.c:202` |
| `API_SET_SECURE_PARAM` / `API_GET_SECURE_PARAM` | +70/+71 | 驱动保护参数（校验和存储） | `api.c:1245,1347`（非沙箱限定） |
| `API_MONITOR_GET2` | +72 | trace 批量取（1.6.6+，**SbieDll 无包装导出**） | |
| `API_UPDATE_CONF` | +78 | 改 ini（驱动缓存/仅 leader） | `conf.c:2145-2150` |
| `API_VERIFY` | +79 | 数据签名校验（不用） | |

## 3. SbieDll.dll 导出清单（本项目使用的子集）

签名以 `sbieapi.h` 行号为准；"限制"列 = 驱动端对调用进程的要求。

### 3.1 版本与驱动状态

| 导出 | 原型（摘要） | 语义/限制 |
|---|---|---|
| `SbieApi_GetVersion` (h:56) | `LONG (WCHAR[16])` | 转发 GetVersionEx |
| `SbieApi_GetVersionEx` (h:59) | `LONG (WCHAR[16], ULONG* abi)` | 失败时写 `L"unknown"`/0（c:252-255）。无限制 |
| `SbieApi_QueryDrvInfo` (h:344) | `LONG (ULONG info_class, VOID* data, ULONG size)` | `info_class=0` → 特征位 `SBIE_FEATURE_FLAG_*`（`api_flags.h:132-139`：WFP=1, ObCB=2, SbieLogin=0x10, Win32kHook=0x20, DynDataOk=0x10000000, NewArch=0x40000000）。参考 `SbieAPI.cpp:2287-2311` |

### 3.2 沙箱枚举与路径

| 导出 | 原型 | 语义/限制 |
|---|---|---|
| `SbieApi_EnumBoxes` / `SbieApi_EnumBoxesEx` (h:392-401) | `LONG (LONG index /*init -1*/, WCHAR out[BOXNAME_COUNT], BOOLEAN ignore_hidden)` | 迭代枚举；返回 -1 结束。内部走 `API_QUERY_CONF(NULL,NULL,index|CONF_GET_NO_TEMPLS|CONF_GET_NO_EXPAND,…)`（c:1598-1616）。`BOXNAME_COUNT=40`（`common\defines.h:54`）。无限制（读驱动缓存配置） |
| `SbieApi_IsBoxEnabled` (h:403) | `LONG (const WCHAR box[40])` | `STATUS_SUCCESS`=启用；`STATUS_ACCOUNT_RESTRICTION`=存在但未启用（QSbieAPI `IsBox`，`SbieAPI.cpp:2203-2217`）；其他=不存在 |
| `SbieApi_QueryBoxPath` (h:148) | `LONG (const WCHAR* box, WCHAR* file,key,ipc, ULONG* file_len,key_len,ipc_len)` | 两段式调用：先传 NULL 缓冲取长度，再取串（`SbieAPI.cpp:1603-1658`）。NT 路径；DOS 化见 §3.7 |

### 3.3 进程

| 导出 | 原型 | 语义/限制 |
|---|---|---|
| `SbieApi_QueryProcess` (h:103) | box[40], image[96], sid[96], session* | 转发 Ex2 |
| `SbieApi_QueryProcessEx` (h:111) | + image 名长 | 转发 Ex2 |
| `SbieApi_QueryProcessEx2` (h:120) | 上述 + `ULONG64* create_time` | 任意指针可 NULL；失败清零输出（c:613-632，除 session_id 特殊值 1..4） |
| `SbieApi_QueryProcessInfo` (h:130) | `ULONG64 (HANDLE pid, ULONG info_type)` | `info_type=0` → `SBIE_FLAG_*` 位（`api_flags.h:93-126`：FORCED=2, START_EXE=8, DROP_RIGHTS=0x80, FAKE_ADMIN=0x200, PRIVACY_MODE=0x02000000, APP_COMPARTMENT=0x01000000 …）。多字符码：`'gpit'`=image type、`'pril'`、`'ptok'`（token 句柄，勿用）等（`process_api.c:101-229`） |
| `SbieApi_QueryProcessInfoStr` (h:141) | `LONG (pid, info_type, WCHAR* str, ULONG* len)` | 字符串型 info |
| `SbieApi_EnumProcessEx` (h:176) | `LONG (const WCHAR box, BOOLEAN all_sessions, ULONG which_session /*-1=当前*/, ULONG* pids, ULONG* count)` | `pids=NULL` 时仅取 count（`SbieAPI.cpp:1479-1501` 的用法）。缓冲上限 `API_MAX_PIDS=512`（`api_defs.h:78`）；QSbieAPI 按需扩容 |
| `SbieApi_OpenProcess` (h:328) | `LONG (HANDLE* out, HANDLE pid)` | 经驱动打开（绕过 ACL 限制） |

### 3.4 配置（驱动缓存的 Sandboxie.ini）

| 导出 | 原型 | 语义/限制 |
|---|---|---|
| `SbieApi_QueryConf` (h:363) | `LONG (const WCHAR section[66], const WCHAR setting[66], ULONG index, WCHAR* out, ULONG out_len)` | section/setting 名超 64 WCHAR 截断（c:1438-1456 内部 x_section[66]）。`index` 低 24 位 = 值序号（CONF_INDEX_MASK，`api_flags.h:30`），高 8 位可 OR `CONF_GET_NO_GLOBAL`(0x40000000)/`CONF_GET_NO_TEMPLS`(0x10000000)/`CONF_GET_NO_EXPAND`(0x20000000)/`CONF_JUST_EXPAND`(0x80000000)/`CONF_GET_PROPERTY`(0x01000000)（`api_flags.h:30-39`）。**允许沙箱内调用**（扩展用调用者 box 的 expand_args，`conf.c:1890-1905`）。枚举节：section=setting=NULL 且 `CONF_GET_NO_TEMPLS|CONF_GET_NO_EXPAND`（`SbieAPI.cpp:1180-1182`） |
| `SbieApi_QueryConfBool/Number/Number64` (h:374-390) | 便捷封装 | AsIs + y/n/_wtoi 语义（c:1503-1577） |
| `SbieApi_UpdateConf` (h:356) | `ULONG (ULONG op, section, setting, value)` | `op`: `CONF_UPDATE_VALUE=1`/`CONF_APPEND_VALUE=2`/`CONF_REMOVE_VALUE=4`/`CONF_REMOVE_SECTION=5`（`common\defines.h:59-63`）。**非沙箱限定**（`conf.c:2145-2146` → `STATUS_ACCESS_DENIED`）且**调用者须为会话 leader 或 SbieSvc**（`conf.c:2147-2150`）。只改**驱动内存缓存**，不落盘 —— 落盘必须走 SbieSvc `MSGID_SBIE_INI_*`（03 文档）。QSbieAPI 仅将其用于"改驱动侧配置后立即生效"的旁路（`SbieIniSetDrv`，`SbieAPI.cpp:1294-1336`）；本项目默认统一走 SbieSvc |
| `SbieApi_ReloadConf` (h:353) | `LONG (ULONG session_id, ULONG flags)` | 重读 ini 进驱动缓存；flags: `SBIE_CONF_FLAG_RECONFIGURE=0x1`。非沙箱限定（`conf.c:1701-1702`）。session_id 传 `-1`(当前) |

### 3.5 会话与日志

| 导出 | 原型 | 语义/限制 |
|---|---|---|
| `SbieApi_SessionLeader` (h:191) | `LONG (ULONG session_id, HANDLE* ProcessId)` | `ProcessId != NULL` → 查询某会话 leader（session_id=-1 时用 token_handle 查询，见 session.c:364-388）；`ProcessId == NULL` → **设置自己为当前会话 leader**，要求非沙箱（`session.c:349-351` → `STATUS_NOT_IMPLEMENTED`）；已有他人 leader → `STATUS_DEVICE_ALREADY_ATTACHED`（session.c:354-356）。QSbieAPI `TakeOver()` 全零参数调 IOCTL 即 set 路径（`SbieAPI.cpp:819-831`；注意它走原始 parms，因为 SbieApi_SessionLeader 封装的查询路径不同） |
| `SbieApi_GetMessage` (h:69) | `ULONG (ULONG* msg_num, ULONG session_id, ULONG* msgid, ULONG* pid, WCHAR* buf, ULONG len)` | 日志泵：msg_num 进出（游标，从 0 开始每会话独立）；一条记录 = msgid + 多段 `\0` 分隔 WCHAR 串（`SbieAPI.cpp:2438-2534` 解析）。**非沙箱 + 本会话 leader 限定**（`api.c:725-733`）。缓冲建议 4KB。特殊 msgid：低 16 位 `1399`=进程启动通知（数据[1]=镜像路径,[2]=box,[3]=父pid,[4]=cmdline）、`2199`=自动恢复通知（`SbieAPI.cpp:2510-2522`）。`ProcessId==4` 表示来自驱动本身 |
| `SbieApi_Log/LogEx/vLogEx/LogMsgEx/LogMsgExt` (h:78-91) | 写日志 | CLI 仅在诊断时用；`API_LOG_MESSAGE_MAX_LEN=800`（`api_defs.h:76`） |

### 3.6 监控/trace

| 导出 | 原型 | 语义/限制 |
|---|---|---|
| `SbieApi_MonitorControl` (h:205) | `LONG (ULONG* set, ULONG* get)` | 全局 trace 开关 |
| `SbieApi_MonitorGetEx` (h:248) | 单条取（旧 ABI 兼容） | |
| （无导出）`API_MONITOR_GET2` | — | 批量取：**SbieDll.dll 未导出包装**（导出表核对无 `SbieApi_MonitorGet2`）。两条路：(a) `SbieApi_Ioctl` 直接投 `API_MONITOR_GET2` parms（结构 `API_MONITOR_GET2_ARGS {WCHAR* buffer_ptr; ULONG* buffer_len;}`，`api_defs.h:372-375`）；(b) `SbieApi_Call(API_MONITOR_GET2, 2, buf, &len)`。批量缓冲区为记录数组：`[ULONG size][LONGLONG timestamp]…`（`SbieAPI.cpp:3041-3060` 解析）。本项目选 (a) |

### 3.7 其他（SbieDll_* 前缀，同样 GetProcAddress）

| 导出（`sbiedll.h` 行号） | 用途 |
|---|---|
| `SbieDll_PortName` (h:136) | 返回 LPC 端口名常量 `\RPC Control\SbieSvcPort`（= `QSbieAPI\SbieDefs.h:8` 的 `SBIESVC_PORT`；服务端 `PipeServer.cpp:263` 用同名创建）。避免我们硬编码 |
| `SbieDll_RunStartExe` (sbieapi.h:437) | 组装 `/box:<name> <cmd>` 并从安装目录启动 `Start.exe`（c:2122-2164）。`sbie proc start` 的**降级路径**（无 SbieSvc 时）；首选仍是 SbieSvc `MSGID_PROCESS_RUN_SANDBOXED` |
| `SbieDll_RunFromHome` (h:193) | 从 Sandboxie 安装目录 CreateProcess；server 拉起/升级器不用 |
| `SbieDll_KillOne/KillAll` (h:107-110) | 经 SbieSvc 杀进程；本项目直接走 `MSGID_PROCESS_KILL_ONE/ALL`（03），不绑这两个 |
| `SbieDll_FormatMessage0/1/2` (h:118-124) | 从 `SbieMsg.dll`（安装目录，`LOAD_LIBRARY_AS_DATAFILE`，`SbieAPI.cpp:344`）格式化 SBIE 错误文案。`sbie log watch` 输出人类可读消息用 |

**不绑定**（属于沙箱进程内 hook 基础设施或本项目无对应功能）：`SbieDll_Hook*`、`SbieDll_Inject*`、`SbieDll_Match*`、`SbieDll_Com*`、`SbieDll_Queue*`（队列走我们自己的 LPC 客户端，见 03）、`SbieApi_HookTramp`、`SbieApi_RenameFile/OpenFile/OpenKey/...`（SbieSvc 专用语义）。

## 4. 参数结构体布局（vendor `api_defs.h` 后的 ABI 事实）

- 宏 `API_ARGS_BEGIN/FIELD/CLOSE`（`api_defs.h:202-204`）生成：`func_code` 后跟若干
  `union { ULONG64 val64; T val; }` —— **每个字段定长 8 字节**，结构体总长
  `8 × API_NUM_ARGS = 64` 字节，8 字节对齐（`__declspec(align(8))` 调用侧，c:201 等）。
- 指针字段一律传**用户态地址**；驱动侧 ProbeForRead/Write。字符串出参走两层：
  指向 `UNICODE_STRING64` 的指针（`{USHORT Length; USHORT MaximumLength; ULONG64 Buffer;}`
  ——注意该结构体在 core 中定义为 16 字节布局，`Buffer` 为 64 位指针，见
  `Sandboxie\common\win32_ntddk.h` 的 `UNICODE_STRING64`）。
- vendor 清单（直接复制，不改）：`api_defs.h` 全文件、`api_flags.h` 全文件、
  `common\defines.h` 中的 `BOXNAME_COUNT`/`CONF_LINE_LEN`/`CONF_*` 操作码、
  `win32_ntddk.h` 中 `UNICODE_STRING64`/`MAX_PORTMSG_LENGTH` 相关段落。
  这些是 GPLv3 core 头，允许 vendor（见 01-license-map §4）。

## 5. 返回码语义

- 全部 `NTSTATUS`（LONG）。`NT_SUCCESS(rc)`（>=0）为成功；`SbieApi_QueryProcessInfo*`
  例外：成功返回值即数据，失败返回 0。
- 常见值（本项目必须显式翻译成 CLI 退出码/错误文案）：

| NTSTATUS | 含义 | CLI 呈现 |
|---|---|---|
| `STATUS_SUCCESS` (0) | 成功 | — |
| `STATUS_UNSUCCESSFUL` (0xC0000001) | 服务端泛失败兜底（SbieSvc SHORT_REPLY） | 退出码 1 |
| `STATUS_SERVER_DISABLED` (0xC0000080) | 驱动不存在（c:131-133 映射）| "驱动未运行" 退出码 3 |
| `STATUS_ACCESS_DENIED` (0xC0000022) | 权限不足（如非 leader 调 UPDATE_CONF） | 退出码 6 |
| `STATUS_NOT_IMPLEMENTED` (0xC0000002) | 调用方在沙箱内（多处 `if (proc)` 守卫） | "不能在沙箱内运行" 退出码 6 |
| `STATUS_OBJECT_NAME_INVALID` (0xC0000033) | 名字非法 | 退出码 5 |
| `STATUS_OBJECT_NAME_NOT_FOUND` (0xC0000034) | pid/box/节不存在 | 退出码 5 |
| `STATUS_RESOURCE_NAME_NOT_FOUND` (0xC000008B) | **配置值不存在**（`Conf_Api_Query` 无值返回，conf.c:1885-1887；`cfg get` 缺键走此值） | "setting not found" 退出码 5 |
| `STATUS_DEVICE_ALREADY_ATTACHED` (0xC0000038) | 会话已有 leader（Session_Api_Leader） | server 启动失败提示 |
| `STATUS_ACCOUNT_RESTRICTION` (0xC000006E) | box 存在但 `Enabled=n`（QSbieAPI IsBox 约定） | box disabled |
| `STATUS_WRONG_PASSWORD` (0xC000006A) | SbieSvc EditPassword 密码错 | 退出码 6 |
| `STATUS_LOGON_NOT_GRANTED` (0xC0000155) | SbieSvc：非管理员改他人节 | 退出码 6 |
| `STATUS_NOT_SUPPORTED` (0xC00000BB) | SbieSvc：调用方在沙箱内 | 退出码 6 |
| `STATUS_BUFFER_TOO_SMALL` (0xC0000023) | 出参缓冲不足（EnumBoxes 跳过逻辑，c:1608） | 加大缓冲重试（内部折叠不外泄） |

- 错误文案：优先 `SbieDll_FormatMessage*`（SBIE xxxx 消息表，msgid 见 `Sandboxie\msgs`），
  回退 `FormatMessage(FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandleW(L"ntdll.dll"), …)`
  （QSbieAPI `CSbieAPI__FormatNtStatus` 的做法，`SbieAPI.cpp:1213-1229`，注意负值先
  `RtlNtStatusToDosError`）。

## 6. "哪些调用要求进程非沙箱 / 需要 SbieSvc 辅助" 汇总

| 操作 | 非沙箱 | 会话 leader | 需 SbieSvc |
|---|---|---|---|
| GetVersion/QueryDrvInfo/GetHomePath | — | — | — |
| QueryConf（读） | 允许沙箱内 | — | — |
| EnumBoxes/IsBoxEnabled/QueryBoxPath/QueryProcess*/EnumProcessEx/QueryProcessInfo | — | — | — |
| ReloadConf | 要求（conf.c:1701） | — | — |
| UpdateConf | 要求（conf.c:2145） | 要求（conf.c:2147-2150） | 否（但落盘要 SbieSvc） |
| GetMessage（日志） | 要求（api.c:725） | 要求读自己会话（api.c:729-733） | — |
| SessionLeader set | 要求（session.c:349） | — | — |
| MonitorControl/MonitorGet2 | （未显式守卫；GetMessage 模式类推） | — | — |
| ProcessExemptionControl | 要求（api.c:1118） | — | — |
| Set/GetSecureParam | 要求（api.c:1245/1347） | — | — |
| 启动沙箱进程 | — | — | **是**（`MSGID_PROCESS_RUN_SANDBOXED`，令牌/设备映射由 SbieSvc 处理；无 SbieSvc 时降级 `SbieDll_RunStartExe`） |
| 杀进程/挂起 | — | — | **是**（`MSGID_PROCESS_KILL_*` / `SUSPEND_RESUME_*`） |
| ini 落盘修改 | —（SbieSvc 侧拒绝沙箱调用者，03 文档） | — | **是** |

因此：**sbie-cli server 与 client 都不得在沙箱内运行**；启动时自检（`SbieApi_QueryProcess(GetCurrentProcessId(),NULL,NULL,NULL,NULL)` 返回成功 ⇒ 自己在沙箱内 ⇒ 报错退出——与 SbieSvc `SbieIniServer::Handler2` 同款检测，`sbieiniserver.cpp:126-127`）。

## 7. 坑记录

1. **`API_MONITOR_GET2` 无 SbieDll 导出**：初稿以为所有 API 码都有 SbieApi_ 包装；
   以 5.73.5 x64 `SbieDll.dll` 导出表核对，`SbieApi_MonitorGet2` 不存在（导出列表止于
   `MonitorGetEx`）。批量 trace 取数改走 `SbieApi_Ioctl`/`SbieApi_Call`，已写入 §3.6。
2. **`SbieApi_SessionLeader` 的封装与 QSbieAPI 用法不一致**：封装版 (h:191) 的 `ProcessId==NULL`
   走"设置 leader"需要 session_id 参数被忽略（c:1966-1973 置 0）；QSbieAPI 的 TakeOver 直接
   零参数 IOCTL。绑定封装版即可，但**查询 leader** 时要传 `ProcessId!=NULL` + session_id，
   `session_id==-1` 分支需要 token_handle（c:1968 注释），本项目不用该分支。
3. **`STATUS_SERVER_DISABLED` 数值（TODO-VERIFY 已解除）**：初稿凭记忆写 0xC0000080，
   2026-09-27 以本机 Windows SDK `10.0.26100.0\shared\ntstatus.h` 实读核实——**正确**：
   `STATUS_SERVER_DISABLED = 0xC0000080L`。同时核实的错误翻译表常数（已入
   `SbieCore/Util/Status.cpp`，勿改）：`NOT_IMPLEMENTED=0xC0000002`、
   `ACCESS_DENIED=0xC0000022`、`BUFFER_TOO_SMALL=0xC0000023`、
   `OBJECT_NAME_INVALID=0xC0000033`、`OBJECT_NAME_NOT_FOUND=0xC0000034`、
   `DEVICE_ALREADY_ATTACHED=0xC0000038`（注意：不是 0xC000036C）、
   `WRONG_PASSWORD=0xC000006A`、`ACCOUNT_RESTRICTION=0xC000006E`、
   `NOT_SUPPORTED=0xC00000BB`、`LOGON_NOT_GRANTED=0xC0000155`。
   第三波（2026-09-27）补核实并入表：`UNSUCCESSFUL=0xC0000001`（:2074）、
   `RESOURCE_NAME_NOT_FOUND=0xC000008B`（:3354，驱动"值不存在"，
   conf.c:1885-1887；§5 表已同步补齐上述已核实全部常量）。
4. **`SbieApi_QueryProcessInfo` 失败返回 0 与"flags 恰为 0"不可区分**——QSbieAPI 同样含糊
   （`SbieAPI.cpp:1743-1761`）。`sbie proc info` 输出 flags 时需配合 `SbieApi_QueryProcessEx2`
   的成功与否判断进程是否存在。
5. `SbieApi_EnumProcessEx` 的 pids 缓冲：驱动端上限 `API_MAX_PIDS=512`（api_defs.h:78），
   QSbieAPI 先取 count 再分配（count+128 余量，`SbieAPI.cpp:1503-1517`）——本项目沿用两段式。
6. **（M1 实测）`create_time` 单位**：`SbieApi_QueryProcessEx2` 的 `out_create_time` 即
   `PsGetProcessCreateTimeQuadPart`（core\drv\process.c:662）——100ns 单位、1601 纪元，
   可直接 FILETIME→FileTimeToLocalFileTime→SystemTime 转本地时间（sbie-cli proc list
   实跑输出 `2026-09-27 04:51:16` 验证）。
7. **（M1 实测）`SbieApi_GetVersionEx` 无驱动也能成功返回**：驱动不在场时它写
   `L"unknown"`/abi=0 并返回错误码——薄封装按"返回值判错、version 字段供显示"用即可；
   判断"驱动是否在场"必须用 NtOpenFile(\Device\SandboxieDriverApi)（DriverAlive 的实现）。
8. **（M1 实测）绑定表全 32 项导出在本机 5.73.5 x64 SbieDll.dll 上全部存在**，含
   `SbieDll_TranslateNtToDosPath`（02 §3.7 未列、为 PathMapper 附加绑定的那一项）。
