# 04 — 模块划分与 CLI 命令规格

功能面来源：`SandboxiePlus\QSbieAPI\SbieAPI.h`（LGPL，公共方法全集，:33-332）、
`QSbieAPI\Sandboxie\SandBox.h`（沙箱/快照操作，:36-127）、`QSbieAPI\Sandboxie\BoxedProcess.h`、
`QSbieAPI\SbieUtils.h`（组件启停）、`QSbieAPI\Sandboxie\SbieTemplates.cpp`（模板）。
**SandMan 自身源码（custom license）未读、不参考**；其功能面经由上述 LGPL/GPL 接口与
`Sandboxie\install\Templates.ini`（GPLv3 资产）确定。

## 1. 模块边界与依赖方向

```
sbie-cli.vcxproj (exe)
 ├─ cli/            命令行解析、路由、自动拉起、输出格式化     ──► 依赖 SbieCore + ipcc
 ├─ server/         管道监听、会话 leader、日志泵、队列泵       ──► 依赖 SbieCore + ipcc
 └─ ipcc/           client↔server 帧协议（SbieIpc.h，唯一共享头）
        │
        ▼
SbieCore.vcxproj (静态库, 无 UI 依赖, 可单测)
 ├─ DriverApi/      SbieDll.dll 动态绑定 + 驱动调用封装（02）
 ├─ SvcClient/      SbieSvc LPC 客户端（专用线程）（03）
 ├─ QueueClient/    MSGID_QUEUE_* 封装 + interactive queue（03 §5-6）
 ├─ Model/          Box/Process/Snapshot/Template/Config 领域逻辑（纯用户态）
 └─ Util/           Json(手写最小实现)、UTF8/UTF16、TablePrinter、Status/ExitCode、PathMapper(Nt↔Dos)
```

规则：

- `SbieCore` **不包含**管道服务器/客户端与命令行解析（可在无 IPC 环境下单元测试）。
- `sbie-cli` 两个入口共享 `ipcc/SbieIpc.h` 与 JSON schema；**禁止**让 server 直接调用
  cli/ 的格式化代码（client 输出格式化在 client 进程做，server 只回 JSON）。
- 所有跨模块调用以 `SbieStatus`（`Util/Status.h`）返回，`NTSTATUS`/win32 错误包装其中，
  到 cli 层再映射退出码（§6）。

## 2. 模块对外接口契约（多 agent 并行开发的分界线）

以下签名是**契约**：实现可换，签名/语义冻结。命名空间 `sbie::`。

### 2.1 `SbieCore/DriverApi/DriverApi.h`

```cpp
namespace sbie::drv {
  // 进程启动时调用一次；失败后所有 Api() 调用返回 ERR_SBIEDLL
  bool LoadSbieDll(const std::wstring& explicitDir = L"");
  bool Loaded();
  struct VersionInfo { std::wstring version; ULONG abi; };   // "5.73.5" / 0x57230
  VersionInfo GetVersion();                                   // SbieApi_GetVersionEx

  // 绑定表（02 §1 规范）。所有驱动经由此结构调用，禁止散落 GetProcAddress。
  struct Api { /* SbieApi_* 函数指针成员，名字同导出名 */ };
  Api* ApiP();

  bool InSandbox();          // SbieApi_QueryProcess(self) 成功 => 在沙箱内 => 全部命令拒绝
  bool DriverAlive();        // 尝试打开 \Device\SandboxieDriverApi；false = 未装/未跑
}
```

### 2.2 `SbieCore/SvcClient/SvcClient.h`

```cpp
namespace sbie::svc {
  // 单实例。专用线程串行执行所有 LPC 请求（03 §1 线程亲和）。
  class SvcClient {
  public:
    static SvcClient& Instance();
    bool Connected();                        // 惰性连接 \RPC Control\SbieSvcPort
    // req 指向含 MSG_HEADER 的结构；返回 malloc 的完整回复（调用方 free）；
    // 传输错误置 status=ERR_SVC_TRANSPORT 并内部重连一次。
    SbieStatus Call(const void* req, size_t reqLen, void** outRpl, size_t* outRplLen);
  };
  // 便捷层（内部组装 vendored 结构体）：
  SbieStatus IniGetUser(bool* admin, std::wstring* section, std::wstring* name);
  SbieStatus IniGetPath(std::wstring* path, bool* isHome);
  SbieStatus IniGetVersion(std::wstring* version, ULONG* abi);
  enum class SetMode { Update, Append, Insert, Delete };     // 对应 0x1811-0x1814
  SbieStatus IniSetSetting(const std::wstring& section, const std::wstring& setting,
                           const std::wstring& value, SetMode mode, bool refresh,
                           const std::wstring& password);
  SbieStatus IniGetSetting(const std::wstring& section, const std::wstring& setting,
                           std::wstring* value);
  SbieStatus SetPassword(const std::wstring& oldPw, const std::wstring& newPw);
  SbieStatus TestPassword(const std::wstring& pw);
  // ProcessServer（03 §4）
  SbieStatus KillOne(ULONG pid);
  SbieStatus KillAll(const std::wstring& box, ULONG sessionId = (ULONG)-1);
  SbieStatus SuspendResume(ULONG pid, bool suspend);
  SbieStatus SuspendResumeAll(const std::wstring& box, bool suspend);
  struct ProcInfo { ULONG parentId, flags; bool suspended;
                    std::wstring image, cmdline, workdir; };
  SbieStatus GetProcInfo(ULONG pid, unsigned infoClasses /*1|2|4*/, ProcInfo* out);
  struct RunResult { HANDLE hProcess; ULONG pid; };          // hProcess 由调用方 CloseHandle
  SbieStatus RunSandboxed(const std::wstring& box, const std::wstring& cmd,
                          const std::wstring& dir, ULONG creationFlags, RunResult* out);
}
```

### 2.3 `SbieCore/QueueClient/QueueClient.h`

```cpp
namespace sbie::queue {
  class QueueClient {           // "*MANPROXY_%08X"（会话 id 十六进制大写，03 §6）
  public:
    SbieStatus Create(const std::wstring& name, HANDLE* outEvent);
    SbieStatus GetReq(const std::wstring& name, ULONG* clientPid, ULONG* reqId,
                      std::vector<uint8_t>* data);
    SbieStatus PutRpl(const std::wstring& name, ULONG reqId, const void* data, ULONG len);
  };
}
```

### 2.4 `SbieCore/Model/*`（领域逻辑；快照/磁盘扫描是纯文件系统操作）

```cpp
namespace sbie::model {
  // Boxes.h -------------------------------------------------------------
  struct BoxInfo { std::wstring name; bool enabled; bool exists;
                   std::wstring fileRoot, regRoot, ipcRoot;   // QueryBoxPath 两段式
                   bool hasProcesses; };
  class BoxRepository {
  public:
    explicit BoxRepository(sbie::drv::Api* api, sbie::svc::SvcClient& svc);
    std::vector<std::wstring> EnumSections();                  // 含 Template_*/UserSettings_*（枚举节）
    std::vector<BoxInfo> EnumBoxes(bool enabledOnly);          // EnumBoxesEx + IsBoxEnabled
    SbieStatus GetInfo(const std::wstring& name, BoxInfo* out);
    SbieStatus Create(const std::wstring& name);   // ValidateName + SET "Enabled"="y"（行为对齐
                                                   // QSbieAPI CreateBox，SbieAPI.cpp:1459-1477）
    SbieStatus Rename(const std::wstring& oldN, const std::wstring& newN); // 复制节+删旧节（refresh 后）
    SbieStatus Delete(const std::wstring& name, bool delFiles, bool delSection);
    SbieStatus SetEnabled(const std::wstring& name, bool on);
    SbieStatus TerminateAll(const std::wstring& name);
    // 名称校验（对齐 ValidateName，SbieAPI.cpp:1412-1445）：<=38 WCHAR；仅 [A-Za-z0-9_]；
    // 非 aux/con/nul/prn/com0-9/lpt0-9/clock$；非 GlobalSettings / UserSettings_ 前缀
    static SbieStatus ValidateName(const std::wstring& name);
  };

  // Processes.h ---------------------------------------------------------
  struct ProcEntry { ULONG pid; std::wstring box, image; ULONG sessionId;
                     ULONG64 createTime; ULONG flags; };
  class ProcessRepository {
  public:
    std::vector<ProcEntry> Enum(bool allSessions, const std::wstring& box = L"");
    SbieStatus Info(ULONG pid, svc::ProcInfo* out);
    SbieStatus Kill(ULONG pid);  SbieStatus KillBox(const std::wstring& box);
    SbieStatus Suspend(ULONG pid); SbieStatus Resume(ULONG pid);
    SbieStatus Start(const std::wstring& box, const std::wstring& cmd,
                     const std::wstring& dir, bool elevated, svc::RunResult* out);
  };

  // Snapshots.h（文件布局对齐 SandBox.cpp:353-505 行为：Snapshots.ini + snapshot-<ID>/）
  struct SnapshotInfo { std::wstring id;           // 十进制字符串，从 1 递增
                        std::wstring parentId, name, info; ULONGLONG date; };
  class SnapshotManager {
  public:
    explicit SnapshotManager(const BoxInfo& box);
    std::vector<SnapshotInfo> List(std::wstring* currentId, std::wstring* defaultId);
    bool HasAny();
    SbieStatus Take(const std::wstring& name);              // 要求沙箱内无进程
    SbieStatus Remove(const std::wstring& id);
    SbieStatus Select(const std::wstring& id);              // 切换当前（要求无进程）
    SbieStatus SetInfo(const std::wstring& id, opt name, opt info);
  };

  // ConfigStore.h -------------------------------------------------------
  class ConfigStore {
  public:
    // 读：SbieApi_QueryConf（驱动缓存，无 SbieSvc 也能读）
    std::optional<std::wstring> Get(const std::wstring& section, const std::wstring& setting,
                                    ULONG index = 0, bool noExpand = true, bool noTemplates = true);
    std::vector<std::wstring> GetList(/*同上*/);            // index 递增到失败
    std::vector<std::wstring> ListSettings(const std::wstring& section); // 枚举 setting 名
    SbieStatus Set/SetAppend/SetInsert/Delete(…);           // 全部经 SvcClient（落盘+refresh）
    SbieStatus Reload(bool reconfigureDrv);                 // SbieApi_ReloadConf(-1, flag)
    SbieStatus Path(std::wstring* out, bool* isHome);
    bool Locked();                                          // GlobalSettings/EditPassword 非空
  };

  // Templates.h（激活方式已核实：box 节多值设置 "Template=<名>"，驱动端 Conf_Merge_Templates
  //             按 section->settings_map[L"Template"] 合并，drv/conf.c:1239-1249,172-174；
  //             模板本体 = [Template_<名>] 节，类目 = 其 Tmpl.Class 值，SbieTemplates.cpp:334-356）
  struct TemplateInfo { std::wstring name, clazz, descr; };
  class TemplateRegistry {
  public:
    std::vector<TemplateInfo> List(const std::wstring& clazzFilter /*L"*"=全部*/);
    SbieStatus Info(const std::wstring& name, std::vector<std::pair<…>>* settings);
    SbieStatus Apply(const std::wstring& box, const std::wstring& tmpl);   // Append "Template"
    SbieStatus Revoke(const std::wstring& box, const std::wstring& tmpl);  // Delete 该值
    std::vector<std::wstring> Applied(const std::wstring& box);
  };

  // BoxUsage.h（磁盘占用：递归统计沙箱根目录；行为=资源管理器"大小"，见 01 §2 禁 BoxMonitor）
  SbieStatus ScanBoxSize(const std::wstring& fileRoot, ULONGLONG* bytes,
                         const std::function<bool(ULONGLONG)>& progress /*可取消*/);
  // ↑ box size 波次的 additive 扩展（docs/04 §14 登记，原签名保留）：
  struct BoxUsageStats { u64 total_bytes, files, dirs; };
  SbieStatus ScanBoxSize(const std::wstring& fileRoot, BoxUsageStats* out,
                         const std::function<bool(ULONGLONG)>& progress);

  // Recovery.h（文件恢复，P0-12；docs/04 §14 登记——行为基准 GPL core 可参考）
  struct RecoverEntry { wstring sandboxPath, boxPath, targetPath; u64 size; };
  class RecoveryManager {
    explicit RecoveryManager(const BoxInfo& box);
    vector<RecoverEntry> List();       // 扫 RecoverFolder 声明目录（GetRealPath
                                       // 规则映射，排序+去重）
    bool MapToRealPath(sandboxPath, wstring* out);   // \drive\X→X:\、\user\*、
                                       // \share→UNC、剥 \snapshot-*
    SbieStatus Copy(paths[], toDir, overwrite, RecoverCopyOutcome* out);
  };
  SbieStatus RecoverAddFolder(box, folder, password);   // 追加 RecoverFolder 值

  // Maintenance.h（组件维护，P1-2；docs/04 §15 登记的新增 Model 文件）
  enum class Component { Driver, Service };             // SbieDrv / SbieSvc
  struct ComponentStatus { bool installed, running; wstring state; };
  SbieStatus QueryComponent(Component c, ComponentStatus* out);  // SCM 查询
  SbieStatus StartComponent(Component c);   // Service=StartService；Driver=
                                            //   KmdUtil.exe start（RunFromHome）
  SbieStatus StopComponent(Component c);    // Service=ControlService(STOP)+等待；
                                            //   Driver=KmdUtil.exe stop
  // 只在显式命令时调用；启停需管理员（rc 6）
}
```

### 2.5 `sbie-cli/ipcc/SbieIpc.h`（client↔server 唯一共享协议头）

```cpp
// 帧格式（管道字节流，小端）：
//   struct Frame { u32 magic = 'SBOS'; u32 version = 1; u32 msgid; u32 payloadLen;
//                  u8  payload[payloadLen]; }
// payload 恒为 UTF-8 JSON 对象。
// 请求:  {"op":"<OpName>", "params":{…}}
// 回复:  成功 {"ok":true, "data":{…}}；失败 {"ok":false,
//        "error":{"code":<int 退出码语义>, "message":"…", "ntstatus":"0xC0000022"(可选)}}
// msgid：每连接自增的关联号，回复回填同值。
constexpr uint32_t kIpcMagic = 0x53424F53;  // 'SBOS'
// OpName 全集 = §5 命令树的 "ipc op" 列；server 按 op 分派到 Model 层。
```

### 2.6 `sbie-cli/server/*` 与 `sbie-cli/cli/*`

- `server/ServerMain.cpp`：`--start-server` 入口；00 §4 双锁、§6 状态机。
  对外契约：`int RunServer(const ServerOptions&)`；`ServerOptions{ idleTimeoutSec=300 }`。
- `server/Dispatcher.cpp`：op → Model 调用 → JSON。**每个 op 的 handler 独立函数**，
  注册表 `std::map<std::string, Handler>`，新命令只加注册项。
- `server/LogPump.cpp`：`API_GET_MESSAGE` 泵（线程）；日志事件写入环形缓冲并向订阅连接
  推送 `{"op":"log.event"}` 帧（仅 `log watch` 在线订阅时）。
- `cli/Cli.cpp`：argv 解析（位置参数 + `-`/`--` 选项，`--json/--quiet/--password <pw>`）；
  自动拉起（00 §5）；降级直连判定表 §5 注。
- `cli/Output.cpp`：`TablePrinter`（默认人类可读）与 `--json` 二选一；JSON 由
  `Util/Json` 生成。

## 3. 通用选项（所有命令）

| 选项 | 说明 |
|---|---|
| `--json` | 输出 JSON（规范 §7）到 stdout |
| `--quiet` / `-q` | 只输出数据，无表头/装饰 |
| `--password <pw>` | 等同设置 `SBIE_PASS`（优先级：选项 > 环境变量） |
| `--no-server` | 不自动拉起 server；能降级则降级，否则退出码 4 |
| `--no-refresh` | 仅 `box/cfg set` 类：置 `SBIE_INI_SETTING_REQ.refresh=false` |
| `--help` / `-h` | 用法，stdout，退出码 0 |

## 4. 完整命令树

```
sbie status / version
sbie server  start|stop|status
sbie box     list [--type <t>]|info|create [--type <t>] [--location <dir>] [--temp]|
             [--v2-delete] [--auto-recover] [--block-net] [--drop-admin]|
             delete|rename|enable|disable|set|get|list-setting|clean|size|
             copy|export|import|types|
             dump <name>|explore <name>|recover (list|copy|add)|
             snapshot (list|take|remove|select|set-info|default)
sbie proc    list|info|start|kill|kill-all (<box>|--all)|suspend|resume|
             suspend-box|resume-box (<box>|--all)|exempt <pid> <on|off|get>
sbie cfg     get|set|unset|list-setting|reload|path|lock|unlock|whoami|dump [<section>]
sbie template list|info|apply|revoke|check|gen-browser
sbie log     watch|dump
sbie trace   watch|dump
sbie force   on [<seconds>]|off|status
sbie maint   status|start|stop|install|uninstall [--driver|--service|--all]
sbie img     list|status [<box>]|create <box> --size-mb <N>|mount <box>|unmount <box>
sbie ramdisk status
sbie usb     status|sync [--dry-run]
sbie doctor
```
（D1/D3 波次命令详表：trace §20、box/proc/cfg/template/maint/doctor 增强与新命令 §19）
【08 审计回填：本树补 box copy/export/import/types（波 B/D3，§17/§19）；原树误列的
snapshot info 为幽灵动词——实现无此 verb（实测 `box snapshot info` → USAGE，动词集
list/take/remove/select/set-info/default），已删。来源 docs\08 §3.1。】

每命令规格如下。`--json` 时 "输出" 列的表格数据改为 §7 的 JSON 对象数组；退出码列仅列
特异值，通用失败见 §6。所有命令共有的错误：`3`（驱动不可用，读类）、`4`（server 不可用
且不可降级）、`6`（在沙箱内运行 / 密码错误 / 权限不足）。

### 4.1 全局

| 命令 | 参数 | ipc op | 行为/降级 | 输出（默认表格） | 退出码 |
|---|---|---|---|---|---|
| `sbie status` | — | `status` | 可降级直连 | 三行：driver(version/abi/alive)、service(connected/version)、server(running/pid/uptime) | 0 |
| `sbie version` | — | `version` | 降级直连 | `sbie-cli x.y.z` / `driver 5.73.5 (abi 0x57230)` / `svc 5.73.5` | 0 |

### 4.2 server

| 命令 | 参数 | 行为 | 输出 | 退出码 |
|---|---|---|---|---|
| `sbie server start` | `[--idle-timeout <sec>]` `[--no-guardians]`（08 审计回填：原表 `--idle` 为误记，实际旗标 `--idle-timeout`，server_cmd.cpp:66；`--no-guardians` 波 A，§16） | 本进程派生 `--start-server`（DETACHED）；已在跑则幂等成功 | `server started (pid 1234)` | 0；4=拉起失败/超时 |
| `sbie server stop` | — | 经管道发 `server.shutdown`（校验同用户） | `server stopped` | 0；5=未运行 |
| `sbie server status` | — | 探测管道+互斥体 | running/pid/clients/idle-remaining 或 `not running` | 0（not running 也是 0，供脚本判断用输出） |

### 4.3 box（沙箱）

| 命令 | 参数 | ipc op | 语义 | 输出（默认） | 退出码 |
|---|---|---|---|---|---|
| `sbie box list` | `[--all]`（兼容旗标，见 §13 注）/`[--json]` | `box.list` | EnumBoxes(Ex)+IsBoxEnabled，**恒枚举全部 box、不按 Enabled 过滤**（QSbieAPI GetAllBoxes 语义；disabled 沙箱仍列出，ENABLED 列 yes/no——验收回传修正，§13） | 表：NAME ENABLED ACTIVE_PROCS FILE_ROOT | 0 |
| `sbie box info <name>` | `[--paths]` | `box.info` | GetInfo | 键值行（name/enabled/fileRoot/regRoot/ipcRoot/hasProcesses） | 0；5=不存在 |
| `sbie box create <name>` | `[--template <tpl>]…` | `box.create` | ValidateName→Create→（可选 Apply 模板） | `box 'X' created` | 0；7=名字非法；5=已存在 |
| `sbie box delete <name>` | `[--files]` `[--keep-section]` | `box.delete` | 有进程先拒（提示 kill-all）；默认删节+删目录 | `box 'X' deleted` | 0；5；9=仍有进程（见 §6） |
| `sbie box rename <old> <new>` | — | `box.rename` | ValidateName(new)+Create(new)+复制设置+Delete(old 节) | `renamed` | 0；7；5 |
| `sbie box enable/disable <name>` | — | `box.setEnabled` | Enabled=y/n | — | 0；5 |
| `sbie box set <name> <setting> <value>` | `[--append|--insert|--index <i>]` `[--password <pw>]` | `box.set` → `svc::IniSetSetting` | 默认 Update；`--append` Add；`--insert` Ins | — | 0；6=密码/权限 |
| `sbie box get <name> <setting>` | `[--index <i>]` `[--raw]` | `box.get` | 驱动缓存读（可降级直连） | 值本身（多行=多 index） | 0；5=不存在 |
| `sbie box list-setting <name>` | `[--all]`（含模板注入项） | `box.listSetting` | 枚举 setting 名（CONF_GET_NO_TEMPLS 反向控制） | 每行一名 | 0；5 |
| `sbie box clean <name>` | — | `box.clean` | TerminateAll + 删沙箱内容（保留节） | `cleaned` | 0；9=有进程杀不掉 |
| `sbie box size <name>` | — | `box.size` | ScanBoxSize 递归统计 FileRoot（04 §14：同步执行，重解析点跳过=资源管理器"大小"口径） | 表：SIZE/BYTES/FILES/DIRS | 0；5 |
| `sbie box recover list <name>` | — | `recover.list` | 扫 RecoverFolder 声明目录（含模板注入、%env% 展开）下的沙箱内文件 | 表：#/PATH(相对 FileRoot)/TARGET/SIZE | 0；5 |
| `sbie box recover copy <name> <index\|all\|path...>` | `[--to <dir>]` `[--overwrite]` | `recover.copy` | CopyFileW 拷出（保留 mtime；拷贝语义非移动）；缺省恢复到原位，`--to` 保留 FileRoot 相对结构；目标存在默认报错 | `N file(s) recovered (M)` | 0；1=目标存在/文件错；5=选择子无匹配/越界 |
| `sbie box recover add <name> <folder>` | — | `recover.add` | 追加 RecoverFolder 值（%var%/DOS/UNC/NT 原样存储，04 §14）+ 写后回读 | `recover folder added: <值>` | 0；5；7=形态非法 |
| `sbie box snapshot list <name>` | — | `box.snap.list` | 读 `<FileRoot>\Snapshots.ini` | 表：ID NAME DATE CURRENT(*) DEFAULT(d) | 0；5 |
| `sbie box snapshot take <name> <snap-name>` | `[--info <text>]` | `box.snap.take` | 要求无活动进程 | `snapshot #3 taken` | 0；9=有进程 |
| `sbie box snapshot remove <name> <id>` | — | `box.snap.remove` | 同上 | `removed` | 0；5；9 |
| `sbie box snapshot select <name> <id>` | — | `box.snap.select` | 切换当前快照 | `switched` | 0；5；9 |
| `sbie box snapshot set-info <name> <id>` | `[--name …] [--info …]` | `box.snap.setInfo` | 改 Snapshots.ini 字段 | — | 0；5 |

> 【08 审计回填注】本表为波次前基础形态；波次 A/B/D 增改以详表为准：`box create`
> 类型预设与高级旗标（§17/§19）、`box clean`/`delete --files` 的 OnBoxDelete
> 触发器与 `--no-triggers`（§16）、`box recover copy` 的 `--move` 与 OnFileRecovery
> 检查器缺省执行/`--no-check`（§17——本表"拷贝语义非移动"句已由波 B 更新）、
> `box snapshot default`（§19）、`box copy/export/import/types`（§17/§19）。
> 来源：docs\08 §3.1 三方核对（--help/代码注册表 vs 本表）。

### 4.4 proc（进程）

| 命令 | 参数 | ipc op | 语义 | 输出 | 退出码 |
|---|---|---|---|---|---|
| `sbie proc list` | `[--box <name>]` `[--all-sessions]` | `proc.list` | EnumProcessEx + QueryProcessEx2（可降级直连） | 表：PID BOX IMAGE SESSION STARTED | 0 |
| `sbie proc info <pid>` | — | `proc.info` | GetProcInfo(7)+flags | 键值行 | 0；5 |
| `sbie proc start <box> <cmd…>` | `[--dir <d>]` `[--elevated]` `[--wait]` | `proc.start` | RunSandboxed（SbieSvc）；`--elevated` 降级走 `SbieDll_RunStartExe /elevated`；**`--dir` 缺省 = client 当前目录显式进 params**（P1-7，04 §15——两路径 cwd 语义一致） | 新 PID；`--wait` 附加退出码行 | 0；4；`--wait` 时透传子进程退出码 |
| `sbie proc kill <pid>` | — | `proc.kill` | KillOne | `killed` | 0；5 |
| `sbie proc kill-all <box>` | `[--all-sessions]` | `proc.killAll` | KillAll（**不查** ExcludeFromTerminateAll——QSbieAPI 单箱 TerminateAll 亦不查，08-P1-1 语义决策） | `n process(es) terminated` | 0；5 |
| `sbie proc kill-all --all` | `[--no-exceptions]`（08-P1-1 波 E） | `proc.killAll`（box 空，no_exceptions 透传） | EnumBoxes 循环 KillBox（P1-3，全局形态；仅启用 box——进程只可能运行于启用 box）；`ExcludeFromTerminateAll=y` 的 box 跳过（对齐 QSbieAPI TerminateAll，SbieAPI.cpp:1786-1792），`--no-exceptions` 全杀（对齐 bNoExceptions 逃生） | `n process(es) terminated (m box(es)), k box(es) skipped (ExcludeFromTerminateAll)`（无跳过时省后半） | 0 |
| `sbie proc suspend <pid>` / `resume <pid>` | — | `proc.suspend/resume` | SuspendResume | — | 0；5 |

### 4.5 cfg（全局配置）

| 命令 | 参数 | ipc op | 语义 | 输出 | 退出码 |
|---|---|---|---|---|---|
| `sbie cfg get <setting>` | `[--section <s>=GlobalSettings]` `[--index <i>]` | `cfg.get` | 驱动缓存读（可降级） | 值 | 0；5 |
| `sbie cfg set <setting> <value>` | `[--section <s>]` `[--append/--insert]` | `cfg.set` | IniSetSetting | — | 0；6 |
| `sbie cfg unset <setting>` | `[--section <s>]` `[--index <i>]` | `cfg.unset` | Del（index 缺省删整 setting） | — | 0；5 |
| `sbie cfg list-setting [section]` | — | `cfg.listSetting` | 枚举 setting 名 | 名单 | 0 |
| `sbie cfg reload` | `[--reconfigure]` | `cfg.reload` | SbieApi_ReloadConf(-1, SBIE_CONF_FLAG_RECONFIGURE?) | `configuration reloaded` | 0 |
| `sbie cfg path` | — | `cfg.path` | IniGetPath（可降级需 SbieSvc…否：GET_PATH 走 SbieSvc；无 server 时也拉起，见注） | 路径 + `(home|system)` | 0；4 |
| `sbie cfg lock <new-pw>` | — | `cfg.lock` | SET_PASSWORD | — | 0；7=密码>64 WCHAR |
| `sbie cfg unlock <pw>` | — | `cfg.unlock` | TEST_PASSWORD 后在本连接缓存密码 | — | 0；6 |

注：`cfg path` 与一切写操作必须经 SbieSvc，**不设降级**——无 server 时自动拉起（00 §5），
拉不起则 4。

### 4.6 template（模板）

| 命令 | 参数 | ipc op | 语义 | 输出 | 退出码 |
|---|---|---|---|---|---|
| `sbie template list` | `[--class <c>]` `[--all]`（08-P2-7 波 E） | `tpl.list`（all 透传） | 枚举 `[Template_*]` 节 + Tmpl.Class；默认滤 `Tmpl.Hide=y`（与 Plus 呈现对齐），`--all` 显示 | 表：NAME CLASS DESCRIPTION(Tmpl.Name) | 0 |
| `sbie template info <name>` | — | `tpl.info` | 列模板节全部键值（波 E 起真 handler，不再 Stub 降级） | 键值行 | 0；5 |
| `sbie template apply <box> <name>` | — | `tpl.apply` | box 节 Append `Template=<name>` | `applied` | 0；5=模板不存在；6 |
| `sbie template revoke <box> <name>` | — | `tpl.revoke` | 删该值 | `revoked` | 0；5 |
| `sbie template check <box>` | — | `tpl.check` | 列 box 已启用模板及来源（波 E 起真 handler：盘上三节分档 + TemplateRegistry 目录对照；--json 行含 exists 字段，表形态列同前） | 表：TEMPLATE SOURCE(config/DefaultTemplates) | 0；5 |

### 4.7 log（消息日志）

| 命令 | 参数 | ipc op | 语义 | 输出 | 退出码 |
|---|---|---|---|---|---|
| `sbie log watch` | `[--pid <p>]` `[--msg <id低16位>]` `[--follow]`（默认跟随） `[--raw]` | `log.watch` | 订阅 server 日志泵；server 必须已 set leader | 每行：`[hh:mm:ss] SBIE9999 pid text`（文案经 SbieDll_FormatMessage*，无则 raw） | 0；Ctrl-C 0；3/4 |
| `sbie log dump` | `[--last <n>=100]` | `log.dump` | 取 server 环形缓冲最近 n 条后退出 | 同上 | 0；5=缓冲空 |

### 4.8 force / maint（P1 清尾波次，04 §15）

| 命令 | 参数 | ipc op | 语义 | 输出（默认） | 退出码 |
|---|---|---|---|---|---|
| `sbie force on [<seconds>]` | `<seconds>` 0<s≤86400，缺省沿用现配置 | `force.set` | API_DISABLE_FORCE_PROCESS set=1（直驱动）；seconds 给定时先写 `GlobalSettings\ForceDisableSeconds`（QSbieAPI 同序） | `force process disabled for N second(s)` | 0；3；6；7=seconds 非法（0 也拒——ForceDisableSeconds=0 禁止禁用） |
| `sbie force off` | — | `force.set` | set=0（清除禁用时刻，立即恢复） | —（`--json` 给 message） | 0；3 |
| `sbie force status` | — | `force.status` | get 查询（Session_IsForceDisabled）；剩余秒 = server 记录的禁用时刻推算（跨进程触发 → 剩余未知） | `force process: disabled (N second(s) remaining)` / `…: normal (force enabled)` | 0；3 |
| `sbie maint status` | `[--driver\|--service\|--all]`（缺省 --all） | —（client 本地执行，机器级操作不走会话 server） | SCM 状态查询；驱动行附 DriverAlive 设备交叉核证 | 表：COMPONENT/STATE/INSTALLED/NOTE | 0 |
| `sbie maint start` | 同上 | — | SbieSvc=进程内 StartService；SbieDrv=`KmdUtil.exe start`（RunFromHome 拉起）；顺序 service→driver | 表：COMPONENT/RESULT/STATE | 0；5=未安装；6=需管理员 |
| `sbie maint stop` | 同上 | — | SbieSvc=ControlService(STOP)+等待落定；SbieDrv=`KmdUtil.exe stop`（KmdUtil 含驱动专属卸载序列）；顺序 service→driver | 同上 | 同上 |

注：force 的驱动开关（API_DISABLE_FORCE_PROCESS 经 SbieApi_Ioctl）**无 SbieSvc 依赖**——
唯 ForceDisableSeconds 写经 SbieSvc（同 `cfg set` 写路径）。maint 为机器级组件操作，
只在显式命令时执行（server 启动路径不做任何组件启停）。

### 4.9 img / ramdisk / usb（运维域波次 D2，04 §18）

| 命令 | 参数 | ipc op | 语义 | 输出 | 退出码 |
|---|---|---|---|---|---|
| `sbie img list` | — | `img.list` | SbieSvc MountManager 已挂载根枚举（IMBOX_ENUM；ImDisk 缺席 → 3） | 表：REG_ROOT | 0；3 |
| `sbie img status [<box>]` | `[<box>]` | `img.status` | 键面（UseFileImage/UseRamDisk/ConfidentialBox/EnableEFS/ForceProtectionOnMount）+ 镜像文件（`<FileRoot>.box` 存在性/大小）+ 挂载状态（IMBOX_QUERY；ImDisk 缺席 → "unknown"）聚合；带 box=单箱键值行，缺省=全箱表 | 键值行/表 | 0；5 |
| `sbie img create <box>` | `--size-mb <N>`(≥256) `[--password <pw>]`(AES) | `img.create` | IMBOX_CREATE：SbieSvc→ImBox 挂载-格式化-卸载，产物 `<FileRoot>.box` | `image created for box 'X' (N MB[, AES])` | 0；3；5；7 |
| `sbie img mount <box>` | `[--password <pw>]` `[--protect\|--no-protect]` `[--admin-only\|--no-admin-only]` `[--auto-unmount]` | `img.mount` | IMBOX_MOUNT：显式挂载 + FileRoot junction；--protect 缺省 = ForceProtectionOnMount 键，admin_only 缺省 = ProtectAdminOnly 键；加密箱必须显式 mount（自动路径不传口令） | `box 'X' image mounted` | 0；3；4；5 |
| `sbie img unmount <box>` | — | `img.unmount` | IMBOX_UNMOUNT（未挂载=幂等成功） | `box 'X' image unmounted` | 0；4 |
| `sbie ramdisk status` | — | `ramdisk.status` | 全局 RamDiskSizeKb（<100MB 标记拒挂）/RamDiskLetter + UseRamDisk=y 箱 + 共享盘 QUERY（空 reg_root） | 键值行 | 0 |
| `sbie usb status` | — | `usb.status` | ForceUsbDrives/UsbSandbox(缺省 USB_Box)/DisabledForceVolume/现 ForceFolder + 卷表（SN `HHHH-LLLL`/卷标/挂载点/USB 总线/是否接管；BusType=Usb 直查，非 SetupAPI） | 头四行 + 表：SERIAL/LABEL/MOUNTS/USB/TAKEN | 0 |
| `sbie usb sync` | `[--dry-run]` `[--password <pw>]` | `usb.sync` | 07-P2-1 一次性接管：枚举 USB 卷→建 UsbSandbox 箱（缺三键 UseFileDeleteV2/UseRegDeleteV2/UseVolumeSerialNumbers 幂等补 y）→ ForceFolder 整表替换（多写序列末条 refresh 提交，§8.4）；ForceUsbDrives=n 拒绝 | 摘要行 + 表：FORCE_FOLDER | 0；4；7 |

注：`box set UseFileImage=y / UseRamDisk=y` 写键即生效——SbieSvc 在箱内首进程
注入路径自动挂载/卸载（AcquireBoxRoot，04 §18.1），img mount/unmount 是显式面
（加密箱口令场景必需）。ImDisk 驱动与 ImBox.exe 属 SandboxieTools 运行时：缺席时
create/mount 报 3，status 呈现 "unknown (ImDisk driver not available)"。

## 5. 降级直连适用表（无 server 时 client 自行执行）

| 可降级 | 不可降级（必须拉起 server） |
|---|---|
| `status`、`version`、`box list/info/get/list-setting`、`proc list`、`cfg get/list-setting`、`template list/info/check`（读驱动缓存即可的部分）；`force on/off/status`（P1 清尾波次：开关=直驱动 ioctl，on 的 ForceDisableSeconds 写在直连路径经进程内 SvcClient，§15） | 一切 SbieSvc 写操作（`box create/set/rename/delete`、`cfg set/unset/reload?/lock/unlock`、`template apply/revoke`）、`proc start/kill*`、`cfg path`、`log watch/dump`（需 leader） |

`img list/status`、`ramdisk status`、`usb status`（读：键面走驱动缓存，ImBox QUERY 经进程内 SvcClient；04 §18）

D1/D3 波次（04 §19/§20）：`trace watch/dump`（订阅被拒或 --no-server 时降级
直连自拉 API_MONITOR_GET2，§20；注意排空式读取——server 泵与直连 watch 不可
同时读同会话环）、`box dump`、`cfg dump`、`cfg whoami`、`doctor`、`box explore`、
`template gen-browser`（探测纯读；写走 SbieSvc）、`proc exempt`、
`maint install/uninstall`、`box create 高级旗标`（SbieSvc 直连）均为可降级/恒直执
形态。波次 E（08-P2-4/5）后 `proc suspend-box/resume-box` 与 `box snapshot
default` 已 IPC 化（§22），不再属恒直连清单。

（`cfg reload` 走 SbieApi_ReloadConf 直连驱动，可降级；但它同时是 SbieSvc refresh 的一部分，
两路径一致。`maint status/start/stop` 无 IPC op——机器级操作，client 本地恒直执。）

## 6. 退出码总表（稳定契约，脚本依赖）

| 码 | 常量 | 含义 |
|---|---|---|
| 0 | OK | 成功 |
| 1 | GENERIC | 未分类失败（内部错误、文件系统错误等） |
| 2 | USAGE | 命令行语法错误（打印用法到 stderr） |
| 3 | DRIVER_UNAVAILABLE | 驱动未运行/未安装（STATUS_SERVER_DISABLED 等） |
| 4 | SERVER_UNAVAILABLE | server 拉起失败/管道超时且命令不可降级 |
| 5 | NOT_FOUND | 目标（box/pid/snapshot/setting/template/节）不存在 |
| 6 | ACCESS_DENIED | 权限不足、密码错、或本进程在沙箱内 |
| 7 | INVALID | 名字/参数值非法（ValidateName 失败、密码超长等） |
| 8 | RETRY_SUGGESTED | server 中途崩溃且命令非幂等 |
| 9 | BOX_BUSY | 沙箱内有活动进程，操作要求先终止（delete/snapshot/clean） |

## 7. 输出格式规范

### 7.1 默认（人类可读表格）

- 列间两空格左对齐；数字列右对齐；UTF-8 输出（`SetConsoleOutputCP(CP_UTF8)` + `_setmode`）。
- 表头行仅当结果 ≥1 行且非 `--quiet`。
- 空结果：输出 `（no rows）` 或对应单行 `no boxes`，退出码仍 0。

### 7.2 `--json`

- 顶层恒为对象：`{"ok":true,"data":…}` / `{"ok":false,"error":{"code":N,"message":"…"}}`。
- `data` 对应表格：对象数组（字段名 = 表列的 snake_case：`name/enabled/active_procs/file_root`…）。
- 日期：ISO-8601 本地时间 `2026-09-27T12:34:56`；字节数：整数（同时给 `"human"` 字符串字段）。
- 失败时退出码 = `error.code`。**stdout 只放 JSON**；诊断信息一律 stderr。

### 7.3 JSON 手写最小实现（`Util/Json.h`）——禁止第三方库

必须支持：`null/true/false/十进制整数(int64)/double/字符串/数组/对象`。
约束：

1. 值类型 `JsonValue`（variant 风格，`std::wstring` 存字符串）；仅**序列化**方向必须，
   `log watch` 等不解析 JSON（server 请求解析需最小 parser：递归下降，拒绝未知转义）。
2. 字符串转义：`"` `\` `\b \f \n \r \t` + `\uXXXX`（代理对正确合成 UTF-16→UTF-8）；
   控制字符 (<0x20) 必须 `\u00XX`。
3. 输出 UTF-8；数字不允许前导 `+`/多余精度；int64 超出 double 精度时仍按整数输出。
4. 拒绝深递归 > 32 层（防恶意管道 payload）。
5. 不支持注释/尾逗号；解析器遇之报 `ERR_JSON`。

## 8. 坑记录

1. **模板激活设置名是 `Template`（不是 `TemplateName`）**：核实于
   `Sandboxie\core\drv\conf.c:1239-1249`（`map_key_iter(&section->settings_map, Conf_Template)`
   即 `L"Template"`，:174）与 `conf.c:1171-1173`（`[DefaultTemplates]` 节同样机制）。
   文档初稿按 SandMan 界面印象写的是 `TemplateName`，已纠正。
2. **`sbie cfg reload` 与 `--no-refresh` 的关系**：SbieSvc 写路径自带 refresh
   （`SBIE_INI_SETTING_REQ.refresh` → 服务端 `SaveIni+SbieApi_ReloadConf`）。
   client 直调 `cfg reload` 是独立命令，勿与 `set --no-refresh` 语义混淆。
   **【语义修正（P1 清尾波次实测，04 §15）】** refresh=false 并非"仅省热重载、
   ini 仍落盘"（本条原表述有误）——SbieSvc 侧 `SetSetting` 只改**服务进程内存
   中的 CIniFile 树**，落盘与驱动重载**都**由 RefreshConf 完成
   （sbieiniserver.cpp:282-283、SaveIni 唯一调用点在 RefreshConf:1343）。
   即 refresh=false = 变更只存于 SbieSvc 内存（随后任一 refresh=true 写会把
   整棵内存树落盘提交——QSbieAPI `CommitIniChanges` 即此机制）；且任何配置
   重载通知会 `NotifyConfigReloaded` **清空**该内存缓存（未提交变更丢弃，
   sbieiniserver.cpp:899-910），下个写请求从盘上重读。
3. **`box snapshot` 全家是纯文件操作**（`<FileRoot>\Snapshots.ini` + `snapshot-<ID>\`，
   行为源 `SandBox.cpp:353-505`），不经驱动/SbieSvc；`delete/clean` 亦然——因此这些 op 的
   server 实现不依赖 SbieSvc 存活，但**必须**先 `proc kill-all`（走 SbieSvc）保证一致性。
4. QSbieAPI 的 `CreateBox` 只写 `Enabled=y`（`SbieAPI.cpp:1468`），不做磁盘目录——目录由
   驱动在首个进程启动时创建。`sbie box create` 保持同语义（不预建目录），
   `box delete --files` 才触碰目录。
5. `sbie server status` 的 "not running" 用退出码 0（供 `&&`/`||` 脚本判断靠输出文本而非
   退出码）——初稿写 4，已改；退出码 4 仅表示"命令因无 server 而失败"。
6. **（M1 实现登记：契约的 additive 具体化，签名语义未变）**——实现于
   `SandboxieOSS\SbieCore\` 与 `SandboxieOSS\sbie-cli\`（2026-09-27）：
   - `DriverApi.h` 在 §2.1 契约六函数之外**追加**了薄封装组（QueryDriverInfo/
     EnumBoxes/IsBoxEnabled/QueryBoxPath/EnumBoxProcesses/QueryProcessById/
     QueryConfText/QueryConfList/EnumConfSections/ReloadConf/GetHomePath）与诊断辅助
     （LastNtStatus/LastLoadError/LoadedFrom/AbiMatches）；`struct Api` 绑定表含
     32 项导出（02 §3 全集 + SbieDll_TranslateNtToDosPath）。**契约成员未被改动**。
   - `ConfigStore` 的 `Set/SetAppend/SetInsert/Delete(…)` 形参具体化为
     `(section, setting, value, refresh, password)`；Delete 增加
     `std::optional<ULONG> index`（0x1814 只支持整 setting 删，索引级删除由
     客户端 List→重放 组合）。
   - `ProcessRepository`/`TemplateRegistry` 增加与 `BoxRepository` 同款构造函数
     `(sbie::drv::Api*, sbie::svc::SvcClient&)`（§2.4 原文未写构造）。
   - `Snapshots::SetInfo` 的 `opt` = `std::optional<std::wstring>`。
   - `SbieStatus`（Util/Status.h）= §6 退出码 0..9 + 内部扩展码
     `ERR_JSON/ERR_SBIEDLL/ERR_SVC_TRANSPORT/ERR_NOT_IMPLEMENTED`（>100，
     经 ToExitCode 折叠为 1/3/4）。
   - sbie-cli 路由框架：命令专属标志（`--all`/`--box` 等）在**组名之后**原样透传给
     handler，只有组名之前的 `-` 前缀 token 才按全局选项解析（未知 → USAGE 2）。
7. **（M1 实现登记：SvcClient 提前全量可用）**：04 §2.2 的 Call/便捷层（含写路径
   IniSetSetting/SetPassword/ProcessServer 组/RunSandboxed）已在 M1 实现（client
   侧串行 + mutex；server 波次若多线程请移交专职线程，03 §1）。QueueClient 仍是占位桩。
8. **（M2 实现：QueueClient/Templates/cfg-template-log 命令集）**——
   `SbieCore\QueueClient\QueueClient.{h,cpp}`（契约三法 + additive
   InteractiveSession/Reconnect 泵，03 §5-§6）、`SbieCore\Model\Templates.cpp`
   填充（直读安装目录 Templates.ini + Sandboxie.ini 本地 [Template_*] 节；
   Apply/Revoke 经 SbieSvc）、`sbie-cli\cli\Commands\`：cfg_read.cpp /
   cfg_reload.cpp / template_cmd.cpp / log_cmd.cpp + CfgTemplateCommands.h。
   自测见 §9。
9. **模板描述键是 `Tmpl.Title`**（实机 Templates.ini 395 处），§4.6 初稿写
   `Tmpl.Name` 有误；实现以 Title 为主、Name 兜底。浏览器类模板的 Title 是
   `#4323,Mozilla Firefox` 式消息表引用（SandMan 端本地化用），CLI 原样输出。
10. **（给 SvcClient 维护 agent）M1 `SvcClient::IniSetSetting` 的请求长度约定
    与服务端 `CheckRequest` 不符**：M1 按 `value_len=size+1`、总长
    `offsetof(value)+…` 组装——空 value 时 `h.length < sizeof(SBIE_INI_SETTING_REQ)`
    直接被拒（STATUS_INVALID_PARAMETER）；非空时服务端校验点
    `req->value[req->value_len]`（sbieiniserver.cpp:444）越界读堆垃圾，偶发同错。
    正确约定（QSbieAPI SbieIniSet，SbieAPI.cpp:1271-1286）：`value_len=wcslen`
    （不含 NUL）、`h.length = sizeof(结构体)+value_len*2`、尾随填充区清零。
    本波次已在 Templates.cpp / template_cmd.cpp 内置正确的 `SvcIniSetLocal`
    过渡（修复后可退役）。
    **【已修复（第三波，2026-09-27）】**`SvcClient::IniSetSetting` 已按正确约定
    重写（同族 `IniGetSetting` 的 `h.length < sizeof` 同步修复，Delete 模式改为
    透传 value 以支撑 RemoveValue 语义）；Templates.cpp / template_cmd.cpp 的
    `SvcIniSetLocal` 过渡实现已删除并回归便捷层。详见 §8.22。
11. **（给 Status/DriverApi 维护 agent）驱动"值不存在"= `STATUS_RESOURCE_NAME_NOT_FOUND`
    = 0xC000008B**（conf.c:1885-1887；SDK ntstatus.h:3354 实读核对），FromNtStatus
    未列该值会折叠 GENERIC——`cfg get` 已在命令层经 `drv::LastNtStatus()` 精确
    判别（cfg_read.cpp），映射表宜补。
    **【已修复（第三波，2026-09-27）】**`Status.cpp` FromNtStatus 已补
    `0xC000008B → NOT_FOUND`（cfg_read.cpp 的本地判别保留为冗余防御）；
    02 §5 错误码表同步补齐全部已核实常量。副作用为正向：
    `drv::QueryConfList` 对缺失 setting 由折叠 GENERIC 改为按其既有的
    NOT_FOUND→OK 吞并分支正常收尾。详见 §8.22。
12. **SbieMsg.dll 消息表键是完整 msgCode**（含严重度/设施位，如 1399 =
    `0x41020577`；实机 .rsrc 遍历核实），传低 16 位查不到；而 1399 的英文文案
    按设计就是 `%0`（无输出——进程启动通知供程序消费，msgs 文本 :281），
    故 `log watch` 对 FormatMessage 空结果回退为插入串直拼。语言取注册表
    `SbieSvc\Language`（本机中文 2052，实测输出中文文案）。
13. **静态初始化期运行命令的陷阱（SBIE_CLI_DIRECT 通道）**：命名空间级
    `std::wstring` 常量（动态初始化）在该时点跨翻译单元顺序未定——Templates.cpp
    的 `kTmplPrefix` 曾因此在通道调用时为空串（过滤失效、名字未剥离）。已改为
    `constexpr` 字面量（常量初始化）。同通道代码禁用非平凡全局；`log watch`
    类流式命令须逐行 `fflush(stdout)`（重定向到文件/管道时 stdio 全缓冲，
    否则 kill 时丢输出）。

## 9. 验收记录（M2：cfg/template/log/maintenance 命令集，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**。并行窗口内
Snapshots.cpp 曾处另一 agent 中间态（不可改），期间以仓库外影子目录构建验证
（影子补丁仅作用于影子副本，未入仓库）；其修复落地后真树完整构建通过并部署到
`Installer\SbiePlus_x64\sbie-cli.exe`。

**注册（接线 agent 注意）**：`sbie::cli::RegisterCfgTemplateCommands()`
（声明于 `sbie-cli\cli\Commands\CfgTemplateCommands.h`，定义于 cfg_read.cpp）
幂等，请在 `RegisterCommands()` 末尾调用并删除 cfg/template/log 对应桩行。
各实现文件另有静态自注册（对 `log messages` 这类新键即时生效）。接线完成前的
自测通道：环境变量 `SBIE_CLI_DIRECT=1` 时 cfg_read.cpp 的分发器在静态初始化期
直接路由本命令集（非本命令集的调用原样放行），接线后冗余可删。

实测（环境：SbieDrv/SbieSvc 5.73.5 运行中，Sandboxie.ini =
`C:\WINDOWS\Sandboxie.ini`；输出经 `SBIE_CLI_DIRECT` 通道取得）：

- `cfg get MarkOfTheWebBox` → `DefaultBox`（rc 0）；`cfg get Template` →
  8 值逐行（7zipShellEx…WindowsRasMan）；`cfg get Template --index 2` →
  `Microsoft_MSMQ`；`cfg get Enabled --section DefaultBox` → `y`；
  `cfg get NoSuchSetting` → rc 5（NOT_FOUND）。
- `cfg path` → `C:\WINDOWS\Sandboxie.ini (system)`（rc 0）。
- `cfg reload` → `configuration reloaded`；`cfg reload --reconfigure` →
  `configuration reloaded (reconfigured)`（均 rc 0）。
- `template list` → 436 行（Templates.ini 437 个 [Template_*] 节，1 个空名跳过），
  列 NAME/CLASS/DESCRIPTION；`--class Local` → `no templates`；
  `--class Misc` 首行 `Template_LessConfidentialBox Misc Allow some Windows
  processes…`（注：浏览器类 Title 为 `#NNNN,名` 引用格式，见 §8.9）。
- `template info 7zipShellEx` → 8 键值行（Tmpl.Title/Tmpl.Class/…/
  OpenIpcPath ×2）；`template info NoSuchTpl` → rc 5。
- `template check DefaultBox` → 15 行：box 自身 7 项 `config` + 经
  GlobalSettings 回退生效的 8 项 `GlobalSettings`（来源分档读盘上 ini，
  GetPrivateProfileSectionW）。
- `template apply New_Box Firefox_Phishing_DirectAccess` → `applied`；
  重复 apply 幂等落盘；`template revoke …` → `revoked`，盘上与驱动缓存均
  复原（0 残留）。
- `log watch`（降级直连：进程自行接管会话 leader）：先停 SandMan（会话 leader
  占用 → 未停时报 `session leader is pid N…` rc 6），watch 运行 10 秒内以
  `Start.exe /box:DefaultBox cmd /c …` 制造事件，实时收到 SBIE1399 进程启动
  通知（Start.exe 与 cmd.exe 各一条，`[hh:mm:ss] SBIE1399 pid 插入串` 格式）；
  interactive 队列 `*MANPROXY_00000001` 创建成功（无"queue unavailable"诊断，
  即 QUEUE_CREATE 往返 OK；GETREQ/PUTRPL 需沙箱内大文件/网络封锁触发，未在
  本轮制造，代码按 03 §6 逐字段核对）。测试后清理：杀测试进程、重启 SandMan
  （leader 回归 SandMan，`log dump` 复验 rc 6）。
- `log dump --last 2` / 别名 `log messages --last 2` → rc 0（含 SBIE2337
  中文消息表文案 `启动程序失败: [44 / 123]`，验证 FormatMessage 路径）；空缓冲
  时 rc 5。
- JSON 抽验：`cfg get Template --json` → `{"ok":true,"data":{"section":…,
  "values":[8 项]}}`；`cfg path --json` → `{"path":"C:\\WINDOWS\\Sandboxie.ini",
  "is_home":false,"location":"system"}`；`template check --json` → 对象数组
  （template/source/exists）；`log dump --json` → `{count,messages:[…]}`
  （code=1090651511、id=1399 转义正确）；`log watch --json` → 每行一个 JSON
  对象（NDJSON，UTF-8）。
- 退出码抽验：成功 0 / 未知选项 2 / 驱动缺席 3（桩路径）/ 目标不存在 5 /
  leader 被占 6，与 §6 一致。
14. **（box/proc/snapshot 波次）SbieDll.dll 无任何快照导出**（5.73.5 x64，
    137 项命名导出逐项核对，无 SbieDll_*/SbieApi_* snapshot 符号）：快照是
    纯 GUI 层文件约定 `<FileRoot>\Snapshots.ini` + `snapshot-<ID>\`。关键
    语义在**沙箱内 SbieDll**：File_InitSnapshots（core\dll\file_snapshots.c）
    读 `[Current]/Snapshot` 构建 current→parent 链，File_FindSnapshotPath 把
    根目录缺失文件的**读**回退到快照目录——因此 `select` 只清根目录数据、
    **不搬**快照目录；`remove(当前快照)` 必须把内容并回根目录再删快照目录
    （Model\Snapshots.cpp 已按此实现；简化点：合并为"活动覆盖快照"的递归
    复制 + FilePaths.dat 字节级追加，未实现墓碑/重定位回放——见该文件头注）。
15. **（同上，与 §8.10 独立交叉证实）SbieSvc ini 协议两个隐藏操作**：
    - `setting="*"` + 空 value（DEL_SETTING）→ **RemoveSection**（整节删除，
      sbieiniserver.cpp SetSetting 分支）；box delete/rename 的删旧节用它。
    - `setting=""` + 非空 value（SET_SETTING）→ **整节替换**（CIniFile::SetValue
      的 ReadSection 分支，value 为 `Key=Value\n` 每值一行）；rename 一步建新节。
    - 两者共同前提：`h.length ≥ sizeof(SBIE_INI_SETTING_REQ)`。本波次在
      **Model 层**落地修复：`ConfigStore.cpp::IniSetRaw`（经 `SvcClient::Call`
      原始组包，QSbieAPI 尺寸规则）承担全部 Set/Append/Insert/Delete；与
      §8.10 的 `SvcIniSetLocal`（template 波次，位于 Templates.cpp）系同因
      独立修复，SvcClient 修复后两者均可退役。
      **【已退役（第三波，2026-09-27）】**`SvcIniSetLocal` 已删（§8.22）；
      `ConfigStore.cpp::IniSetRaw` 保留为 Model 层直组包路径（与修复后的
      便捷层同规则并存，防回归双保险）。
16. **CONF_GET_PROPERTY 与 setting 枚举无关**：驱动端 Conf_GetEx（drv/conf.c:
    1533-1537）仅在 index 低 24 位==0 时走 Conf_Get_Prop（节属性
    IsVirtual/IniLocation）；setting 枚举的真身是 `have_section &&
    !have_setting → Conf_Get_Setting_Name(section, index低24位, skip_tmpl)`
    （conf.c:1555）。M1 `ConfigStore::ListSettings` 误 OR 了 PROPERTY →
    恒空（本波次修复；此前 `box list-setting` 恒 no settings、rename 会退化为
    只写 Enabled=y）。`--all`（含模板注入项）= 去掉 NO_TEMPLS，实测
    DefaultBox 6 → 16 项。
17. **RunSandboxed（0x1205）组包两个必要条件**——任一不满足 → 服务端
    CreateProcessAsUser 失败 **ERROR_INVALID_NAME(123)**（SBIE2337
    `[44 / 123]`，§9 log dump 波次亦捕获过同值）：
    - env 必须是**完整环境块**（GetEnvironmentStringsW，双 NUL 结尾）——
      M1 便捷层恒发空 env，本地最小复现：`CreateProcessAsUserW(...,
      lpEnvironment=L"", CREATE_UNICODE_ENVIRONMENT)` → 123；
    - dir 必须**非空**——空串经服务端变成 `lpCurrentDirectory=L""` 在 SbieSvc
      的 SetThreadToken 伪装上下文中必 123（注意：本地无伪装复现时空 dir 可被
      容忍——首查误导源）。dir 空时以调用方当前目录代入（等价继承 cwd）。
    落地：`Processes.cpp::Start` 改为经 `SvcClient::Call` 原始组包
    （对齐 SbieDll_RunSandboxed，core\dll\callsvc.c:1012-1055）；便捷层
    RunSandboxed 维持原样（SvcClient 维护 agent 修复时可回归调用）。
18. **box 根目录带 READ-Only 属性**（Sandboxie 建目录时设置；实测
    `attrib` 显示 R）：`RemoveDirectoryW` 对 R 目录返回 ACCESS_DENIED——
    删除前必须 `SetFileAttributesW(FILE_ATTRIBUTE_NORMAL)`；另目录句柄会被
    驱动/SbieSvc 短暂持有（参考 NtIo_WaitForFolder 10s 等待），`Boxes.cpp`
    DeleteDirRecursive 按 500ms×20 重试。注意 rename **不搬目录**（新 box
    FileRootPath 随新名、旧目录成孤儿，与参考行为一致；测试后需手动清）。
19. **CLI 值取 flag 的值会泄进位置参数**（本波次修复）：`cfg set K V
    --section S` 曾把 S 并进 value（存盘 `V S`）。`BoxProcCommands.h::
    Positional()` 现跳过值取 flag（--index/--dir/--info/--name/--section/
    --template/--password/--sbie-dll-path）及其跟随 token。
20. **`proc start --wait` 退出码偶发被 `0x40010004`（DBG_TERMINATE_PROCESS）
    覆盖**：驱动 kill 路径（drv\process_api.c:1196 ZwTerminateProcess）与
    快退进程竞态（实测 `cmd /c exit 0` 3 次中 1 次）。透传机制本身正确
    （`exit 42` 稳定回传 42）。
21. **杂项**：(a) 杀光 box 进程后 RpcSs/DcomLaunch 服务代理会因首个进程
    启动而自动重生，snapshot take/remove 间隙可能撞 BOX_BUSY——kill 后立即
    重试即可；(b) SandMan（SbieCtrl_AutoStartAgent）会把新出现的 box 追加进
    `UserSettings_*\BoxGrouping`，测试后需还原；(c) git-bash 测试时 `/c` 会被
    MSYS 路径转换成 `C:\`（用 `MSYS_NO_PATHCONV=1` 规避），非 CLI 问题。
22. **（第三波修复登记，2026-09-27）SvcClient 写路径便捷层按 §8.10 正确约定
    重写 + 过渡实现退役**：
    - `SvcClient::IniSetSetting`：`value_len = wcslen(value)`（不含 NUL）；
      `reqLen = max(sizeof(SBIE_INI_SETTING_REQ), offsetof(value) +
      (value_len+1)*2)`，calloc 清零保证服务端 `value[value_len]` 尾零校验
      （sbieiniserver.cpp:446）恒过且无越界读——与 `ConfigStore.cpp::IniSetRaw`
      同一规则的便捷层落地（两处并存：Model 层绕行保留，防回归双保险）。
      **Delete 模式语义修正**：value 原样透传（M1 曾强制置空）——`value_len==0`
      时服务端 DelSetting 转投 SetSetting（删整 setting），非空时 RemoveValue
      （只删该值，template revoke 依赖）。
    - `SvcClient::IniGetSetting`（同族独立 bug）：请求也必须
      `h.length ≥ sizeof`（服务端 GetSetting :984-986 同款下限，M1 按
      offsetof(value) 组包必被拒）；改为定长 `sizeof` 请求 + 回复按
      `value_len-1` 剥结尾 NUL（服务端 GetSetting 的 value_len 含 NUL，
      多值以 `\n` 连接）。
    - `Templates.cpp` / `template_cmd.cpp` 的 `SvcIniSetLocal` 过渡实现删除，
      Apply/Revoke 回归 `svc_.IniSetSetting` / `Svc().IniSetSetting`（行为
      等价，消除三处重复组包）。附带受益：`Boxes.cpp` Create/Rename/SetEnabled
      直调便捷层的三处（此前非空 value 路径存在越界读偶发失败）自动修复。
    - `Status.cpp`：补 `STATUS_RESOURCE_NAME_NOT_FOUND(0xC000008B) → NOT_FOUND`
      （§8.11，SDK ntstatus.h:3354 与 conf.c:1885-1887 双核实）；02 §5 表补齐
      全部已核实常量（UNSUCCESSFUL/OBJECT_NAME_INVALID/WRONG_PASSWORD/
      LOGON_NOT_GRANTED/NOT_SUPPORTED/RESOURCE_NAME_NOT_FOUND）。
    验收实测见 §11。
23. **（box size / recover 波次实现登记，2026-09-27）**：
    - `BoxUsage.h` 契约 additive 扩展（§2.4 原签名保留）：新增
      `BoxUsageStats{total_bytes,files,dirs}` + 对应重载（docs/04 §14）。
    - 新增 `SbieCore\Model\Recovery.{h,cpp}`（P0-12 领域逻辑：路径映射/
      扫描/拷贝，纯用户态；`RecoverAddFolder` 走 ConfigStore 写路径）。
    - **驱动配置展开会把 DOS 路径转 NT**（drv\conf_expand.c
      `File_TranslateDosToNt` 分支——展开入口先于 %var% 解析对整串做
      X:\… → \Device\… 翻译）：`RecoverFolder=C:\foo` 盘上原样、驱动读
      （非 NO_EXPAND）回 `\Device\HarddiskVolumeN\foo`。因此 recover add
      **原样存储**用户输入（与 SandMan OnAddFolder 写 DOS 形态一致），
      两形态对驱动等价；Recovery 的正向映射对 DOS/NT/UNC 三形态均识别。
      另注意 `Util::DosToNtPath`（QueryDosDeviceW 带 `C:\` 尾斜杠调用）
      实测不转换（本次发现，路径原样保留）——调用方不可依赖其生效，
      修复归 Util 维护方。
    - `FormatHumanBytes` 在 cli（BoxProcCommands.h）与 server
      （Dispatcher.cpp）各有一份同构副本（模块隔离，§1；与
      FormatUnixSeconds/DeleteDirRecursiveLocal 同款处理）。
24. **（P1 清尾波次实现登记，2026-09-27；docs/04 §15）**：
    - `DriverApi.h` 薄封装组新增 `DisableForceProcess(ULONG* set,
      ULONG* get)`（API_DISABLE_FORCE_PROCESS 经 SbieApi_Ioctl，参数编排
      对齐 sbieapi.c:916-940）；新增 `SbieCore\Model\Maintenance.{h,cpp}`
      （组件启停，§2.4 契约登记）。
    - **QueryDosDeviceW 的设备名必须是 `C:` 形态**（§14 遗留 2 收口）：
      带 `C:\` 尾斜杠实测 n=0 不转换（P/Invoke 复核）。`Util::DosToNtPath`
      与 `NtToDosPath` 的逐盘符回退分支同坑同修；自测（obj\pathtest 独立
      编译五断言：转换/根形态/往返/回退分支/UNC 原样）全过。QSbieAPI
      Dos2NtPath（SbieAPI.cpp:963）同为无尾斜杠形态。
    - **API_DISABLE_FORCE_PROCESS 的状态生命周期跟 SESSION 块**：驱动把
      disable_force_time 存在会话的 SESSION 结构（session.c:69）；leader
      进程退出触发 `Process_NotifyProcess_Delete → Session_Cancel` **整块
      释放**（process.c:1510、session.c:243-274）→ 禁用状态随之消失。对
      本 CLI 的可观察后果：`server stop`（server 是会话 leader）即清空
      force 禁用（实测：force on 30 → server stop → status=normal，秒级
      内非窗口到期）；SandMan 用户侧同理（SandMan 退出清状态）。剩余秒
      推算因此只在本 server 进程存续期内可靠（跨进程触发报 unknown）。
25. **force 禁用窗口的权威值在驱动配置**：`Session_IsForceDisabled` 每次
    查询现读 `Conf_Get_Number(NULL,ForceDisableSeconds,0,10)`（session.c:
    482）——`force on <s>` 写入后 refresh 立即生效；`=0` 语义为"永不允许
    禁用"（驱动直接回 FALSE），故 CLI 拒绝 `force on 0`（rc 7）而非写入 0。

## 10. 验收记录（M2：box/proc/snapshot/cfg-set 命令集，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error（/W4 /WX）**，产物部署到
`Installer\SbiePlus_x64\sbie-cli.exe`（期间两次 copy 目标被并行 agent 的
运行中测试进程短暂锁定，非代码问题）。

**注册（接线 agent 注意）**：五个函数声明于 `sbie-cli\cli\Commands\
BoxProcCommands.h`，请在 `RegisterCommands()` **之后**调用（覆盖同名桩，
桩行删除与否皆可，后调用者胜）：

| 函数 | 定义于 | 覆盖命令 |
|---|---|---|
| `sbie::cli::RegisterBoxCreateCommands()` | cli\Commands\box_create.cpp | box create |
| `sbie::cli::RegisterBoxManageCommands()` | cli\Commands\box_manage.cpp | box info/get/set/list-setting/rename/delete |
| `sbie::cli::RegisterBoxSnapshotCommands()` | cli\Commands\box_snapshot.cpp | box snapshot list/take/remove/select/set-info（三级路由，handler 内解析第三级 token） |
| `sbie::cli::RegisterProcCommands()` | cli\Commands\proc_cmd.cpp | proc info/start/kill |
| `sbie::cli::RegisterCfgSetCommands()` | cli\Commands\cfg_set.cpp | cfg set |

（box enable/disable、proc kill-all/suspend/resume、cfg unset 等不在本波次
任务面，仍为桩，归后续波次。）

**自测通道**：仓库外 `%TEMP%` 影子 harness（复刻 Run() 全局选项解析 +
RegisterCommands() → 五个 Register*() → Route()，全源码重编），等价接线后
行为；环境为真实系统（SbieDrv/SbieSvc 5.73.5 运行中，ini =
`C:\WINDOWS\Sandboxie.ini`），写路径仅触碰 TestOss* 临时沙箱与
GlobalSettings 可还原标记项。

实测（节选，全部直连驱动/SbieSvc）：

- **box 生命周期**：`box create TestOss` → `box 'TestOss' created`（rc 0）；
  重复 create → rc 5（已存在）；`box create bad!name` → rc 7；
  `box list` 三行含 TestOss；`box set TestOss TestOssOption hello`（rc 0）→
  `box get` → `hello`（refresh 生效，驱动缓存即时可见）；`--append` 多值 →
  get 双行 v1/v2；`box get --index 1` → `v2`；越界 index → rc 5；
  `box list-setting TestOss` → Enabled/TestOssOption/TestOssMulti（§8.16
  修复后），`--all` 在 DefaultBox 6→16 项；`box info` 键值 8 行
  （含 has_snapshots）。
- **rename**：`box rename TestOss TestOss2` → `renamed`；新旧节正确切换，
  单值/多值设置完整复制（TestOssOption=hello、TestOssMulti=v1/v2 均在），
  box list 无 TestOss 有 TestOss2。
- **proc**：`proc start TestOss cmd /c "ping -n 60 …"` → 输出新 PID（rc 0）；
  `proc list --box TestOss` 见 cmd.exe；`box info` has_processes=yes；
  `proc info <pid>` 键值 11 行（image/cmdline/workdir/parent/session/
  started/suspended/flags/flags_hex）；`proc kill` → `killed`（rc 0），
  再 kill → rc 5；`proc start NoSuchBox …` → rc 5；
  `proc start … --wait`：`cmd /c "exit 42"` → 透传退出码 42（rc 42）。
- **snapshot（DefaultBox，允许面）**：`take DefaultBox TestOssSnap` →
  `snapshot #1 taken`；list 表行 `1 TestOssSnap 2026-09-27T05:19:17 *`；
  remove（当前快照路径：活动态并回根）→ `snapshot #1 removed`，RegHive
  回根、snapshot-1 目录消失、list 复空。
- **snapshot（TestOss2 全循环）**：proc start 初始化后 `take` → `set-info
  --name SnapRenamed --info …`（list 即时反映）→ `select 1`（switched，
  根数据文件刷新、活动子目录清空）→ `remove 1` → list 复空、目录结构复原；
  `remove 99` → rc 5；take 时有进程 → rc 9（BOX_BUSY）。
- **cfg set**：`cfg set TestOssCfgMark temp123`（GlobalSettings）→ 盘上
  出现该行；`cfg set TestOssCfgMark ""`（空值语义=移除全部实例，走 §8.15
  原始组包路径）→ 盘上消失；`--section` 指定节正常。
- **box delete**：`box delete TestOss2 --files` → 节删除（`*` RemoveSection）
  + 目录删除（§8.18 属性清理）→ box list/盘上节/目录三处复原；
  `--keep-section`/`--files` 组合语义按 §4.3。
- **JSON 抽验**：`--json box list` → 对象数组（snake_case：name/enabled/
  active_procs/file_root/…）；`box get`（多值）→ `{"values":[v1,v2],
  "count":2}`；`box info` → 对象；`box list-setting` → `{"settings":[…],
  "count":n}`；`box snapshot list` → 数组（id/name/date/current/default/
  parent_id/info）；`proc info` → 对象；错误信封 `{"ok":false,"error":
  {"code":5,"message":…}}`；stdout 纯 JSON（降级提示走 stderr）。
- **退出码抽验**：0/2（usage）/5（目标不存在：重复 create、缺 box、缺
  setting、缺 pid、缺 snapshot）/6（密码/权限面）/7（非法名）/9（BOX_BUSY）
  均按 §6。

**清理确认**：TestOss/TestOss2/TestOssR(egression) 节与目录全数删除；
GlobalSettings 的 TestOssCfgMark 已移除；DefaultBox 快照循环后
Snapshots.ini 残留已删、RegHive/目录结构复原；UserSettings BoxGrouping
还原为 `:DefaultBox,New_Box`；`C:\Sandbox\Administrator` 仅余 DefaultBox/
New_Box；盘上无任何 TestOss/ProbeKey 残留（python 逐行核验）。

## 11. 验收记录（接线波次：命令注册接线 + IPC 优先路由，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error（/W4 /WX）**，产物部署到
`Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv/SbieSvc 5.73.5 运行中
（session 1，SandMan 未跑，server 可任会话 leader），ini =
`C:\WINDOWS\Sandboxie.ini`。

**改动清单**（仅 `sbie-cli\cli\Commands\**` + Commands.cpp + 本文档）：

- `Commands.cpp`：RegisterCommands() 末尾依序调用 7 个注册函数
  （`RegisterServerCommands` / `RegisterCfgTemplateCommands` /
  `RegisterBoxCreateCommands` / `RegisterBoxManageCommands` /
  `RegisterBoxSnapshotCommands` / `RegisterProcCommands` /
  `RegisterCfgSetCommands`，后调用者胜）；删除被覆盖桩行——server
  start/stop/status、cfg get/set/reload/path、template 全部、log watch/dump、
  box info/create/delete/rename/set/get/list-setting/snapshot、proc
  info/start/kill。保留桩：box enable/disable/clean/size、proc
  kill-all/suspend/resume、cfg unset/list-setting/lock/unlock（后续波次）。
- 新增 `cli\Commands\IpcRoute.h/.cpp`：命令 handler 的
  **IPC 优先 → 降级直连** 共享路由（`ipcroute::Invoke`）+ 行集/键值渲染器
  （`RenderRows`/`RenderKv`，字段名 = server op 契约 snake_case）。
- 新增 `cli\Commands\server_cmd.cpp` + `ServerCommands.h`：`server
  start|stop|status`（§4.2）。
- 各命令文件 IPC 接线（读 retry=true / 写 retry=false，00 §7）：
  Commands.cpp（status/version/box list/proc list）、cfg_read.cpp（cfg
  get/path）、cfg_reload.cpp、log_cmd.cpp（dump + watch）、box_manage.cpp
  （info/get/list-setting + set/rename/delete）、box_create.cpp、
  box_snapshot.cpp（按 verb 分派）、proc_cmd.cpp（info/start/kill）、
  cfg_set.cpp、template_cmd.cpp（list/info/check/apply/revoke）。
- **删除** cfg_read.cpp 的 SBIE_CLI_DIRECT 过渡分发器（DispatchDirect +
  SelfRegistrar + shellapi 依赖，§9 所述自测通道，接线后冗余）。

**IPC 路由规则**（`ipcroute::Invoke`，核实 `ServerConnect::Call` 语义后固化）：

1. `!srvconn::HasServer()`（--no-server / 预拉起失败）→ 请求从未送出，
   直接降级直连（可降级命令照常工作 + NoteDegraded 提示）。
2. op 成功 → 按 data 渲染表格/JSON（行集 = 对象数组，与直连 --json 同构）。
3. server 业务错误（NOT_FOUND/BOX_BUSY/ACCESS_DENIED…）→ **权威结论**，
   按 error.code 报错退出，不降级（降级重跑只会同错）。
4. ERR_NOT_IMPLEMENTED → 降级直连。**坑**：server 侧序列化 error.code 经
   `ToExitCode` 折叠，103→1（GENERIC，Dispatcher.h 明示）——本波次同时按
   消息标记（"not implemented in this server build" / "unknown op"）识别，
   故本构建全部写路径 op（box.create/set/delete/rename、box.snap.*、
   proc.start/kill、cfg.set、tpl.apply/revoke）与 tpl.list/info/check、
   box.listSetting(--all 除外，见下)自动走 client 直连（Model 层经
   SbieSvc，与 §10 各波次自测路径一致）；server 补齐 op 后同一接线自动切
   换为 IPC 主路径，无需再改 client。
5. 传输断裂：幂等读（retry=true）→ Call 内部重拉重试一次，仍败降级直连；
   非幂等写（retry=false）→ 报 8 RETRY_SUGGESTED，**绝不降级**（请求可能
   已部分执行，降级直连会双写，00 §7）。

**server 命令**（04 §4.2）与两个框架交互的处置：

- `server status`：ProbeRunning（不拉起）；运行中取 status op 的 server 段
  （表格 RUNNING/PID/UPTIME_SEC/CLIENTS/IDLE_REMAINING_SEC/LOG_PUMP，
  idle null=不退出；--json 同字段）；未运行输出 `not running` 退出 0。
- `server stop`：`server.shutdown` op（retry=false）；未运行退出 5。
  框架交互 1：Run() 对一切命令预拉起——"stop 时 server 本不在跑"会先拉起
  再停。处置：server_cmd.cpp 以静态初始化期（先于 EnsureConnected）的
  ProbeRunning 快照判定"命令发起时是否在跑"；不在跑 → 静默收掉预拉起
  实例并报 5（已验证 `--no-server server stop` 与裸 `server stop` 两路皆
  rc 5、无残留进程）。
- `server start [--idle-timeout N]`：幂等（在跑报 `already running (pid
  N)` rc 0）。框架交互 2：预拉起实例用默认 300s 超时——若命令发起时无
  server 且用户给了自定义超时，先停掉预拉起实例再按自定义超时重启
  （已验证裸 `server start --idle-timeout 8` 得到按 8s 空闲退出的实例）。
- **坑（本波次实测修复）**：`server stop` 原先以"管道探测失败"为停机完成
  依据——管道消失 ≠ 进程终止，垂死实例仍持互斥体/管道首实例锁，紧随的
  重拉起在双锁上败退并静默退出（复现：stop 后立即 `version` → 降级提示
  且新实例不存活）。修复：stop 以**进程句柄 signaled**（句柄表全释放、
  锁全让位）为准（OpenProcess+WaitForSingleObject ≤5s）；且停机排空期
  不做探测轮询（每次探测=新连接=worker 计入 ActiveClients，会拖长排空）。
  修复后 stop→立即 version 三轮全绿。
- **坑（环境行为，非 CLI bug）**：SbieSvc 在沙箱**首次进程启动**时会把
  模板物化（`Template=SkipHook/OpenBluetooth`、`ConfigLevel=10`）回写进
  box 节。微循环测试（proc start 后 1s 内 delete）撞上该异步回写，delete
  的 RemoveSection先落、SbieSvc 回写后到 → ini 残留 `[TestOss]`。以
  `box delete TestOss`（纯节删）收尾即可。首生命周期（进程与 delete 隔
  30s+）无此现象。

**实测**（节选，全部真实驱动/SbieSvc；行集命令走 IPC，写路径自动降级
直连）：

- 首命令自动拉起：无 server 时 `box list` 直接成功且**无降级提示**
  （stderr 干净），server 进程驻留（DETACHED）。
- `server status` → `RUNNING yes / PID 81016 / UPTIME 5 / CLIENTS 1 /
  IDLE none / LOG_PUMP yes`（server 已任会话 leader，泵激活）；--json
  `{"running":true,"pid":…,"uptime_sec":…,"clients":1,
  "idle_remaining_sec":null,"log_pump":true}`。
- `status` 的 server 行变为真实值：`server: running (pid 81016, uptime
  5s, clients 1, idle no timeout, log pump yes)`；`version` 三行（svc abi
  0x57230 从 int 派生——server 的 service 段无 abi_hex 字段）。
- TestOss 全生命周期（与 §10 直连路径等价，读段走 IPC）：
  create → `box 'TestOss' created`（重复 rc 5、非法名 rc 7）→ set/get
  （`hello`）→ proc start（pid 回显）→ proc list --box（cmd.exe 行）→
  proc info 11 键值 → kill（`pid N killed`，再杀 rc 5）→ snapshot take
  `#1 taken` → list（`1 TestSnap 2026-09-27T06:00:36 *`）→ remove → list
  `no snapshots`（remove 99 rc 5）→ delete --files（节+目录+盘上全净）。
  坑 §8.21a 复现并按策处置：杀 cmd 后 PING.EXE/RpcSs/DcomLaunch 残留，
  逐一 kill 后 take 成功。
- `template check DefaultBox` → 15 行（7 config + 8 GlobalSettings，与
  §9 一致）；`cfg get MarkOfTheWebBox` → `DefaultBox`；`cfg path` →
  `C:\WINDOWS\Sandboxie.ini (system)`；`cfg reload` → `configuration
  reloaded`；`cfg set TestOssCfgMark temp123` → 盘上出现，置空串 → 消失。
- `log dump --last 20 | head` → server 环形缓冲真实 SBIE1399 事件
  （`[hh:mm:ss] SBIE1399 pid 插入串拼接`；注：server 侧 text 不经
  SbieMsg.dll 格式化，与直连文案形态不同、字段一致）；`log dump --json`
  → `{count,messages:[{time,code,id,pid,text}]}`（code=1090651511、
  id=1399 转义正确）。`log watch` 走 server 订阅：专用连接 RoundTrip
  (log.watch) 后 PeekNamedPipe 100ms 轮询消费 msgid=0 的 log.event 推送
  帧，DefaultBox 起进程实时收到事件行（Ctrl+C/连接断均干净退出；
  --interactive 的 interactive-queue 源仅直连模式有，server 模式 Diag
  说明后仅日志源）。
- `--no-server version` → stderr 首行
  `server not running - degraded to direct driver connection`，输出一致。
- 空闲退出：`--idle-timeout 5` 静止 8s 进程消失；`--idle-timeout 60`
  稳定驻留。自愈：`server stop` → 下一条命令（cfg get/version）自动重拉
  成功、无降级提示 → `server stop` 收尾（rc 0，进程消失）。
- JSON 抽验（>3 条）：`box list --json`（对象数组，snake_case 全字段）、
  `server status --json`、`cfg get --json`（{section,setting,values}）、
  `box snapshot list --json`、`box get --json`、`log dump --json`、错误
  信封 `{"ok":false,"error":{"code":5,…}}`。stdout 纯 JSON。
- **清理确认**：ini 仅余 [GlobalSettings]/[UserSettings_4BC00582]/
  [DefaultBox]/[New_Box]，0 处 TestOss 引用；`C:\Sandbox\Administrator`
  仅余 DefaultBox/New_Box；box list/proc list 与基线一致（0 进程）；
  UserSettings BoxGrouping 仍为 `:DefaultBox,New_Box`；无 sbie-cli 进程
  残留。

**遗留问题**（后续波次/维护 agent）：

1. server 写路径 op 未实现（Dispatcher.cpp Stub）——本波次 client 侧已按
   04 §4 参数 schema（name/setting/value/mode/index、box/cmd/dir/elevated、
   old/new、files/keep_section、snap_name/id/info 等）预接线并 retry=false，
   server 补齐注册项即自动切换，届时需对拍参数名与 data.message 渲染。
2. `box list-setting --all` 恒走直连：server 的 box.listSetting 未参数化
   NO_TEMPLS 反向语义（本波次 --all 时不发 IPC）。
3. `box get --raw` 语义：server ConfValues 恒 noExpand（等价 --raw）；
   非 raw（展开 %env%）仅直连模式区分，值含环境变量引用时两路径输出可
   能不同。
4. `log watch` 经 server 的 text 为插入串拼接（无消息表文案）；server
   推送不携带 interactive queue 事件（LogPump 仅泵 API_GET_MESSAGE）。
5. Run() 预拉起对 `server stop`/`server start --idle-timeout` 的干扰已用
   启动期快照 + 重启绕过（见上）；若后续 cli\Cli.* 允许改动，更优解是
   server 组命令跳过预拉起。

## 11. 验收记录（第三波：SvcClient 写路径修复 + 过渡退役 + 错误码表，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error（/W4 /WX）**，改动面 =
`SvcClient.cpp`（IniSetSetting/IniGetSetting）、`Status.cpp`（+0xC000008B）、
`Templates.cpp` / `template_cmd.cpp`（SvcIniSetLocal 退役）、docs 02/04；
部署 `Installer\SbiePlus_x64\sbie-cli.exe`。

实测（真实 SbieDrv/SbieSvc 5.73.5 运行中，ini = `C:\WINDOWS\Sandboxie.ini`，
全部直连；盘上核对注意 Sandboxie.ini 为 UTF-16LE+BOM，MSYS grep 需 `-a`
否则按二进制静默不匹配——本次实测曾因此误判"盘上无残留"，`-a` 后修正）：

- **box create（修复后便捷层，Boxes.cpp:152 直调 IniSetSetting）**：
  `box create TestOss` → rc 0；重复 create → rc 5；`bad!name` → rc 7；
  盘上出现完整 `[TestOss]` 节（SbieSvc 首写自动物化默认模板组）。
- **cfg set/get/置空删除**：`cfg set TestOssMark hello --section TestOss`
  → rc 0；`cfg get` → `hello`（rc 0，refresh 生效）；盘上 `TestOssMark=hello`；
  `cfg set TestOssMark "" --section TestOss`（空 value，原 bug 路径）→ rc 0；
  再 get → `setting not found: TestOss/TestOssMark (NOT_FOUND)` rc 5；
  盘上 0 处 TestOssMark。
- **template apply/revoke（修复后便捷层）**：
  `template apply TestOss Firefox_Phishing_DirectAccess` → `applied`（rc 0），
  驱动缓存与盘上（ini :72）均有该值；`template revoke …` → `revoked`（rc 0），
  缓存与盘上 **0 残留**，且节内其余 7 个 `Template=` 默认值原样保留
  （RemoveValue 单值语义成立——M1 Delete 模式置空 value 会误删整 setting）。
- **错误码映射**：`cfg get NoSuchSettingAtAll`（GlobalSettings 与 TestOss 节
  各一次）→ `setting not found: … (NOT_FOUND)` rc 5，可读输出无裸
  0xC000008B（cfg_read.cpp 命令层判别 + Status.cpp 映射双路径一致）。
- **box delete / 环境还原**：`box delete TestOss` → rc 0；box list 回到
  基线两箱；ini 0 处 TestOss；`UserSettings_*\BoxGrouping` 仍
  `:DefaultBox,New_Box`（SandMan 未掺入，无需还原）。
- **回归抽测**：`version`（driver/svc 5.73.5 abi 0x57230，rc 0）、
  `box list`（表 + --json 均正常）、`template check DefaultBox`（7 行 config
  档，rc 0）/ `template check New_Box`（rc 0）、`cfg get MarkOfTheWebBox` →
  `DefaultBox`、`cfg path` → `C:\WINDOWS\Sandboxie.ini (system)`、
  `template info 7zipShellEx`（Tmpl.Title/Class 正常）。
- `IniGetSetting` 便捷层修复（h.length ≥ sizeof + 回复剥 NUL）为契约级
  修正，当前 CLI 无调用方（cfg get 走驱动缓存路径），未单测。

## 12. 验收记录（server 写路径波次：Dispatcher 写 op + 差异项 + 错误信封，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error（/W4 /WX）**，产物部署到
`Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv/SbieSvc 5.73.5 运行中
（session 1，SandMan 未跑，server 任会话 leader + 持 interactive 队列），
ini = `C:\WINDOWS\Sandboxie.ini`。

**改动清单**（仅 `sbie-cli\server\**` + `cli\Commands\IpcRoute.*` +
`cli\Commands\log_cmd.cpp` + `cli\Cli.h/.cpp`（--show-transport 最小改动）+
本文档）：

- `server\Dispatcher.cpp/.h`：补齐全部写 op 注册（见下表）；读 op 两处
  参数化（box.listSetting 加 `no_tmpls`；box.get/cfg.get 加
  `noexpand`/`raw`）；新增写路径公共小工具（FormatUnixSeconds /
  ResolvePasswordParam / ParseSetMode / FailSvcDown / GetBoxInfoSafe /
  MakeSnapMgr）。
- `server\ServerMain.cpp`：错误信封 `error.code` 改为 SbieStatus 业务码
  **原样**（不再经 ToExitCode 折叠 103→1），另加 `error.exit` 字段携带
  折叠后退出码（向后兼容）；RunServer 在 leader 确立后启动
  StartInteractivePump（与日志泵同生命周期）。
- `server\LogPump.cpp/.h`：text 改经 `SbieDll_FormatMessage0/1/2`
  （SbieMsg.dll 消息表，与 client 直连 FormatText 同构：表键=完整 msgCode、
  "%0"/无表项回退插入串空格直拼、剥表文案自带 `SBIE%04u ` 前缀）；新增
  interactive queue 聚合泵（事件源 2）。
- `cli\Commands\IpcRoute.cpp/.h`：未实现 op 判定改为按 code==103 精确命中
  （消息标记保留为旧 server 兼容第二判据）；`--show-transport` 各判定点
  stderr 报告 ipc/direct。
- `cli\Commands\log_cmd.cpp`：server 推送事件解析 `interactive/kind/
  file_path/file_size` 加字段并渲染（人类/JSON 双轨，与直连行形态一致）；
  WatchViaServer 诊断与注释更新。
- `cli\Cli.h/.cpp`：`--show-transport` 全局诊断选项（仅组名前位置生效）。

### 12.1 op 注册清单（本波次新增真 handler）

| op | handler | Model 调用 | SvcCall 包套 |
|---|---|---|---|
| box.create | HBoxCreate | BoxRepository::Create + TemplateRegistry::Apply×N | 是 |
| box.set | HBoxSet | ConfigStore::Set/SetAppend/SetInsert（--index 重放组合） | 是 |
| box.delete | HBoxDelete | BoxRepository::Delete（含 NeverDelete/BOX_BUSY 前置） | 是 |
| box.rename | HBoxRename | BoxRepository::Rename（整节替换+删旧节） | 是 |
| box.snap.list | HBoxSnapList | SnapshotManager::List（纯文件） | 否 |
| box.snap.take | HBoxSnapTake | SnapshotManager::Take + SetInfo(--info 补写) | 否 |
| box.snap.remove | HBoxSnapRemove | SnapshotManager::Remove | 否 |
| box.snap.select | HBoxSnapSelect | SnapshotManager::Select | 否 |
| box.snap.setInfo | HBoxSnapSetInfo | SnapshotManager::SetInfo | 否 |
| proc.start | HProcStart | ProcessRepository::Start（RunSandboxed/RunStartExe） | 是 |
| proc.kill | HProcKill | QueryProcessById 前置 + ProcessRepository::Kill | 是 |
| cfg.set | HCfgSet | ConfigStore::Set/SetAppend/SetInsert | 是 |
| tpl.apply | HTplApply | TemplateRegistry::Info(存在性)+Apply | 是 |
| tpl.revoke | HTplRevoke | QueryConfList(已激活对照)+TemplateRegistry::Revoke | 是 |

仍为 Stub（ERR_NOT_IMPLEMENTED→client 降级直连）：box.setEnabled/clean/
size、proc.killAll/suspend/resume、cfg.unset/lock/unlock、tpl.list/info/
check。线程规约：一切触 SbieSvc 的 Model 调用（含 TemplateRegistry::Info
——其 FindSandboxieIni 走 IniGetPath）经 SvcProxy 专职线程；纯驱动/文件
操作在 worker 线程直执。

### 12.2 参数对拍表（client 发送名 vs server 读取名）

| op | client 发送（IpcRoute 预接线） | server 读取 | 备注 |
|---|---|---|---|
| box.create | name, templates[] | name, templates[] | 模板逐个 Apply |
| box.set | name, setting, value, mode | 同左 + index/password/refresh（server 先行支持） | index/password 已发（§13 收口）、refresh 已发（§15） |
| box.delete | name, files, keep_section | 同左 | delSection = !keep_section |
| box.rename | old, new | old, new | server 复检 ValidateName(new) |
| box.snap.list | name | name | |
| box.snap.take | name, snap_name, info? | 同左 | info 为 take 后补写 |
| box.snap.remove/select | name, id | name, id | |
| box.snap.setInfo | name, id, new_name?, new_info? | 同左 | 字段在场即改 |
| proc.start | box, cmd, dir, elevated | 同左 | dir **恒发**（P1-7：缺省=client cwd）；--wait 恒走直连（句柄语义） |
| proc.kill | pid | pid | |
| proc.killAll | box（可空=全局，P1-3） | 同左 | 空 box = EnumBoxes 循环 KillBox |
| cfg.set | setting, value, section, mode | 同左 + password/refresh | refresh 已发（P1-6）；password 已发（§13） |
| tpl.apply/revoke | box, name | box, name | |
| box.listSetting | name | name + no_tmpls（缺省 true） | --all 客户端仍直连——见坑 4 |
| box.get/cfg.get | name/setting/index | + noexpand/raw（缺省 true） | raw 未发——见坑 4 |
| force.set（新，§15） | enable, seconds?, password? | 同左 | 直驱动；seconds 写 ForceDisableSeconds |
| force.status（新，§15） | — | — | 剩余秒 = server 记录时刻推算 |

### 12.3 实测（全部真实驱动/SbieSvc；`--show-transport` 确认走 server）

- TestOss 全生命周期**全部 op `transport: ipc`**（stderr 逐条核实）：
  create（`box 'TestOss' created`；重复 rc 5 "already exists"、非法名 rc 7
  均权威结论不降级）→ set/get（`hello`）→ set --append×2 → get 多值
  （v1/v2 两行）→ list-setting（8 项，与 `--no-server` 直连逐行一致）→
  cfg set/get/置空串删除（rc 5 复验）→ template apply/revoke（`applied`/
  `revoked`；重复 revoke rc 5、未知模板 rc 5）→ proc start（pid 回显）→
  proc list --box / proc info 11 键值 → kill（再杀 rc 5）→ snapshot take
  `#1 taken` → list（ID/NAME/DATE/CURRENT/DEFAULT + --json parent_id/info）→
  set-info（`updated`，改名改描述生效）→ remove（remove 99 rc 5）→
  rename 往返（节内容随迁：TestOssMark=hello 在新名下可读）→ rename 到
  已占名 rc 5 → delete --files（节+目录+盘上全净）。
- `box create TestOss2 --template SkipHook`（templates[] 数组路径）→
  Template 值含 SkipHook；delete 复净。
- 错误信封：server 业务错误码**原样到达 client**（5/9 退出码、消息文案与
  直连分支同款，如 BOX_BUSY "has running processes; terminate them
  first"）；`box delete` 有进程时 rc 9 权威结论不降级。未实现 op 精确命中
  code==103：`template list/check` → `transport: direct (server op not
  implemented)` 后直连照常输出。
- log watch server 源文案：与直连**逐字节一致**——SBIE2337 中文消息表文案
  `启动程序失败: [44 / 123]`（IPC dump 与 --no-server 直连 dump 同串）；
  SBIE1399（按设计 "%0"）回退插入串空格直拼，两路径同构。§11 遗留 4 的
  "server 文案=逗号拼接插入串"差异消除。
- server 死亡行为：`taskkill /F` server 后**下一条写命令**自动重拉并走
  IPC（`transport: ipc`、写入落盘复验）；`--no-server` 写命令降级直连
  （`transport: direct (server not connected)` + NoteDegraded）正常完成。
  非幂等写 mid-command 断裂的 rc 8 路径本波次未动（ServerConnect/IpcRoute
  接线逻辑沿用 §11，CLI 一命令一进程使该窗口无法确定性构造）。
- 环境还原：ini 仅 [GlobalSettings]/[UserSettings_4BC00582]/[DefaultBox]/
  [New_Box]，0 处 TestOss/Wave3/FileMigration 引用；DefaultBox 的
  FileMigrationLimit 试验值已置空串移除；C:\Sandbox\Administrator 仅
  DefaultBox/New_Box（试验 probe 文件已清）；BoxGrouping 仍
  `:DefaultBox,New_Box`；proc list 0 进程；`server stop` 收尾 rc 0、
  无 sbie-cli 进程残留。

### 12.4 坑记录

1. **错误信封折叠（任务坑 3，已修）**：server 侧 `error.code` 原为
   `ToExitCode(status)`，103 折叠为 1（GENERIC），client 被迫按消息标记
   嗅探占位桩且 100+ 内部码语义在传输中丢失。修复：业务码原样下发 +
   `exit` 兼容字段；传输码（4/8）由 client 传输层合成、从不出现在信封。
   行为兼容性：0..9 两字段恒等；100+ 码 client `EmitError→ToExitCode`
   折叠结果与旧 server 预折叠一致（102→4、101→3、103→1），旧 client
   无感。
2. **interactive queue 线程模型**：QueueClient 的全部 LPC（Create/GetReq/
   PutRpl）经 `SvcClient::Call`，server 内直接在泵线程调会违反 03 §1 线程
   亲和——泵线程只做 `iq.event()` 等待，Start/Drain 包进 `SvcCall`（在
   SvcProxy 专职线程执行）；断线重建的 `Reset` 收归泵线程（等待点已返回，
   无并发等待者）。
3. **client 预接线缺口（后续波次 2 行级改动，本波次 server 先行支持）**：
   box.set 的 `--index` 与 cfg.set 的 `--no-refresh` 未进 IPC params（走
   IPC 时 index 语义退化为整 setting 替换、refresh 恒 true）；写命令的
   `--password`/`SBIE_PASS` 未进 params（server 侧回退读自身环境——即
   首个拉起 server 的 client 所设，后续 client 不同密码时锁配置写会
   ACCESS_DENIED）。server op 已接受 index/password/refresh 参数。
4. **§11 遗留 2/3 server 侧已参数化、client 侧待接线**：box.listSetting
   `no_tmpls=false`（--all）与 box.get/cfg.get `noexpand/raw` server 已
   透传（QueryConf 的 CONF_GET_NO_* 位）；client（box_manage.cpp，本波次
   禁改）--all 仍直连、--raw 仍不发送，行为不变。
5. **proc start 的 cwd/env 语义差**：dir 缺省时 Model 层以**调用方**当前
   目录代入——直连=client cwd，IPC=server cwd（拉起目录=exe 目录）；
   env 块同理（server 继承首个 client 的环境）。带 `--dir` 即无差异。
6. **interactive 事件通道实测边界**：聚合泵代码按 03 §6 逐字段实现并随
   leader 启动，但本轮环境未能制造真实 GETREQ（FileMigrationLimit=1 +
   100KB 文件追加/整写，沙箱内副本生成而 SbieSvc 未走询问分支；与 §9
   波次"未在本轮制造"结论一致）。`--interactive` 的 y/N 决策经 server 需
   请求-应答 op（超出本波次 op 集），server 模式维持无人值守自动拒绝 +
   事件呈现。

## 13. 验收记录（补缺波次：06 缺口表 P0 收口 + 参数接线，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**（本波次
两轮），产物部署到 `Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv/
SbieSvc 5.73.5 运行中（session 1，SandMan 未跑，server 任会话 leader），ini =
`C:\WINDOWS\Sandboxie.ini`。任务面 = docs/06 §3.1 缺口表的 P0-1/2/3/4/6/7/8/
9/10/11 + P1-4/5（box size 与文件恢复归后续波次）。

**改动清单**（仅 `sbie-cli\**` + 本文档追加 + docs/06 打勾；SbieCore\Model
未动——本波次全部为"下层已备、上层接线"）：

- client 桩补实现（原 `Commands.cpp:561-576` 桩行删除/被覆盖）：
  - `box enable/disable/clean` → `cli\Commands\box_manage.cpp`（P0-3/4）；
  - `proc kill-all/suspend/resume` → `cli\Commands\proc_cmd.cpp`（P0-1/2）；
  - `cfg unset/lock/unlock` → `cli\Commands\cfg_set.cpp`（P0-6/7/8）；
  - `cfg list-setting` → `cli\Commands\cfg_read.cpp`（P0-9，server op
    cfg.listSetting 原本已实现、client 命令是桩——反向缺口收口）。
- params 接线（正确性缺陷修复）：
  - `box set --index` 进 IPC params（**P0-10**：此前 server 在场时按整
    setting 替换 → 其余值静默丢失）；
  - 写命令 `--password`/`SBIE_PASS` 进 IPC params（**P0-11**：box
    set/create/delete/rename/enable/disable、cfg set/unset、tpl apply/
    revoke；此前 server 回退读自身环境 → 锁配置下经 IPC 的写恒
    WRONG_PASSWORD）；
  - `box list-setting --all` 发送 `no_tmpls:false`（**P1-4**：删恒直连
    特判）；`box get`/`cfg get` 发送 `noexpand`（**P1-5**：消除两路径
    %env% 展开语义漂移；cfg get 新增 `--raw` 旗标与 box get 对齐，缺省
    展开）。
  - 附带修复：boxproc 风格命令此前**静默丢弃**命令专属区的尾置
    `--password <pw>`（Positional 跳过其值但 ResolvePassword 只读全局
    选项/环境）——新增 `ResolvePasswordArgs`（尾置 > 组名前 > SBIE_PASS）
    统一双路径。
- server `Dispatcher.cpp` 新真 handler（替换 Stub，tpl.list/info/check
  维持降级现状——06 §5 判可接受）：`HProcKillAll/HProcSuspend/HProcResume`、
  `HBoxSetEnabled/HBoxClean`、`HCfgUnset/HCfgLock/HCfgUnlock`；既有写 op
  `HBoxCreate/HBoxDelete/HBoxRename/HTplApply/HTplRevoke` 补 password 透传
  （Model 方法不带密码形参，以 ConfigStore/SvcClient IniSetSetting 等价
  转录——空密码时与 Model 调用字节等价）；写失败且 ACCESS_DENIED+无密码
  时 message 附 WRONG_PASSWORD 提示（见下）。
- `Cli.cpp` --help 文案从 M1 更新为全命令树（06 §5 遗留）。

**§12.2 参数对拍表的增量修正**（本波次后 client 发送面）：

| op | client 发送 | 备注 |
|---|---|---|
| box.set | name, setting, value, mode, **index**, **password** | P0-10/11 收口 |
| box.create / box.delete / box.rename | …, **password** | 锁配置写凭据 |
| box.setEnabled（新） | name, enabled, password? | |
| cfg.set / cfg.unset（新） | …, password?；unset 另有 index/refresh | |
| cfg.lock（新） | new_password, password(=旧密码) | 空新密码=解除锁定 |
| cfg.unlock（新） | password | 仅验证 |
| tpl.apply / tpl.revoke | box, name, **password** | 实测曾漏（首轮实测抓出后补） |
| box.listSetting | name, **no_tmpls** | --all → no_tmpls:false |
| box.get / cfg.get | …, **noexpand**（cfg get 加 --raw） | 缺省 false=展开 |

**语义登记**：

- **box clean（P0-4）**：NeverDelete=y → ACCESS_DENIED(6)；有活动进程 →
  BOX_BUSY(9)（提示 `proc kill-all`，与 box delete 一致——06 §P0-4 建议的
  "拒+提示"变体）；否则递归删 **FileRoot 内容**（保留 FileRoot 目录本身
  与 ini 节；对齐 CleanBoxFolders）。递归删除器（只读属性清理 + 500ms×20
  句柄重试）在 cli 与 server 各有一份同构副本（Model DeleteDirRecursive
  为 Boxes.cpp 内部静态，模块边界禁引用）。
- **cfg lock（P0-7）**：`cfg lock <new-pw>`；变更已设密码需 `--password
  <旧>`/SBIE_PASS；**空新密码 = 解除锁定**（SET_PASSWORD 空串，QSbieAPI
  SbieAPI.cpp:2248 同语义）；>64 WCHAR → rc 7；盘上 EditPassword 存哈希。
- **cfg unlock（P0-8，连接级密码缓存语义）**：**仅验证**（TEST_PASSWORD），
  密码**不缓存**——CLI 一命令一进程，server 端不做全局明文缓存（06 §P0-8
  建议采纳）；实测 unlock 成功后无密码写仍 rc 6（附提示），带 --password/
  SBIE_PASS 的写正常。首次 WRONG_PASSWORD 的提示覆盖两处：写命令失败且
  无密码时 message 附 "(config is locked: pass --password \<pw\> or set
  SBIE_PASS)"；unlock/lock 专用文案。
- **cfg unset（P0-6）**：`--index` = 删该值（ConfigStore::Delete 的
  List→重放组合）；缺省 = 删整 setting（0x1814 空 value）；整删前置存在
  性检查（rc 5）；index 越界 rc 5。
- **proc kill-all（P0-1）**：计数=请求时该 box 进程数（本波次不含 P1-3
  全局 `--all` 形态）；suspend/resume（P0-2）状态经 `proc info` 的
  suspended 字段呈现（proc list 无状态列）。

**实测**（节选，全部真实驱动/SbieSvc；`--show-transport` 逐条核实新 op
全部 `transport: ipc`，--no-server 对照走 direct）：

- **enable/disable（P0-3）**：create TestOss → disable（ipc）→ `box list`
  默认不含 TestOss（disabled 过滤，与 §10 语义一致）→ enable → 回
  yes；`box get Enabled` y；`--json` `{"message":"enabled"}`；
  disable 不存在 box → rc 5。
- **suspend/resume（P0-2）**：双进程 start → suspend（ipc）→ `proc info`
  suspended=yes（文本/JSON 双验，JSON `"suspended":true`）→ resume →
  no；resume 非沙箱 pid 4 → rc 5。
- **kill-all（P0-1）**：`proc kill-all TestOss` → `4 process(es)
  terminated`（2×cmd + 派生 PING + RpcSs 代理，请求时计数）→ proc list
  空、has_processes no；kill-all 不存在 box → rc 5；`--json`
  `{"count":0,"message":"0 process(es) terminated"}`。
- **box set --index（P0-10）**：三值 v1/v2/v3（--append×2）→ `box set
  TestOss TestOssMulti MODIFIED --index 1`（**transport: ipc**）→ get =
  v1/MODIFIED/v3（**多值不丢**，--json `{"values":[...],"count":3}`）；
  index 9 → rc 5（值数提示）。
- **cfg unset（P0-6）**：`cfg unset TestOssMulti --section TestOss --index 2`
  （ipc）→ 剩 v1/MODIFIED；整删 → get rc 5；缺 setting rc 5；越界 rc 5；
  `--json` `{"message":"unset"}`。
- **cfg list-setting（P0-9）**：GlobalSettings → MarkOfTheWebBox/Template
  （ipc）；`cfg list-setting TestOss` 6 项；--json `{settings,count}`。
- **list-setting --all（P1-4）**：DefaultBox `--all` 经 ipc（此前恒直连），
  6 → 16 项（与 §10 直连计数一致）。
- **raw/noexpand（P1-5）**：`cfg set ExpandMark "%SystemRoot%\notepad.exe"`
  → IPC 缺省 = 展开（`\Device\HarddiskVolume3\WINDOWS\notepad.exe`）、
  `--raw` = 原文；`--no-server` 直连两形态**输出与 IPC 逐字节一致**；
  box get 继承读同样区分。
- **cfg lock/unlock/password（P0-7/8/11）**：lock Test123（ipc，盘上
  EditPassword=哈希）→ 无密码 `cfg set` → rc 6 附提示 → 尾置
  `--password Test123` 经 **ipc** 写入成功 → `SBIE_PASS=Test123`（ipc 与
  --no-server 直连双验）→ 组名前 `--password`（ipc）→ `cfg unlock
  WrongOne` rc 6（附解除指引）/ `cfg unlock Test123` rc 0（"password
  verified (not cached; …)"）→ **unlock 后无密码写仍 rc 6**（不缓存
  语义实证）→ 锁配置下 box create（直连 SBIE_PASS）/ tpl apply+revoke
  （ipc --password，首轮实测抓出 tpl 漏发后修复复验）/ box rename（ipc
  --password）/ box delete --files（ipc --password）全通 → 65 字符
  密码 rc 7 → 无旧密码改锁 rc 6 → `cfg lock "" --password Test123` →
  "config lock removed"、EditPassword 消失、无密码写恢复。
- **box clean（P0-4）**：沙箱内写 `C:\clean_marker.txt` + RegHive 等真实
  内容 → 有进程时 clean → rc 9（提示 kill-all）→ NeverDelete=y → rc 6 →
  解除后 clean（ipc）→ "cleaned"，FileRoot 内容全删、**根目录与 ini 节
  保留**（list-setting 6 项、box list 在列）。
- **JSON 抽验**（>6 处）：box enable/proc kill-all/cfg unset/cfg unlock/
  cfg get（raw/expand 双形态）/cfg list-setting/box list-setting --all/
  box get 多值——stdout 纯 JSON、错误信封 `{"ok":false,"error":{"code":N,…}}`。
- **退出码抽验**：0/2（usage）/5（缺 box/缺 pid/缺 setting/index 越界）/
  6（WRONG_PASSWORD 系/无密码锁写/NeverDelete）/7（密码>64）/9（BOX_BUSY）
  均按 §6。

**清理确认**：cfg lock 收尾**必已解锁**（EditPassword rc 5 复验）；ini 仅
[GlobalSettings]/[UserSettings_4BC00582]/[DefaultBox]/[New_Box]，0 处
TestOss/LockMark/EnvMark/PrefixMark/AfterUnlock/ExpandMark/DirectMark/
NeverDelete 引用；`C:\Sandbox\Administrator` 仅 DefaultBox/New_Box；proc
list 0 进程；BoxGrouping 仍 `:DefaultBox,New_Box`；回归抽测（version/
cfg get MarkOfTheWebBox/template check DefaultBox/box snapshot list/
cfg path）全绿；`server stop` 收尾、无 sbie-cli 进程残留。

**遗留**（后续波次）：P0-5 box size（三重桩，ScanBoxSize 归 Model/
BoxUsage 维护方）、P0-12 文件恢复（需新建 Model）；P1-6（cfg set 的
refresh 参数仍未发——本波次任务面外，cfg unset 已带）；P1-7 cwd/env；
server 写 op 中 box.snap.* 的 password 透传未做（快照为纯文件操作，
无锁配置问题）；`proc kill-all --all-sessions` 旗标未实现（Model KillBox
恒 sessionId=-1，协议语义未核实前不引入）。

### 13.1 验收回传修复：box disable/enable 落盘与列表语义（2026-09-27 第二轮）

**回传现象**：验收方复现 `box create TestVC → box disable`（transport: ipc、
rc 0）后，`grep -i TestVC` 仅见节头（判"节内无 Enabled= 行"），且 `box list`
两路径（ipc / --no-server）均不含该 box；另一场景（TestVB，create 后立即
disable）list 却显示 `yes`——两种现象交替。

**排查结论（三层根因，均已实证）**：

1. **读路径过滤（OSS bug，确定性，主因）**：`box list` 缺省按
   `EnumBoxes(enabledOnly=true)` 过滤——disable 后 box 从缺省列表"消失"
   （`--all` 才可见），验收按"消失=写丢失"误判。写链路本身无问题：
   `IniSetSetting(refresh=true)` 的 LPC 回复返回前，SbieSvc 内部已同步完成
   `CIniFile::SetValue`（服务内存）→ `SaveIni`（全文件落盘）→
   `SbieApi_ReloadConf`（驱动缓存重载）三步（sbieiniserver.cpp::RefreshConf
   实读），rc 0 即"盘上与缓存均已是新值"。压测 4/4 轮（含 create→disable
   背靠背）disable 后立即检查：盘上 `Enabled=n`、驱动缓存 `box get`=n、
   `--all` 显示 no——写路径零异常。**grep 口径附注**：`grep -i TestVC` 只能
   匹配节头行，`Enabled=n` 不含节名、本就不会出现在该 grep 结果中。
2. **外部写者在场（环境因素，解释"不稳定交替"）**：排查时发现
   **SandMan.exe 正在运行**（QSbieAPI 全家）。其 `CSandBox` 构造函数
   （QSbieAPI\Sandboxie\SandBox.cpp:40-121）在加载 ConfigLevel<10 的 box 时
   会把默认组（BlockNetworkFiles/RecoverFolder×3/BorderColor/Template×7/
   ConfigLevel=10）**物化回写进 ini**——实测现场抓获：CLI disable 落
   `Enabled=n` 后 1 秒，TestVD 节被 SandMan 追加全套默认（其写不经
   Enabled，n 保留）；验收遗留的 TestVC 节内容恰为该物化块（DefaultBox
   同款）。此类回写与 CLI 写经同一 SbieSvc 串行、不回退 Enabled；但
   SandMan 的 UI 操作（启用开关/向导/BoxGrouping 追加）与其文件 watcher
   触发的整段重写属既有环境干扰（§8.21b、§11 坑 2 同族），在验收窗口内
   可造成"disable 后 list 仍 yes"的表观（另一可信机制：当时配置处于锁定
   态——§13 已实测锁态下无密码 disable 报 rc 6、Enabled 保持 y）。
3. **缺省过滤语义本身对齐 Plus 修正**：QSbieAPI GetAllBoxes/ReloadBoxes
   枚举全部节、不按 Enabled 过滤（disabled 沙箱在列表中以状态呈现）。
   原 04 §4.3 的"`--all`（含 disabled）"规格与之相反，予以纠正。

**修复**（构建 0 error / 0 warning）：

- `Commands.cpp CmdBoxList` 与 `Dispatcher.cpp HBoxList`：枚举恒
  `EnumBoxes(false)`（不过滤，ENABLED 列 yes/no）；`--all`/params.all 保留
  为 wire/脚本兼容的 no-op。04 §4.3 表行同步改写。
- **写后回读校验加固**（client 直连 CmdBoxSetEnabled + server
  HBoxSetEnabled 同款）：写成功后立即经驱动缓存回读 `Enabled`，与请求值
  不符 → 显式 GENERIC 报错（"verification failed: Enabled is still '…'"）。
  依据第 1 条的同步链，正常路径回读必中；该校验把任何"静默成功但未生效"
  （如未来引入的异步刷新、外部回退）转化为可见失败，杜绝再次误判。

**证据链（修复后实测，SandMan 仍在运行的环境下）**：三轮
`create → disable（rc 0）→ ini 节显示 Enabled=n → box list（ipc）显示
no → box list --no-server 显示 no → box get=n → enable（rc 0）→ ini
Enabled=y → 两路径 list yes → delete（节+grep 0 残留）`——R1/R2 走 IPC
（--show-transport: ipc），R3 全程 --no-server 直连；R1 顺带清掉验收遗留
TestVC 节（其内容=物化块+新写的 Enabled=n，反证 disable 写入本就可达）；
附带现场捕获 SandMan t+1s 物化回写（Enabled=n 不受影响）。收尾：无
TestV* 残留、BoxGrouping 未被污染（`:DefaultBox,New_Box`）、锁定态未变
（EditPassword NOT_FOUND）、server 停机。

## 14. 验收记录（box size / 文件恢复波次：06 缺口表 P0-5/P0-12，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**（本波次
四轮：首轮 box_recover.cpp 缺 ConfigStore include；二轮 human 单位 off-by-one
实测抓出修复；三轮 --to 值泄进位置参数实测抓出修复；四轮 recover add 存储
形态按实测结论改原样存储），产物部署 `Installer\SbiePlus_x64\sbie-cli.exe`。
环境：SbieDrv/SbieSvc 5.73.5 运行中，ini = `C:\WINDOWS\Sandboxie.ini`，
**SandMan 运行中**（§13.1 同款环境噪声；全程独立 TestOss 沙箱，未受干扰——
仅 SbieSvc/SandMan 物化的默认 RecoverFolder×3/Template×7/ConfigLevel=10 属
正常行为）。

**改动清单**（`SbieCore\Model\**` + `sbie-cli\**` + 本文档 + docs/06 打勾）：

- `SbieCore\Model\BoxUsage.{h,cpp}`（P0-5 三重桩收口）：ScanBoxSize 实现
  （FindFirstFileExW 递归、`\?\` 长路径、重解析点跳过、progress 每 256
  条目回调可取消）；契约 additive 扩展 BoxUsageStats（§8.23 登记）。
- `SbieCore\Model\Recovery.{h,cpp}`（新建，P0-12）：RecoveryManager
  （List/MapToRealPath/Copy）+ RecoverAddFolder。
- `sbie-cli\ipcc\SbieIpc.h`：+3 op 常量（recover.list/copy/add）。
- `sbie-cli\cli\Commands\box_manage.cpp`：+`CmdBoxSize`（IPC `box.size`
  retry=true + 降级直连）。
- `sbie-cli\cli\Commands\box_recover.cpp`（新建）：三级路由
  list/copy/add；选择子（index/all/path 后缀匹配）在 client 解析为沙箱
  绝对路径后提交 recover.copy；`--to`/`--overwrite`；`RecoverPositional`
  跳过值取 flag（§8.19 同款坑，--to 值曾泄进选择子——实测抓出）。
- `sbie-cli\cli\Commands\BoxProcCommands.h`：kValueFlags +`--to`；
  +FormatHumanBytes；+RegisterBoxRecoverCommands 声明。
- `sbie-cli\server\Dispatcher.{h,cpp}`：HBoxSize（纯文件，worker 线程同步）、
  HRecoverList（读）、HRecoverCopy（**真实文件 IO server 侧**）、HRecoverAdd
  （SvcProxy + 写后回读）；box.size Stub 删除。
- `sbie-cli\cli\Commands.cpp` / `Cli.cpp`：box size 桩行删除、
  RegisterBoxRecoverCommands 接入、--help 命令树补 recover。

**架构决策（P0-12）**：recover 的真实文件 IO（CopyFileW 拷出）在 **server
侧**完成（HRecoverCopy）——维持"server 负责所有真实操作"模型（00 §5）；
无 server 时 client 直连同一 Model（RecoveryManager）等价执行。选择子解析
（index/path → 沙箱路径）在 client：copy 命令先经 recover.list（IPC 或直连）
取行集再提交 recover.copy——索引稳定性由两侧同一 List 排序保证。`--to`
目录下保留 FileRoot 相对结构（`--to D\user\current\Documents\f.txt`），
对齐 GPL core 文件夹恢复的 GetDestPlusRelative 行为；缺省（无 --to）恢复到
原位（targetPath）。**拷贝语义**（CopyFileW，保留 mtime）而非 SbieCtrl 的
移动语义（FO_MOVE）——非破坏性，沙箱内副本保留（box clean/delete 才清除）。

**§12.2 参数对拍表的增量**（本波次 client 发送面）：

| op | client 发送 | 备注 |
|---|---|---|
| box.size | name | 读，retry=true；data {name,bytes,human,files,dirs} |
| recover.list | name | 读，retry=true；行集 {index,sandbox_path,box_path,target_path,size,size_human} |
| recover.copy | name, paths[], to?, overwrite | **写，retry=false**；paths = client 解析后的沙箱绝对路径 |
| recover.add | name, folder, password? | 写，retry=false；folder 原样存储 |

**实测**（节选，全部真实驱动/SbieSvc；`--show-transport` 逐条核实新 op 全部
`transport: ipc`，`--no-server` 对照走 direct；TestOss 沙箱内经
`proc start cmd` 制造真实文件）：

- **box size（P0-5）**：`box create TestOss` → `box size TestOss`（ipc）→
  `0 B / 0 / 0 / 0`（未初始化沙箱）；`box size NoSuchBox` → rc 5；--json
  `{"name":…,"bytes":…,"human":…,"files":…,"dirs":…}`；--no-server 直连
  输出一致。proc start 写入沙箱后：**`1.11 MB 1159498 11 4`**——与
  `dir /a /s`（11 个文件 1,159,498 字节）及 PowerShell
  `-Recurse -File -Force | Measure-Object -Sum Length`（bytes=1159498
  files=11 dirs=4）**逐字节一致**（资源管理器"大小"口径；注意 PS 不带
  -Force 时漏 hidden 文件——RegHive/desktop.ini 均 hidden，对比须 -Force）。
- **recover list（P0-12）**：SbieSvc/SandMan 已物化
  `RecoverFolder=%Desktop%/%Personal%/%Downloads-GUID%`；沙箱内
  `user\current\Documents\{OssRecoverA.txt 28B, OssRecoverC.bin 4096B,
  sub\OssRecoverB.txt 22B}` → list（ipc）三行：PATH（相对 FileRoot）、
  TARGET（`C:\Users\Administrator\Documents\...` 映射正确）、SIZE；
  --json 含 sandbox_path/target_path/size/size_human 全字段；直连一致。
- **recover copy**：`copy TestOss all --to %TEMP%\OssRecoverOut`（ipc）→
  `3 file(s) recovered (4.05 KB)`，目标树保留 `user\current\Documents\`
  结构，diff/cmp 内容逐字节一致，**mtime 保留**（stat 同秒）；重复 copy →
  rc 1 `target exists; use --overwrite to replace`（--json 错误信封同文案）；
  `--overwrite` → rc 0；`copy TestOss 1`（index）→ 单文件 4.00 KB；
  `copy TestOss OssRecoverB.txt`（路径选择子）→ 命中 sub 下文件；无 --to
  默认原位恢复 → 真实 `C:\Users\Administrator\Documents\OssRecoverA.txt`
  落盘、内容一致（验证后清理）；index 99 → rc 5（含文件数提示）；无匹配
  选择子 → rc 5；空列表 copy → rc 5；直连（--no-server）list/copy/exists
  错误三路输出与 IPC 一致。
- **recover add**：`add TestOss C:\Users\Administrator\Pictures`（ipc）→
  `recover folder added: \Device\...\Pictures (C:\...\Pictures)`，盘上
  原样存 `C:\Users\Administrator\Pictures`（DOS 形态，§8.23：驱动展开读回
  NT，两形态等价）；`add TestOss %Favorites%` → 原样存 %var%；相对路径 →
  rc 7；`add NoSuchBox …` → rc 5；--json {value,message}；写后回读两侧
  均实现（值数不增 → GENERIC）。
- **错误码抽验**：0/1（目标存在）/2（usage：缺参/未知子命令）/5（缺 box、
  越界 index、无匹配选择子、空列表）/7（folder 形态非法）均按 §6。
- 期间实测抓出并修复：human 单位 off-by-one（4096 显示 4.00 MB → 4.00 KB，
  cli/server 两副本同修）；`--to` 值泄进位置参数（选择子误报 no match，
  RecoverPositional 值取 flag 跳过）。

**清理确认**：ini 仅 [GlobalSettings]/[UserSettings_4BC00582]/[DefaultBox]/
[New_Box]，0 处 TestOss/OssRecover/OssDirect 引用（RecoverFolder 试加值
经 `cfg unset --index` 逐一移除）；`C:\Sandbox\Administrator` 仅
DefaultBox/New_Box；`%TEMP%` 四个 OssRecover* 目录删除；真实 Documents
试恢复文件删除；proc list 0 进程；BoxGrouping 仍 `:DefaultBox,New_Box`；
EditPassword NOT_FOUND（解锁态未变）；`server stop` 收尾、无 sbie-cli
进程残留。

**遗留**（后续波次/维护方）：

1. **超大沙箱的 size 进度**：box.size 同步执行（CLI 一问一答无流式呈现
   面；本机沙箱毫秒级）。ScanBoxSize 的 progress 回调契约已在（可取消），
   未来可加 `box.size --watch` 类订阅 op 或后台线程+缓存。
2. **`Util::DosToNtPath` 失效**（§8.23）：QueryDosDeviceW 以 `C:\`（带尾
   斜杠）调用实测不转换、原样返回——当前无生效调用方（recover add 已改为
   原样存储），修复归 Util 维护方（应改 `C:` 形态调用）。
3. **recover copy 的移动语义变体**（recover move = SbieCtrl FO_MOVE 同款）
   未做（拷贝语义已满足任务面）；自动恢复（2199 事件触发的即时提示）属
   P2-10 interactive 决策通道。
4. `recover copy` 对 `\share`（UNC）来源的 --to 结构恢复未实测（环境无
   UNC 可写目标）；映射代码按 GetRealPath 规则实现。

## 15. 验收记录（P1 清尾波次：06 缺口表 P1-1/2/3/6/7 + DosToNtPath 修复，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**（全量
rebuild 复核：删除中间产物后重建，无告警），产物部署
`Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv/SbieSvc 5.73.5 运行中、
**SandMan 运行中**、ini = `C:\WINDOWS\Sandboxie.ini`（测试前仅
[GlobalSettings]空/[UserSettings_4BC00582]/[DefaultBox]/[New_Box]）。

**改动清单**（`SbieCore\**` additive + `sbie-cli\**` + 本文档 + docs/06 打勾）：

- `SbieCore\DriverApi\DriverApi.{h,cpp}`（P1-1）：薄封装 +
  `DisableForceProcess(set,get)`（API_DISABLE_FORCE_PROCESS_ARGS，
  API_NUM_ARGS=8 帧经 `SbieApi_Ioctl`；契约区未动）。
- `SbieCore\Model\Maintenance.{h,cpp}`（新建，P1-2）：QueryComponent/
  StartComponent/StopComponent——服务=进程内 SCM（OpenSCManager/
  StartService/ControlService+等待落定），驱动=`KmdUtil.exe start|stop SbieDrv`
  经 `SbieDll_RunFromHome`（安装目录同分发；命令形态核实 kmdutil.c:194-225
  与 SandboxieVS.nsi:1593-1635 的 stop svc→drv / start svc 序列；服务名
  SbieDrv/SbieSvc 核实 common/my_version.h:63/66）。
- `SbieCore\Util\PathMapper.cpp`（附带修复，§14 遗留 2）：DosToNtPath 与
  NtToDosPath 回退分支的 QueryDosDeviceW 改 `C:` 无尾斜杠形态调用。
- `sbie-cli\ipcc\SbieIpc.h`：+2 op（force.set/force.status）。
- `sbie-cli\cli\Commands\force_cmd.cpp`（新建，P1-1）：force on/off/status
  （IPC 优先 + 直连降级；直连 status 剩余未知——跨进程无时刻可考）。
- `sbie-cli\cli\Commands\maint_cmd.cpp`（新建，P1-2）：maint status/start/
  stop [--driver|--service|--all]（**无 IPC op**，机器级 client 本地执行；
  操作后按 SCM 复核报告实际态）。
- `sbie-cli\cli\Commands\proc_cmd.cpp`：kill-all `--all` 全局形态（P1-3，
  box/`--all` 互斥校验）；proc start `--dir` 缺省显式代 client cwd 进
  params（P1-7，恒发）。
- `sbie-cli\cli\Commands\cfg_set.cpp` / `box_manage.cpp`：refresh 进
  `cfg.set`/`box.set` params（P1-6；box set 同坑一并收口）。
- `sbie-cli\server\Dispatcher.cpp`：HForceSet/HForceStatus（直驱动；server
  记录禁用时刻推算剩余秒，mutex 保护）；HProcKillAll box 参数可空=全局。
- `sbie-cli\cli\Commands.cpp` / `BoxProcCommands.h` / `Cli.cpp`：注册 +
  --help 命令树补 force/maint 与 kill-all --all。

**§12.2 参数对拍表的增量**（本波次 client 发送面）：

| op | client 发送 | 备注 |
|---|---|---|
| force.set | enable, seconds?, password? | 写，retry=false；seconds>0 先写 ForceDisableSeconds |
| force.status | — | 读，retry=true |
| proc.killAll | box?（空=全局） | P1-3；全局时 data 增 boxes 字段 |
| cfg.set / box.set | …, **refresh** | P1-6（box set 同坑附带收口） |
| proc.start | box, cmd, **dir（恒发）**, elevated | P1-7：缺省=client cwd |

**实测**（节选，全部真实驱动/SbieSvc；`--show-transport` 核实 force/killAll/
cfg.set 新参路径均 `transport: ipc`，`--no-server` 对照走 direct）：

- **force（P1-1）**：status（ipc）=normal → `force on 10`（ipc）→
  `force process disabled for 10 second(s)` + ini 落盘
  `ForceDisableSeconds=10` → status `disabled (10 → 6 second(s) remaining)`
  （4s 间隔，倒计时正确）→ off → status normal（直连一致；--json 三字段
  disabled/window_seconds/remaining_seconds）。**行为验证**（ForceProcess
  探针 %TEMP%\OssForceProbe.exe）：normal 时探针进 DefaultBox（proc list
  可见）→ `force on 12` 后探针**在沙箱外运行**（tasklist 可见、proc list
  无）→ off 后再次进箱。`force on`（无秒）沿用现配置（30）；错误码：
  on 0 → 7（ForceDisableSeconds=0 语义禁止）、on 99999/abc → 2、未知子命令
  → 2，均按 §6。
- **force 状态生命周期坑（实测定位，§8.24）**：`force on 30` → `server
  stop` → status **normal**（0s 内非窗口到期；直连查询同）——server 是会话
  leader，退出触发驱动 `Session_Cancel` 释放 SESSION 块（disable_force_time
  随之消失）。跨进程直连 on/status（server 不退出）则正常保持（disabled，
  剩余 unknown）。已文档化为上游语义（SandMan 退出同理）。
- **maint（P1-2）**：status（driver running + `device: alive` 交叉核证 /
  service running；--json 双对象行）→ `maint stop --service`（RESULT=ok,
  STATE=stopped；sc query 复核 SbieSvc STOPPED、SbieDrv 仍 RUNNING）期间
  驱动级命令正常（force status 直连、proc list 降级直连）→ `maint start
  --service`（ok/running，sc query 复核 RUNNING；SbieSvc 依赖路径 cfg get
  恢复）→ **系统还原**。幂等：已运行时 start --service → ok。错误码：
  --bogus/未知子命令 → 2。**driver 启停未实测**（任务边界：测试机风险，
  仅 status 分支；`maint start|stop --driver` 代码已实现——KmdUtil.exe 路径，
  标注"未实测"）。
- **kill-all --all（P1-3）**：TestOssA/B 各起 cmd（ping）→ `proc kill-all
  --all`（ipc）→ `8 process(es) terminated (4 box(es))`（A/B 各 4 进程含
  PING/RpcSs/DcomLaunch；4=启用 box 总数）→ 双箱 proc list 空；`--json`
  {count:0,boxes:4,...}；**按 box 兼容**：`kill-all TestOssA` →
  `4 process(es) terminated`（无 box 后缀，与 §13 文案一致）；错误码：
  `--all TestOssA` → 2、无参 → 2、NoSuchBox → 5；直连一致。
- **cfg set --no-refresh（P1-6）**：`cfg set K w1`（refresh）→ 缓存/盘均
  w1 → `cfg set K w2 --no-refresh`（**ipc**）→ 驱动缓存仍 w1、盘仍 w1
  （修复前 IPC 路径会立即热重载为 w2）→ 后续任一 refresh=true 写提交整棵
  SbieSvc 内存树（盘变 w2、缓存 w2）。**语义修正**：refresh=false 变更仅存
  SbieSvc 内存（§8.2 原表述有误，已改）；中途任何 `cfg reload` 经
  NotifyConfigReloaded 清缓存丢弃未提交变更（实测复现）。
- **proc start cwd（P1-7）**：client cwd=`C:\Users\Administrator\OssCwdProbe`
  下 `proc start`（**ipc**，无 --dir）→ `proc info` working_dir=
  `C:\Users\Administrator\OssCwdProbe\`（修复前=server exe 目录）；
  `--dir C:\Windows\Temp` 显式路径仍正确；`--wait` 直连路径退出码透传
  （exit 7 → rc 7）不变。
- **DosToNtPath 自测（附带修复）**：obj\pathtest 独立编译（链 PathMapper/
  DriverApi/Ntdll/Status 四 TU）五断言全过——`C:\Windows`→
  `\Device\HarddiskVolume3\Windows`、根形态 `C:`→`\Device\HarddiskVolume3`、
  NT↔DOS 往返、NtToDosPath QueryDosDeviceW 回退分支（同坑一并修复）、
  UNC 原样保留。

**清理确认**：ini 仅原四节、0 处 Oss/ForceDisableSeconds/ForceProcess 引用
（ForceProcess 探针值、ForceDisableSeconds、OssNoRefreshMark/OssCommitMark
均 `cfg unset` 移除）；TestOssA/B 连目录删除（`C:\Sandbox\Administrator` 仅
DefaultBox/New_Box）；探针 exe 与 OssCwdProbe 目录删除；proc list 0 进程；
`server stop` 收尾、无 sbie-cli 进程残留；**SbieSvc/SbieDrv RUNNING（系统
基线还原）**；SandMan 全程在跑（maint 测试窗口内未干扰服务重启时序）。

**遗留**：

1. **maint driver 启停未实测**（`maint start|stop --driver`/`--all` 含驱动
   分支：实现完整、按任务边界只测 status；首次实测建议在可重启测试机做）。
   maint install/uninstall 归 P2-7。
2. **force status 剩余秒的会话外触发**：SandMan/直连 client 触发的禁用无
   时刻可考（remaining=null，文案明示）；剩余秒推算依赖 server 进程存续
   （server 退出即状态消失，§8.24——上游 SESSION 生命周期语义）。
3. **proc start env 继承差异维持文档化**（04 §12.4 坑 5）：cwd 已收口，
   env 块仍继承路径语义（直连=client env、IPC=server env=首个 client 环境）；
   长期方案 `--env K=V` 显式透传（06 §P1-7 建议注记）。
4. maint stop --driver 若 KmdUtil 报错会弹 GUI MessageBox（GPL 工具自身
   行为）——RunKmdUtil 15s 超时兜底强杀防无人值守挂死；该路径未实测（见 1）。

## 16. 验收记录（波次 A：触发器与生命周期守护，07-P0-1/07-P0-2，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**（全量
rebuild：删 obj/x64\Release 后重建，无告警），产物部署
`Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv 5.73.5 RUNNING /
SbieSvc RUNNING（sc query 复核）；测试窗口前半段 SandMan 常驻（后半段其
自行退出，见遗留 6）；ini = `C:\WINDOWS\Sandboxie.ini`。

**改动清单**（边界内：`SbieCore\Model\Boxes.{h,cpp}` + `sbie-cli\**` + 本文档
+ docs/07 打勾；未动 Recovery/Snapshots 等 Model 与 ipcc 帧格式）：

- `SbieCore\Model\Boxes.{h,cpp}`：+`TriggerStats` / `RunBoxTriggers(box,
  setting, out)`（additive——冻结契约面未动）。触发器执行器：逐条读值 →
  %SANDBOX% 替换 → CreateProcessW（继承环境、CREATE_NO_WINDOW、不继承句柄）
  → 等待 ≤15s → 下一条。
- `sbie-cli\server\Guardian.{h,cpp}`（新建）：空箱守护监视器（1s 轮询
  EnumBoxProcesses 全会话进程数，true→false 转换触发）+ 共享清理执行器
  `ExecuteBoxPurge(name, CleanContents|RemoveRoot, noTriggers)`（原
  Dispatcher.cpp 的 DeleteDirRecursiveLocal/CleanDirContents 副本并入，
  server 模块内统一）。
- `sbie-cli\server\LogPump.{h,cpp}`：+`AppendSyntheticLog(text)`——guardian
  动作合成日志行（msgid=0）入环形缓冲 + 推送 log.watch 订阅者。
- `sbie-cli\server\ServerMain.{h,cpp}`：ServerOptions.+`noGuardians`；
  INIT 3.5 步 StartGuardians（SvcProxy 就位后、不依赖 session leader）；
  EXIT 与管道失败路径 StopGuardians；ready 诊断行 + guardians 位。
- `sbie-cli\server\Dispatcher.cpp`：HBoxClean/HBoxDelete +`no_triggers`
  参数（经 ExecuteBoxPurge 复用触发器逻辑）；HStatus 的 server 段
  +`guardians`(bool)/`guardian_fires`(int)。
- `sbie-cli\main.cpp`：`--start-server` 解析 +`--no-guardians`。
- `sbie-cli\cli\Commands\box_manage.cpp`：`box clean|delete` +`--no-triggers`
  （IPC params no_triggers；直连路径同序执行触发器——检查 → 触发器 → 删除）。
- `sbie-cli\cli\Commands\server_cmd.cpp`：`server start --no-guardians`
  （透传派生；预拉起实例重启交互与 --idle-timeout 同款）；`server status`
  表格 +GUARDIANS 列。
- `sbie-cli\cli\Cli.cpp`：--help 命令树补 --no-triggers/--no-guardians 与
  guardian 键说明。

**§12.2 参数对拍表的增量**（本波次 client 发送面）：

| op | client 发送 | 备注 |
|---|---|---|
| box.clean | name, no_triggers | 写，retry=false；no_triggers 缺省 false（触发器开） |
| box.delete | name, files, keep_section, **no_triggers**, password? | 07-P0-1；仅 files=true 路径执行触发器 |
| status（读） | — | data.server 增 guardians/guardian_fires 字段（additive） |

**语义决策记录**（与 SandMan 规格对照，规格来源 docs/07 §3.1 07-P0-1/2）：

1. **OnBoxDelete 执行时机与等待**：SandMan = 删除内容前逐条执行、宿主命令、
   "异步运行 + UI 进度可取消"。OSS = **命令路径内同步执行**（server op 与
   client 直连两路），每条 CreateProcessW（继承环境）后等待 ≤15s；超时不杀
   进程、继续下一条。依据：07 的 CLI 建议原文即"命令路径内同步执行（server
   op 内执行更自然——避免 client 短生命周期中断）"；15s 上限对齐 07-P1-3
   记录的 SandMan 检查器 15s 超时习惯；"进度/取消"是 GUI 呈现面，CLI 无此
   面，同步有界等待为其 CLI 等价物。
2. **失败策略**：触发器失败（启动失败/非零退出/超时）**继续**剩余命令且
   **不阻断清理**（尽力而为）。07 未记录 SandMan 的失败中止行为 → 采取继续
   并经 TriggerStats 计数（当前未入输出，见遗留 2）。
3. **变量展开**：值**原样**读出（不走驱动 %env% 预展开——驱动在 SYSTEM
   上下文展开，%TEMP% 会被替换成 C:\WINDOWS\TEMP 语义错误）；仅替换
   **%SANDBOX%**（大小写不敏感）为 box 名（07 记录的 SandMan 触发器变量）；
   %TEMP% 等 Windows 变量由子进程（通常 cmd.exe）在继承的宿主用户环境中
   自行展开（实测：marker 落在用户 Temp）。07 未记录的其他变量（%SANDBOXPATH%
   等）**不支持**。
4. **守护选型**：02 文档无驱动侧"box 空"通知 API（仅 1399 进程启动通知，
   无退出通知）→ **1s 轮询** EnumBoxProcesses（任务书认可 1-2s 粒度）；
   空箱判定按**全会话**进程数（all_sessions=true——他 session 进程仍在时
   不误清）；仅 true→false 转换触发（一次生命周期事件，空箱重复触发免疫，
   新进程重新武装）。**粒度限制**：整个存活期落在两个轮询点之间的子秒级
   进程可能漏检（轮询固有；kill 类场景进程存活长，可靠——实测 kill 后
   1-2s 内完成全部守护动作）。
5. **守护执行序**：OnBoxTerminate → 行为键判定 → 清理（含 OnBoxDelete
   触发器）→ AutoRemove 节删除。AutoDelete=清内容保根目录保节；AutoRemove
   与 **Temp_/Local_Temp_ 前缀**（一次性沙箱，07 记录的规则；按任务书规格
   实现为"等同 AutoRemove"——与 SandMan 的"从 Template 列表移除+清模板节"
   形态不同，见遗留 5）=连根目录删除+节删除。NeverDelete 保护优先于一切
   （守护侧跳过并记日志；命令路径报 6）。AutoRemove 的节删除经 SvcProxy
   空密码——锁配置时失败记日志（内容已清）。
6. **监视器开关**：`--start-server --no-guardians`（显式关，最高优先）>
   `GlobalSettings\GuardiansEnabled=n`（配置键）> 缺省**开**。07 未记录
   SandMan 的等价控制键（SandMan 作为常驻管理器恒执行守护，无键面）→
   GuardiansEnabled 为本项目自有键。`server start --no-guardians` 对已运行
   实例幂等不重启（与 --idle-timeout 的预拉起重启交互一致——仅本命令
   预拉起的实例会按选项重启）。
7. **可见性**：guardian 动作合成日志行（msgid=0，"guardian: " 前缀）入
   log 环形缓冲并推送 log.watch 订阅者（实测 `log dump` 可见）；log dump
   依赖日志泵 = session leader 在场——SandMan 持 leader 时不可用（报
   SERVER_UNAVAILABLE），此时守护行为仍可经文件系统/配置状态观测。
   `server status` 增 GUARDIANS 列（--json：server.guardians +
   server.guardian_fires 计数）。

**实测**（全部真实驱动/SbieSvc；`--show-transport` 核实新参数路径均
`transport: ipc`，`--no-server` 对照走 direct）：

- **OnBoxDelete（07-P0-1）**：TestOss 设 `OnBoxDelete=cmd /c echo deleted >
  %TEMP%\oss_trig.txt`（`box get --raw` 回读原样含 %TEMP%——写后回读通）→
  沙箱内 `cmd /c echo > C:\sbie_mark.txt` 造内容 → `box clean`（ipc）→
  `cleaned` + **触发器文件生成（"deleted"）+ 内容清空**；`--no-triggers`
  → 内容清空且**无**触发器文件；`box delete --files`（ipc）→ 触发器文件
  生成 + 根目录/节全删；直连（--no-server）同语义。**%SANDBOX% 变量**：
  `cmd /c echo box=%SANDBOX% > %TEMP%\oss_var.txt` → 文件内容 `box=TestOss`。
- **守护监视器（07-P0-2，全部 kill 驱动、guardian_fires 计数核证）**：
  TestOss 设 `OnBoxTerminate=cmd /c echo term > %TEMP%\oss_term.txt` +
  `AutoDelete=y` → 起 `ping -n 30` → `proc kill-all` → **~1s 内** fires+1、
  OnBoxTerminate marker 生成、内容清空、**节保留**；改 `AutoRemove=y` →
  kill-all → **~1s 内** fires+1、**根目录+节全删**；`Temp_OssA`（无任何键，
  仅前缀）→ kill-all → **~1s 内**整箱自动删除。NeverDelete=y + AutoDelete=y
  → kill 后**不清理**，log dump 可见 `guardian: box 'TestOss' became empty:
  cleanup skipped (NeverDelete=y)`；`box clean` 同箱报 6（ACCESS_DENIED）。
- **开关**：`server stop` → `server start --no-guardians` → status
  GUARDIANS=**no**，kill 驱动空箱后 fires 恒 0（监视器确证关闭）；
  `cfg set GuardiansEnabled n`（GlobalSettings）→ 重启 → GUARDIANS=no；
  `cfg unset` → 重启 → GUARDIANS=yes。`server status` 新列输出：
  `RUNNING PID UPTIME_SEC CLIENTS IDLE_REMAINING_SEC LOG_PUMP GUARDIANS`。
- **回归**：无键箱 `box clean`/`box delete`（ipc 与 direct）原路径不变；
  `box clean NoSuchBox` → 5；`server status`/`box list`/`proc list`/
  `log dump` 正常；触发器键值经 `box set`/`box get --raw` 读写回读一致
  （键面通用性维持，无需专用命令）。

**清理确认**：TestOss/Temp_OssA/TestOss2 节与目录全删（`C:\Sandbox\
Administrator` 仅 DefaultBox/New_Box）；ini 仅原四节，0 处 TestOss/
Temp_Oss/guardian 键引用（含 SandMan 用户节 BoxGrouping 的 TestOss 残留已
经 `cfg set` 复位为 `:DefaultBox,New_Box`）；触发器 marker 文件
（oss_trig/oss_var/oss_term）与诊断脚本全删；`server stop` 收尾、无
sbie-cli 进程残留；**SbieDrv/SbieSvc RUNNING（系统基线还原）**。

**遗留**：

1. **轮询粒度**：子秒级短命进程的空箱事件可能落在两个轮询点之间漏检
   （kill 类可靠）；长期可改 SBIE 日志事件驱动，但需 session leader 在场
   （SandMan 共存时不可用），且 02 文档未记录进程退出通知 msgid。
2. **触发器执行计数未入命令输出**：box clean/delete 的 data.message 维持
   "cleaned"/"deleted" 契约不变；TriggerStats 已采集，后续可在 --json 加
   triggers_run/triggers_failed 字段。
3. **AutoRemove 守护路径无密码**：锁配置（EditPassword）时守护的节删除失败
   （内容已清、记日志）；命令路径不受影响（--password 可用）。
4. **SandMan 共存双执行者**：同会话 SandMan 常驻时其自身也执行这四个键
   （触发命令可能被执行两次、清理竞争）。实测早期 guardian 观察被 SandMan
   行为掩蔽，以 guardian_fires 计数作判别器完成隔离验证。
5. **Temp_ 语义形态差异**：SandMan 的 Temp_ 模板清理为"从 Template 列表
   移除+清模板节"（07 §3.1 ②）；本实现按任务书规格 = 箱名前缀 Temp_/
   Local_Temp_ 等同 AutoRemove（内容+节删除）。模板节清理形态未实现。
6. **SandMan 在测试窗口中自行退出**（前半段常驻、约 13:31 后不在——无任何
   sbie-cli 命令以其为目标；其后本 server 取得 session leader，log pump
   转为可用并完成了 log dump 验证）。

## 17. 验收记录（波次 B：建箱预设 / 复制 / 导出导入 / 恢复增强，07-P1-1/2/3/4，2026-09-27）

构建：`build_oss.bat` Release x64 **0 error / 0 warning（/W4 /WX）**，产物
`Installer\SbiePlus_x64\sbie-cli.exe`。环境：SbieDrv 5.73.5 RUNNING /
SbieSvc RUNNING；ini = `C:\WINDOWS\Sandboxie.ini`；测试全程 SandMan 常驻
（本波次无守护交互面，无掩蔽问题）。

**接手说明**：本波次由前任 agent 中断后接手完成。前任已写完四任务全部代码面
（Model/Util/CLI/server op/注册接线齐全，构建一次通过），**未实测、未写文档**；
且其中 zip 写入器存在两处格式 bug（见"接手修复"）。遗留现场：TestOss（Note
= "wave B copy probe"，SandMan 向导形态）/TestOss2 两测试箱 + 一个旧二进制
server 进程，均已于本波次清理/重启。

**改动清单**（边界内：`SbieCore\Model\**`、`SbieCore\Util\Zip.{h,cpp}`（新）、
`sbie-cli\**`、docs/04/07）：

- `SbieCore\Model\BoxTransfer.{h,cpp}`（新）：07-P1-1/2/4 的 Model 面——
  `BoxTypePresets()`（六类键组表）/`FindBoxTypePreset`/`ApplyBoxTypeKeys`（预设
  键组落盘，refresh 收尾）；`ReadBoxSection`（节整读 "Key=Value\n" 行集）/
  `CopyDirTree`（目录树拷贝，重解析点跳过口径同 BoxUsage/Recovery）；
  `CopyBox`（节替换写入新名 + 可选内容）；`ExportBox`/`ImportBox`（包 =
  box.ini + content/ 树，zip 或目录形态）。
- `SbieCore\Util\Zip.{h,cpp}`（新）：最小 zip 读写器（PKWARE APPNOTE store
  子集自研，零第三方依赖）——本地头顺序流出 + 集中目录 + EOCD；读侧 EOCD
  回扫 + 集中目录全量载入（≤8 MiB 防病态包）+ 逐条目流式解出（CRC32 校验）；
  UTF-8 文件名（通用位 bit 11）、DOS 时间戳 2s 精度、32 位尺寸（无 zip64）。
- `SbieCore\Model\Recovery.{h,cpp}`：+`RecoverCopyOptions`（move/runCheckers）
  /`CopyEx`（Copy 的超集，原 Copy 冻结为 legacy 委托）；outcome +
  skippedFiles/skippedPaths（检查器拒绝清单）；检查器执行器 `RunFileChecker`
  （值 + %SANDBOX% 展开 + 带引号沙箱路径参数，宿主执行 ≤15s，非零/超时/启动
  失败 = 拒绝）；move = 拷后删源（删源失败 = 该条目失败，已拷目标保留=重试
  安全）。
- `sbie-cli\cli\Commands\box_create.cpp`：`--type <t>`（未知类型 7 +
  指向 `box types`）+ `CmdBoxTypes`（预设表本地渲染，无 IO/无 server 依赖）。
- `sbie-cli\cli\Commands\box_transfer.cpp`（新）：`box copy/export/import`
  三命令（IPC 优先 `box.copy/box.export/box.import`，降级直连 Model）。
- `sbie-cli\cli\Commands\box_recover.cpp`：`copy` + `--move`/`--no-check`
  （参数 move / on_file_recovery）；skipped_paths 数据面输出（文本逐行
  "skipped: <path>"，JSON 数组）。
- `sbie-cli\server\Dispatcher.cpp`：HBoxCreate +type；HBoxCopy/HBoxExport/
  HBoxImport 三个写 op（copy/import 经 SvcProxy——节写入触 SbieSvc；export
  纯文件 IO worker 直执）；HRecoverCopy +move/on_file_recovery。
- `sbie-cli\ipcc/SbieIpc.h`：+`kOpBoxCopy/kOpBoxExport/kOpBoxImport`。
- `sbie-cli\cli\Cli.cpp`/`Commands.cpp`/`BoxProcCommands.h`：--help 命令树、
  注册接线、`--type` 值旗标表。

**§12.2 参数对拍表的增量**（本波次 client 发送面）：

| op | client 发送 | 备注 |
|---|---|---|
| box.create | name, templates[], password?, **type?** | 写，retry=false；type 缺省 = 现行为 |
| box.copy | src, dst, content, password? | 写，retry=false；server 经 SvcProxy |
| box.export | name, to, archive | 产物幂等覆盖，retry=true（box.size 同类） |
| box.import | path, name, archive, password? | 写，retry=false；server 经 SvcProxy |
| recover.copy | name, paths[], to?, overwrite, **move, on_file_recovery** | 写，retry=false；文件 IO server 侧 |

**语义决策记录**（与 SandMan 规格对照，规格来源 docs/07 §3.2）：

1. **六类 = 7 类减 1**：Confidential Encrypted（UseFileImage 链路）许可证
   禁，不出预设表（07 建议"加密类型显式报许可证禁用"——不出表比报错更早
   失败，`box types` 即文档）。standard = 0 键（仅 Enabled=y，向导缺省）。
   键序：hardened-plus 先 UsePrivacyMode 后 UseSecurityMode（向导落键序，
   无语义差）；refresh 收尾（中间写不热重载，rename/HBoxSet 同款节流）。
   向导另写 BorderColor（GUI 主题色）——视觉键域（07-N-A-1），CLI 不写。
2. **zip vs --dir 决策**：**双形态都做**——`--to` 以 `.sbx`/`.zip` 结尾 =
   zip 归档（store，自研读写器），否则目录形态（`<dest>\box.ini` +
   `<dest>\content\`）。依据：07 建议原文"首波做 zip（store 模式）+ --dir"；
   单文件归档是机器迁移的自然形态，目录形态给版本控制/手工检视/挂网络共享
   的用户。导入侧自动识别（已有目录 = 目录包，否则按归档打开）。**zip 限制
   （文档化）**：仅 store 条目（读侧遇压缩条目报错——本工具不读第三方压缩
   包，但产出物可被资源管理器/bsdtar 读取，实测互操作）；无 zip64（单条目
   <4 GiB、全档 <4 GiB）；mtime 2s 精度（zip 格式固有）；文件属性不保存。
   **口令加密归档未做**（zip 传统加密弱、AES 无标准对称互操作，07 已列后续；
   需要时叠加 age/gpg 外部工具即可）。
3. **复制不触发 OnBoxDelete**：复制语义源箱只读（整节读出 + 新节写入），
   不删任何内容/节 → 触发器与守护监视器（07-P0-2）均不参与。dst 已存在 →
   NOT_FOUND(5)（CLI 语义 5 复用："源不存在/目标已存在"合一消息）；src==dst
   /dst 名非法 → INVALID(7)。**组归属与别名**（07 记录的 SandMan Duplicate
   附带行为）未跟随——BoxGrouping 是 SandMan 用户节 GUI 组织数据（07-N-A-3），
   CLI 不复制。
4. **move 语义 = 拷后删源**（非 rename）：跨卷安全（FileRoot 与恢复目标可
   在不同卷）；删源失败 = 该条目失败（已拷出的目标保留——重试时 --overwrite
   即可收敛，比"删源失败回滚目标"更简单且不丢数据）。检查器在**拷贝前**逐
   文件执行（拒绝 = 跳过该文件、继续其余，整体仍 OK——被拒文件沙箱内原样
   保留、经 skipped_paths 列出）。
5. **检查器缺省执行**（与 07 建议的 `--check` 开启式相反）：键存在即校验是
   更安全的缺省（检查器的意义就是拦恢复；显式 `--no-check` 逃生）。命令拼装
   = 键值 + 空格 + 带引号沙箱路径（07 记录的 SandMan CheckFilesAsync 语义），
   %SANDBOX% 展开复用波 A `ExpandSandboxVar`（共享助手）。
6. **导出幂等覆盖**（retry=true）：产物重复生成无破坏语义（CREATE_ALWAYS /
   目录形态覆盖同名），与 box.size 同类；copy/import 非幂等（retry=false）。
7. **空内容箱的导出/复制**：未初始化（无 FileRoot 目录）= 0 文件成功（节
   键仍完整往返）；导入侧 content/ 缺失同样 0 文件成功。坏包判定 = box.ini
   缺失/不可读/空 → INVALID(7)；包不存在（文件或目录）→ NOT_FOUND(5)。

**接手修复**（前任 zip 写入器两处格式 bug，自测发现）：

1. **集中目录条头多写一个 u16 零**（extra/comment/盘号/内部属性后多一个
   → 每条目 48 字节而非 46）：第三方解压器（Windows bsdtar/libarchive）
   报 "Invalid central directory signature"——前任未实测互操作故未暴露。
   修复：删去多余 WriteU16(0)，46 字节固定头按 APPNOTE §4.3.12。
2. **本地头偏移记录错位**：ReserveEntry 在 `&&` 链末尾（名字写完后）以
   `offset_-30` 回推，多扣了名字长 → 集中目录记录的偏移 = 起点+名长，自产
   自读报 "zip local header corrupt"。修复：BeginEntry 入口先记 start。
3. **（补齐）import 归档文件不存在错误码**：原走 ZipReader::Open 失败 =
   GENERIC(1)，与目录形态包不存在的 NOT_FOUND(5) 不一致；Model 层先查
   GetFileAttributes → NOT_FOUND，两路径同为 5。

**实测**（全部真实驱动/SbieSvc；`--show-transport` 核实新参数路径均
`transport: ipc`，`--no-server` 对照走 direct）：

- **--type 六类（07-P1-1）**：六箱分别 hardening/hardened-plus/standard/
  standard-plus/app/app-plus → 特征键逐一 `box get` 核验：UseSecurityMode/
  UsePrivacyMode/NoSecurityIsolation/Template=RpcPortBindingsExt（app 型追加）
  组合全部与预设表一致；standard 仅 Enabled。未知类型 `--type secret` → 7
  +提示 `box types`；重名 → 5；直连路径同语义；`box types` 文本表 + JSON
  （含 keys 数组）输出正确。
- **box copy（07-P1-4）**：键集一致（T_hardened_plus → T_copy1：Enabled/
  UsePrivacyMode/UseSecurityMode 三键对齐）；--content 带文件（4 文件 35 B，
  drive\C 嵌套子目录 + user\current 树全数落位，内容 diff 一致）；错误路径
  （dst 已存在 5 / src 不存在 5 / 同名 7 / 名非法 7）；直连（--no-server）
  含 --content 同语义；源箱文件全程未动（复制只读语义）。
- **box export/import（07-P1-2）**：zip 形态（.sbx）导出 4 文件 → **Windows
  bsdtar 交叉验证通过**（-tf 列目录、-xOf 读 box.ini、完整解压——修复后）；
  目录形态导出（box.ini + content\ 树）；**错误路径核心场景**：删原箱 →
  zip import T_back → 键（Enabled/UsePrivacyMode/UseSecurityMode）与 4 文件
  齐全、内容 diff 一致；目录包 import T_back2 同；import 到已存在名 → 5；
  空目录坏包 → 7（box.ini missing）；包文件/目录不存在 → 5；export 不存在
  箱 → 5；空内容箱（0 文件）zip 往返键集对齐（NoSecurityIsolation +
  Template 均在）；直连路径 export+import 同语义；JSON 输出（files/dirs/
  bytes/archive/message）正确。
- **recover --move + 检查器（07-P1-3）**：`--move` 拷后删源（sandbox 目录中
  源文件消失、目标内容一致，IPC 与 direct 两路径）；OnFileRecovery=
  `cmd /c exit 3` → `0 file(s) recovered, 1 skipped by OnFileRecovery` +
  skipped 清单（文本 "skipped: <path>" 逐行 + JSON skipped/skipped_paths），
  源保留；`--no-check` → 放行恢复；检查器 `cmd /c exit 0` → 正常恢复
  （--move + --overwrite 组合亦可）；**%SANDBOX% 展开**：检查器命令
  `cmd /c echo sandbox=%SANDBOX% > <marker>` 收到 `sandbox=T_rec` + 带引号
  沙箱路径参数。目标已存在保护（+ --overwrite 提示）在 move 路径同样生效。
- **回归**：无 --type 建箱（仅 Enabled）/box clean/delete（无键箱原路径）/
  box rename 往返/box enable-disable/cfg get --section/template list/
  proc list/log dump/server status/version 全部正常；`box clean NoSuchBox`
  → 5。六类箱与全部 T_* 测试箱最终 `box delete --files` 清理成功。

**清理确认**：全部 T_* 测试箱（六类 + copy/import/move 系列）节与目录全删；
前任遗留 TestOss/TestOss2（wave B copy probe）节与目录全删；导出物目录
`C:\Users\Administrator\Documents\sbie\oss_b_test\`（.sbx/.zip/目录包/
xcheck/rec_out/chk_var.txt）整目录删除；`C:\Sandbox\Administrator` 仅
DefaultBox/New_Box；ini 仅原四节 + 无任何 T_/OssRecT/OnFileRecovery 测试键
残留；`server stop` 收尾、无 sbie-cli 进程残留；SbieDrv/SbieSvc RUNNING
（系统基线还原）。

**遗留**：

1. **--type 高级旗标未做**（07-P1-1 建议的 --location/--v2-delete/--temp/
   --auto-recover/--block-net/--drop-admin）：键均可经 `box set` 组合达成，
   旗标属便利层，列为后续波次（07 §5.2 波次 8 剩余项）。
2. **加密/口令归档未做**（07-P1-2 的后续项）：zip 传统加密弱；建议需要时
   外部工具叠加或等 AES 互操作决策。
3. **zip 读侧仅 store**：本工具不读第三方压缩包（报错清晰）；产出物可被
   资源管理器/bsdtar 读取（互操作实测）。压缩（deflate）未做——沙箱内容
   多为可压缩文本，后续可在 store 之上叠加自研 deflate（无依赖约束内的
   格式面扩展）。
4. **copy 不跟随 BoxGrouping/别名**（07-N-A-3，SandMan 用户节 GUI 组织
   数据，见决策 3）；import 的冲突处理（改名/跳过/覆盖三选）简化为
   "已存在即 5 报错"——CLI 用户可用 --name 显式改名达成等价。
5. **前任遗留现场教训**（流程性）：未实测的格式化代码（zip 二进制布局）
   不能视为完成；本波次两处 bug 均为"写完未验"的典型（第三方解压器一验
   即暴露）。已记入本节供后续波次自检。

## 18. 验收记录（波次 D2：磁盘映像 / RAM 盘 / USB 沙箱运维域，2026-09-27）

对应 07 报告运维域条目：07-P2-1（USB 沙箱自动接管）、07-N-A-7（RAM 盘）、
07-N-A-8/06 N-A-1（磁盘映像加密箱）与 EnableEFS 键（07 §2 表）。规格锚点
见 docs/07-deep-gap-analysis.md 对应行（SandMan 来源行号在那里）。

### 18.1 可行性核实结论（决定实现形态，写键即生效 vs pipe 面）

实读 Sandboxie core（GPLv3）后的执行者定位（07 §2 表旁注同步更新）：

1. **UseFileImage / UseRamDisk 的自动挂载执行者在 SbieSvc core**：
   `core/svc/DriverAssistInject.cpp:160` 在每个沙箱进程注入路径调用
   `MountManager::AcquireBoxRoot`（`core/svc/MountManager.cpp:1081-1213`）——
   读箱键 `UseFileImage`/`UseRamDisk`；RAM 盘用全局 `RamDiskSizeKb`（下限
   100MB，低于则记日志 2238 拒挂）+ `RamDiskLetter`（空=自动分配），
   **共享单盘**（m_RamDisk，盘内每箱一个 `<boxname>` 目录）；映像箱用
   `<FileRootPath DOS>.box`（GetImageFileName，:1066-1073）。挂载由 SbieSvc
   以子进程拉起安装目录 `ImBox.exe`（SandboxieTools 运行时；SbieSvc 侧组
   `ImBox type=ram|img image=… cipher=AES size=… mount=<盘符> format=ntfs
   proxy=…!… event=… mem=<ptr>`——命令行格式仅按任务书授权参考其源码，
   未拷任何代码），再把 FileRoot 变为指向 `\Device\ImDiskN\Sandbox`（RAM
   盘为 `\Device\ImDiskN\<boxname>`）的 junction。**结论：`box set
   UseFileImage=y` 写键即生效（下一次箱内首进程启动自动挂载），CLI 不在
   启动路径上做任何事。**
2. **显式挂载/卸载/枚举/查询在 SbieSvc 有完备 pipe 面**：
   `MSGID_IMBOX_CREATE/MOUNT/UNMOUNT/ENUM/QUERY/UPDATE`（0x1D01-0x1D06，
   `core/svc/MountManager.cpp` Handler，挂在与 SBIE_INI 相同的
   `\RPC Control\SbieSvcPort` LPC 端口；QSbieAPI 的 ImBoxCreate/ImBoxMount/
   ImBoxUnmount/ImBoxEnum 即此协议的 LGPL 侧实现）。守卫仅拒绝沙箱内调用
   者（`SbieApi_QueryProcess` 成功即拒）。**结论：CLI 经既有 SvcClient 直
   达，无需 ImBox.exe 子进程封装、无需加载 sbiedll.dll 的 SbieDll_Mount
   导出。**注意：该组回复 status 为 **win32 错误码**（SHORT_REPLY(ERROR_*)
   惯例，与 SBIE_INI 的 NTSTATUS 不同）——SvcClient::ImBox* 实现按高位
   区分（0xC000xxxx=NTSTATUS，否则按 win32 映射：ERROR_DEVICE_NOT_AVAILABLE
   →DRIVER_UNAVAILABLE=ImDisk 缺席、ERROR_NOT_FOUND→NOT_FOUND=未挂载）。
   **vendor/MountManagerWire.h 本波次新增**（01-license-map §2 本就允许
   复制的 GPLv3 wire 头，逐字未改 + 适配头；03 §7 原"许可证禁用不做"的
   记述对这组 msgid 不成立——协议与编排全在 core，仅运行时组件在
   SandboxieTools，已在本节纠正）。
3. **EnableEFS 纯键驱动**：SbieDll 在沙箱进程内代理 EFS 属性文件打开
   （`core/dll/file.c:4735-4781` File_NtCreateFileProxy，队列
   `*USERPROXY_%08X`）→ SbieSvc `UserServer::OpenFile` 查箱键
   `EnableEFS`（`core/svc/UserServer.cpp:642`）放行宿主侧打开（EFS 需用户
   profile 密钥，故必须由用户态服务代开）。**结论：无 CLI 触发面/状态机，
   状态呈现并入 `img status`（enable_efs 字段）。**
4. **USB 接管（07-P2-1）**：SandMan 侧守护行为（UpdateForceUSB，
   `SandMan.cpp:2272-2321`，custom license——本波次仅按 07 已记录的规格
   语义实现，未参考其代码）：ForceUsbDrives=y 时枚举 USBSTOR 卷（卷序列
   号 `HHHH-LLLL` 为标识），未被 DisabledForceVolume 排除的卷的挂载点
   整表写入 UsbSandbox 箱的 ForceFolder；箱不存在则创建并设
   UseFileDeleteV2/UseRegDeleteV2/UseVolumeSerialNumbers 三键。**CLI 等价
   形态 = `usb sync` 一次性命令**（守护形态为可选后续：挂 server 周期任务
   或计划任务调用本命令）。
   **卷枚举实现决策**：不用 SetupAPI 枚举 USBSTOR 枚举器（SandMan 路径），
   改为对每个卷设备发 `IOCTL_STORAGE_QUERY_PROPERTY` 查 BusType==BusTypeUsb
   （纯 kernel32：FindFirstVolumeW + GetVolumePathNamesForVolumeNameW +
   GetVolumeInformationW），零新依赖；语义覆盖更宽（UASP 等 USB 附加 SCSI
   总线亦计入；USBSTOR 枚举器只覆盖传统 BOT）。总线查询失败的卷（无介质
   等）标 bus_known=false 不计入接管、状态面如实呈现。

### 18.2 命令面与 op（本波次新增）

```
sbie img    list                          已挂载根枚举（IMBOX_ENUM）            [img.list]
            status [<box>]                键面+镜像文件+挂载状态聚合（单箱键值/  [img.status]
                                          全箱表）
            create <box> --size-mb <N>    创建镜像（IMBOX_CREATE；下限 256MB，  [img.create]
                [--password <pw>]         口令=AES；走 SbieSvc→ImBox 格式化周期）
            mount <box> [--password <pw>] 显式挂载（IMBOX_MOUNT；              [img.mount]
                [--protect|--no-protect]  --protect 缺省=ForceProtectionOnMount
                [--admin-only|--no-admin-only]    键；--auto-unmount=空箱自动卸载）
                [--auto-unmount]
            unmount <box>                 显式卸载（IMBOX_UNMOUNT，幂等）        [img.unmount]
sbie ramdisk status                       RamDiskSizeKb/Letter + 使用箱 + 共享  [ramdisk.status]
                                          盘挂载状态（空 reg_root QUERY）
sbie usb    status                        ForceUsbDrives/UsbSandbox/            [usb.status]
                                          DisabledForceVolume/现 ForceFolder +
                                          卷表（SN/卷标/挂载点/USB/是否接管）
            sync [--dry-run]              07-P2-1 一次性接管（见 18.1.4）        [usb.sync]
```

文件与 op：`SbieCore/Model/DiskImage.{h,cpp}`（img+ramdisk 领域）、
`SbieCore/Model/UsbSandbox.{h,cpp}`（卷枚举+接管）、`vendor/MountManagerWire.h`、
`SvcClient::ImBoxCreate/ImBoxMount/ImBoxUnmount/ImBoxEnum/ImBoxQuery`（便捷层，
additive）、`cli/Commands/disk_img_cmd.cpp`、`cli/Commands/usb_cmd.cpp`、
Dispatcher `HImgList/HImgStatus/HImgCreate/HImgMount/HImgUnmount/HRamDiskStatus/
HUsbStatus/HUsbSync`（ImBox 调用经 SvcProxy 专职线程，03 §1）。读 op
（img.list/status、ramdisk.status、usb.status）retry=true 可降级直连；写 op
（img.create/mount/unmount、usb.sync）retry=false 不降级。§4 命令树、§5 降级
表、PrintUsage 同步更新。

### 18.3 实测（本机真实驱动/SbieSvc/ImDisk/ImBox.exe 全在场；`--show-transport` 确认 ipc）

1. **只读状态面（IPC + 直连双路径）**：
   - `img list` → `no mounted roots`（真实 IMBOX_ENUM 往返，rc 0）；
   - `img status` 全箱表 / `img status DefaultBox` 单箱键值（image_file=
     `C:\Sandbox\Administrator\DefaultBox.box`、mounted=no）；
   - `ramdisk status` → 未配置/未挂载（QUERY 空 reg_root 回 NOT_FOUND 的
     正确折叠，known=true mounted=no）；
   - `usb status` → 5 卷枚举与 PowerShell Get-CimInstance Win32_Volume 逐卷
     对拍一致（C:=549F-03CD、D:/E:=CF5B-E55D CD-ROM、两无盘符系统卷；
     均非 USB 总线，TAKEN=no）；
   - `--no-server` 直连路径同数据。
2. **参数错误路径**：img create 缺 --size-mb / 100MB → 7（两条消息分立）；
   img create/mount/status 不存在箱 → 5；usb sync（含 --dry-run）在
   ForceUsbDrives=n → 7 + 开键提示。
3. **usb sync 写路径（临时键全还原）**：设 `ForceUsbDrives=y` +
   `UsbSandbox=D2TestBox` → dry-run（0/0）→ 实跑：自动建箱（Enabled=y）+
   三初始键 + 空 ForceFolder（整体删除语义）→ 回读 box get/list-setting
   全部落盘正确 → `box delete D2TestBox` + 两 cfg unset 还原（cfg get 复核
   NOT_FOUND）。本机无 USB 卷，**非空 ForceFolder 写入路径（Set 首值+Append
   余值）未实测**——与 box set --append 同一 ConfigStore 通道，逻辑同源。
4. **img create/mount/unmount 真实全周期（临时箱，测后彻底还原）**——
   安全评估：空箱无进程、protect 关（不触 API_PROTECT_ROOT 驱动态）、
   ImDisk 驱动在场且健康（sc query imdisk=RUNNING）：
   - `img create D2ImgBox --size-mb 300` → 产物 `C:\Sandbox\Administrator\
     D2ImgBox.box` 恰 314,572,800 字节（SbieSvc→ImBox.exe 挂载-格式化-卸载
     周期真实执行）；
   - `img mount D2ImgBox` → junction `D2ImgBox [\Device\ImDisk0\Sandbox]`
     （dir 实证）；`img status` mounted=yes；`--json` 给出 mount 明细
     （disk_root=\Device\ImDisk0、disk_size=314570752、used_size=9371648）；
     `img list` 列出 `\REGISTRY\USER\Sandbox_Administrator_D2ImgBox`；
   - `img unmount D2ImgBox` → 卸载干净（img list 空、junction 消失、
     mounted=no、image_exists 保留）；
   - 还原：`box delete D2ImgBox`（节）+ 删 .box 文件；box list/img list/
     目录列表复核无残留。
5. **不可测项（明示）**：
   - **口令（AES）路径**：create --password + mount --password 未做真实
     加密镜像往返（wire 通道与无口令路径完全相同，仅 password 字段差异；
     口令错误映射 ERR_WRONG_PASSWORD→日志 2243 由 SbieSvc 侧产生）；
   - **--protect/--admin-only**：触发驱动 API_PROTECT_ROOT 全局保护态，本机
     SbieSvc 在服务系统，不做（键参数接线已核，缺省=false 不触）；
   - **--auto-unmount**：需箱内进程起止周期驱动（AcquireBoxRoot/ReleaseBoxRoot
     联动），形态上等价于"空箱守护"波 A 已验收的轮询域，未单独实测；
   - **UseRamDisk 真实挂载**：需写全局 RamDiskSizeKb 并起 RAM 盘箱进程（吃
     内存且常驻），QUERY 状态面已实测（未配置态），挂载执行链与 UseFileImage
     同源（MountManager 同一 MountImDisk），标注环境未测；
   - **usb sync 非空 ForceFolder**（见 18.3.3）；**UASP/BOT 差异**：本机无
     USB 存储设备，BusType 判定只验证了"非 USB 卷正确排除"的方向。
6. **坑记录**：
   - `GetVolumePathNamesForVolumeNameW` 的 lpcchReturnLength **不可为 NULL**
     （传 NULL 静默失败→挂载点恒空；实测发现，修复后与系统卷清单对拍）；
   - ImBox 组回复 status 为 win32 错误码（见 18.1.2），FromNtStatus 会全部
     折叠为 GENERIC——SvcClient 侧显式双轨映射；
   - IMBOX_CREATE/MOUNT_REQ 的 file_root 声明为 [1] 的变长尾区：定长安全
     函数（wcscpy_s）按声明尺寸 [1] 必失败，须 memcpy 到 calloc 全长区
     （QSbieAPI 用裸 wcscpy 同理）；
   - usb sync 多键写序列的落盘语义：SbieSvc refresh=false 只改内存树，
     **末条写必须 refresh=true** 提交（§8.4 坑的波 D2 应用实例）。

### 18.4 遗留

1. **usb sync 的守护形态**（周期自动接管）未做——一次性命令已覆盖语义，
   常驻化建议挂 server Guardian 域后续扩展（需盘符变更通知
   WM_DEVICECHANGE，server 无窗口）或用户计划任务。
2. **img 镜像口令变更/扩容**（IMBOX_UPDATE）SbieSvc 侧本身未实现
   （MountManager.cpp UpdateHandler 返回 ERROR_CALL_NOT_IMPLEMENTED），
   CLI 不暴露。
3. **ConfidentialBox/LessConfidentialBox** 仅状态呈现（键面生效者为
   SbieDll），建箱预设的 Confidential 类型仍按 07-P1-1 决策（映像链路许可
   证域）不含于 `box create --type`——但本波次已证明挂载链路核心侧可用，
   后续可复议加回 `--type confidential`。
4. **ramdisk status 的 boxes 列**：IPC 路径渲染为逗号连接串（JSON 原生数组），
   直连路径同语义（跨传输字段形态一致性按 §7.2 snake_case 契约不破）。


---

## 19. 波次 D3：P2 收口命令面（07-P2-2..5 + 06 P2-3..14 剩余项）

对应缺口：07-P2-2（箱类型派生）、07-P2-3（Start.exe 伪命令路由）、07-P2-4
（doctor 精简版）、07-P2-5（浏览器兼容模板生成器）、06 P2-3（快照默认标记）、
P2-4（空置/初始化状态）、P2-5（proc info 派生列）、P2-6（箱级挂起/恢复含
--all）、P2-7（组件装卸）、P2-9（节全量导出用户面）、P2-12（cfg whoami）、
P2-13（box explore）、P2-14（proc exempt）。处置全表见 §21.3。

### 19.1 命令面（本波次新增/覆盖增强）

```
sbie box    create <name> [--location <dir>]   高级旗标（无旗标=原 IPC 路径；  [无 op——SbieSvc 直连]
            [--temp] [--v2-delete]             有旗标时纯 client 键面写）
            [--auto-recover] [--block-net]
            [--drop-admin] [--type <t>]...
            info <name>                        +type(七类派生)/never_delete/   [box.info+本地补列]
                                               auto_delete/empty/initialized
            list [--type <t>]                  按派生类型过滤                   [本地派生过滤]
            snapshot default <box> [<id>|--clear]  [Current] Default 读写       [box.snap.default——
                                                                            波 E 08-P2-5 接线]
            dump <name>                        原始节全量导出（ini 片段形态）  [无 op——驱动缓存读]
            explore <name>                     宿主 explorer 打开 FileRoot     [无 op——ShellExecute]
sbie proc    suspend-box <box>|--all           箱级整体挂起/恢复              [proc.suspendBox/
            resume-box  <box>|--all            （SuspendResumeAll）            proc.resumeBox——波 E
                                                                            08-P2-4 接线]
            info <pid>                         +flags_decoded/image_type/       [proc.info+本地补列]
                                               elevated/wow64
            exempt <pid> <on|off|get>          API_PROCESS_EXEMPTION_CONTROL   [无 op——ioctl 直投]
            [--what internet|spooler]          直投（'inet'/'splr'）
            start <box> default_browser|...    Start.exe 伪命令路由（改写为     [原 proc.start op]
            ...（伪关键字）                     "Start.exe <pseudo>" 后转发）
sbie cfg     whoami                            SbieSvc IniGetUser 三元组+节存在 [无 op——SbieSvc 直连]
            dump [<section>]                   无参=节清单；给节名=原始节导出  [无 op——驱动缓存读]
sbie template gen-browser [--browser <name>]   探测+生成 [Template_Local_*]；   [无 op——SbieSvc 直连]
            [--access <a,b,...>] [--no-force]  缺省 dry-run
            [--box <NAME> --install|--remove]
sbie maint   install [--driver|--service|--all]  KmdUtil install（参数优先取    [无 op——KmdUtil 子进程]
            [--image <path>]                     注册表先前值）
            uninstall [--driver|--service|--all] 读回显注册参数→停→KmdUtil    [无 op——KmdUtil 子进程]
                                               delete
sbie doctor                                   只读体检（§19.2）                [无 op——本机直查]
```

### 19.2 关键语义决策

1. **类型派生（07-P2-2）**：Hardened=UseSecurityMode（+Plus=叠加
   UsePrivacyMode）、Compartment=NoSecurityIsolation、Insecure=
   UnsecureDebugging、Private=UseFileImage+ConfidentialBox；输出用波 B
   预设名（hardening/hardened-plus/standard/standard-plus/app/app-plus/
   insecure/private）。读生效视图（含模板回退）。
2. **gen-browser（07-P2-5）**：探测=注册表 App Paths（HKLM 64/32 视图+
   HKCU）→ 标准安装目录 → 用户数据目录（Chromium 系 User Data\Default→
   首个 "Profile *"→根兜底；Gecko 系 Profiles\* 通配形）。命中
   %LocalAppData%/%AppData% 根时变量化为 "%Local App Data%"/"%AppData%"
   （drv conf_expand.c 两种写法都展开）。键面=GPL Templates.ini 浏览器族
   形态（Tmpl.Title/Tmpl.Class=WebBrowser/ForceProcess=<exe>/
   OpenFilePath=<exe>,<profile>\模式）；访问类别 bookmarks|history|cookies|
   passwords|preferences|profile（按引擎分档）。写入=SbieSvc 整节替换
   （setting 空+value=整节文本，04 §8.15）+ box 节 Append Template=。
   SandMan 源码未读未复制（07 §3.3 记录的行为规格 + GPL ini 键面形态）。
3. **doctor（07-P2-4）**：JS 诊断树与"无第三方依赖"冲突 → 静态体检清单
   （sbiedll/abi、driver/svc 服务+设备交叉核证、安装布局、ini 定位、
   配置锁、FileRootPath 盘符、DefaultBox、箱清单+FileRootPath 冲突+
   守护键、server 连接）。全只读；退出码 0/1（有 FAIL=1）。
4. **proc start 伪命令（07-P2-3）**：实测发现裸转发不可用——Start.exe
   直投 CreateProcess 对伪串必然 GENERIC（core start.cpp:432-438），
   伪命令必须经 Start.exe 自身解释。首命令 token 命中
   default_browser/mail_agent/run_dialog/auto_run 时改写为
   "Start.exe <pseudo>" 再走原实现。
5. **box dump/cfg dump（06 P2-9）**：原始节读=QueryConf 直投
   NO_TEMPLS|NO_EXPAND|**NO_GLOBAL**——不加 NO_GLOBAL 时多值键在本节值
   耗尽后并进 GlobalSettings 同名键值（core conf.c:1549-1556 check_global
   回退；实测 [TestOss] 的 Template= 会混入全局模板 8 行），"原始节"语义
   要求关掉。坑：驱动节枚举（Conf_Get_Setting_Name，conf.c:1379-1382）恒
   跳过 GlobalSettings——cfg dump 节清单显式补该项。输出=ini 片段形态
   （"[节名]"+Key=Value 行，可直接重定向备份/diff）；--json=
   {section,count,lines:[{key,value}…]}。
6. **快照默认标记（06 P2-3）**：Snapshots.ini [Current] Default=<id> 行级
   改写（UTF-8 无 BOM，与 SnapshotManager::SaveIniFile 同形态——
   WritePrivateProfileStringW 的 ANSI 往返会破坏非 ASCII 快照名）。

## 20. 波次 D1：trace/监控命令面（06 P2-1 收口）

数据通路三层（全部已实测）：
- **DriverApi**：`MonitorControl`（API_MONITOR_CONTROL，SbieDll 导出经
  绑定表）+ `MonitorGet2`（**API_MONITOR_GET2 无 SbieDll 包装导出**——02
  §7 坑 1，经 SbieApi_Ioctl 直投 API_MONITOR_GET2_ARGS{buffer_ptr,
  buffer_len}；rc=0x105=STATUS_MORE_ENTRIES 积压未尽、0x8000001A=空环、
  0xC00000A3=监控未开）。
- **Model/Monitor**：GET2 缓冲布局解码（[ULONG size][记录体]…以 0 结尾；
  记录体=[时间戳 8][type 4][pid 4][tid 4][若干 \0 结尾串][可选 0xFFFF 栈
  标签区]）；串段语义 [0]=name/[1]=message/[2]=subtype（对齐 QSbieAPI
  CTraceEntry）；类型缩写/状态文本（disposition/trace/user 位）；pid→box
  解析（含负缓存）；MonitorDirectSession（直连自拉：启监控→拉尽→仅关回
  自开者）。
- **server/TracePump**：常驻泵（独立于 LogPump——事件源/启停前置/订阅集
  全不同）；环形缓冲 8196 条（trace.dump 数据源）→ {"op":"trace.event"}
  帧推送 trace.watch 订阅连接（复用 Connection::EnqueuePush 推送框架）。

```
sbie trace watch [--box <名>] [--type <缩写>]   实时流；Ctrl+C 退出 0。IPC 优先 [trace.watch 订阅]
             [--pid <pid>] [--json]             （server TracePump 推送）；缺席/   +直连 MonitorDirectSession
             [--no-server 全局旗标]             被拒/--no-server → 直连自拉       降级
sbie trace dump [--last N=100] [--box] [--type] 近期条目（server 环形缓冲，     [trace.dump]
             [--pid] [--json]                   服务端过滤）；直连降级=当次拉尽
```

过滤参数两侧一致：--box（未知 box "-" 不匹配显式过滤）/--type（缩写表
apicall..debug 共 18 类+别名）/--pid。行格式 `[hh:mm:ss] <类型>[/.子类型]
[ (U)] <status> <pid> <box> <值>`；--json=NDJSON（watch 每行一对象；dump
为 {count,entries} 包络）。

### 20.4 监控类型覆盖清单

`Model::MonitorTypeName` 覆盖 core api_flags.h 全部 18 个 MONITOR_* 类型码：
ApiCall/SysCall/Pipe/Ipc/Rpc/WinClass/Drive/ComClass/RtClass/Ignore/Image/
File/Key/Socket(NETFW)/Dns/Scm/Hook/Debug(OTHER)；未知码 → "Unknown"。
disposition 位（open/closed）+ MONITOR_TRACE（trace）+ MONITOR_USER
（" (U)" 用户态来源）按条渲染；MONITOR_SYSCALL 的串段[2]（系统调用名）
并入 message（CTraceEntry 同款）。0xFFFF 栈标签区不解码（MonitorStackTrace
=y 时才有，符号化属 GUI 深水区，07-N-A-12 维持 N-A）。

坑记录：**API_MONITOR_GET2 是排空式读取**（驱动端逐条 pop）——server 泵与
直连 watch 不可同时读同会话环（条目会被瓜分）；--no-server 用于 server
停止时的直连形态。监控环按 Windows 会话隔离（写入侧 Session_Get(-1)、读取
侧=调用方会话），同会话即全集，无二次过滤。

## 21. 验收记录（波次 D1+D3：trace 命令组 + P2 收口，2026-09-27）

接手审计结论：前任因配额中断时 D1（DriverApi 监控绑定/Monitor 模块/
trace_cmd/TracePump/Dispatcher op/注册接线）与 D3（TemplateGen/gen-browser/
doctor/box_d3/proc_d3/maint_d3）**代码已全部在位**且首次构建即 0 error
（/W4/WX）；本波次接手补齐：06 P2-9 用户面（box dump/cfg dump，含两处
实测发现的坑修复，§19.2.5）、全量实测、docs 登记。

### 21.1 D1 实测（真实驱动/SbieSvc；server pid 在场 + --no-server 两形态）

1. **server 订阅路径**：`trace watch`（transport: ipc 订阅）+ TestOss 内
   `cmd /c "echo … > C:\x & reg query …"` → 151-257 条（File 19-124/Key
   28-76/SysCall），box=TestOss、pid、[hh:mm:ss] 均正确解析。
2. **过滤逐一验证**：`--type file`→32 条全 File；`--type key`→80 条全 Key；
   `--type syscall`→7 条全 SysCall；`--box TestOss --type file`→65 条全
   File；`--pid <活跃 pid>`→525 条全属该 pid（同窗未过滤流 2048 条含
   干扰进程）。`--type bogus`/`--last 0`→USAGE rc2。
3. **直连路径**：server stop 后 `--no-server trace watch
   --show-transport`→"direct (self pump via API_MONITOR_GET2)"，512 条
   （File 120/Key 76），沙箱内文件写入同步可见；退出后监控关回。
4. **trace dump**：`--last 3`/`--type key --last 2`/`--box --last 1`/
   `--pid <pid> --last 3` 全部正确；`--json`→{count,entries:[…]} 包络
   字段完整（type/type_code/status/pid/tid/box/time/name/message）。
5. **watch --json**：NDJSON 256 行逐行独立对象，字段同上（time=hh:mm:ss、
   timestamp=100ns 原值）。
6. **Ctrl+C**：handler 路径与 log watch 先例同构（SetConsoleCtrlHandler→
   原子置位→循环退出 rc0）；本测试机 bash 无法投递真实 CTRL_C_EVENT，
   按 log watch 已验收同款代码路径复核（遗留：真机交互式复验）。

### 21.2 D3 实测（节选关键输出）

1. **gen-browser 全周期**：dry-run 表格正确探测本机 Edge（App Paths 来源，
   profile=%Local AppData%…\Default）；生成 [Template_Local_Edge]（Tmpl.
   Title/Class=WebBrowser/ForceProcess=msedge.exe/OpenFilePath 五行）。
   `--install --box TestOss`→Template=Local_Edge 追加（template info
   Local_Edge 可查）；`--remove --box TestOss`→摘除+节删除（template
   info→NOT_FOUND；box dump 复核 ini 节恢复原样）。
2. **box create 高级旗标**：`--location D:\sbie_oss_t --temp --v2-delete
   --drop-admin`→FileRootPath=D:\sbie_oss_t\T_d3new + AutoDelete/AutoRemove/
   UseFileDeleteV2/UseRegDeleteV2/DropAdminRights=y（box dump 逐键核对）；
   info 派生列 auto_delete=yes/empty=yes/initialized=no。测毕 box delete。
3. **box list --type standard**→5 箱；`--type hardening`→no boxes。
4. **snapshot default**：take→list（current=1）→`default TestOss 1`→
   DEFAULT=yes→`--clear`→空。测毕 snapshot remove。
5. **proc suspend-box/resume-box**：suspend 后 proc info suspended=yes、
   resume 后 no；`--all`→"4 process(es) in 11 box(es)"。
6. **proc info D3**：cmd.exe→image_type=unspecified（'gpit' 返回 0 属实）、
   flags_decoded=-（flags=0）、elevated=0、wow64=0；IPC 路径本地补列同构。
7. **proc exempt**：get→off；on→"internet exemption: on"；get→on；off→off。
8. **cfg whoami**：administrator/UserSettings_4BC00582/admin=yes/
   section_exists=yes。
9. **box dump/cfg dump**：box dump TestOss 与 ini 文件节**逐字节一致**
   （diff 验证）；cfg dump 节清单含 GlobalSettings（3 键）+ 各节键数；
   cfg dump GlobalSettings/用户节/缺节 NOT_FOUND rc5；--json 形态核对。
10. **doctor**：13 项全 ok（sbiedll/abi/driver/svc/布局/ini/boxes…），rc 0；
    --json→{checks:[…13],summary:{fail:0,warn:0}}。
11. **maint 安全路径**：install --image 缺失文件→failed rc5；--bogus→
    USAGE rc2。装卸往返（SbieSvc）前任已实测，SbieDrv 装卸维持不实测
    （共享实测机风险，见 maint_d3.cpp 头注）。
12. **box explore**：explorer 打开 FileRoot 成功，COM 关闭窗口复原。
13. **proc start 伪命令**：`default_browser`→"rewritten to 'Start.exe
    default_browser'"→msedge.exe 于 TestOss 启动（proc list 核对），测毕
    kill-all。

### 21.3 P2 清单处置全表（docs/06 P2×14 + docs/07 P2×5）

| 项 | 处置 | 落点/理由 |
|---|---|---|
| 06 P2-1 trace | **已实现**（波 D1） | §20；trace watch/dump 双路径 |
| 06 P2-2 模板应用检测（scan） | **后续** | CSbieTemplates::RunCheck 为逐模板检测器集（注册表/文件/COM/服务四类，量级≈全部 Templates.ini 条目）；template check 已列生效集、doctor 覆盖环境健康。做=中规模独立波次 |
| 06 P2-3 快照默认标记 | **已实现**（波 D3） | box snapshot default（§19.2.6） |
| 06 P2-4 空置/初始化状态 | **已实现**（波 D3） | box info empty/initialized（07-P2-2 组合） |
| 06 P2-5 proc info 派生列 | **已实现**（波 D3） | flags_decoded/image_type/elevated/wow64 |
| 06 P2-6 箱级挂起/恢复 | **已实现**（波 D3） | proc suspend-box/resume-box（含 --all 全局形） |
| 06 P2-7 组件装卸 | **已实现**（波 D3） | maint install/uninstall（KmdUtil） |
| 06 P2-8 外壳集成/快捷方式 | **不做** | 宿主 GUI 集成域（资源管理器右键注册表+.lnk）；脚本场景价值低，cfg set+宿主工具可等价组合；无 server/自动化编排需求 |
| 06 P2-9 节全量导出 | **已实现**（波 D3 用户面） | box dump/cfg dump（§19.2.5，含 NO_GLOBAL/GlobalSettings 两坑修复）；节整读/整写子能力波 B 已落（box copy） |
| 06 P2-10 interactive 人工决策 | **后续** | 需 iq.ask/iq.answer 请求-应答 op+client 等待窗；当前 server 自动拒绝+log watch 事件呈现已可用（04 §12.4-6）；协议面变更留独立波次 |
| 06 P2-11 cfg set --drv-cache | **不做** | 边缘场景（改驱动缓存不落盘，重启即失）；UpdateConf 的 0x1811-14 编码序专为 SbieSvc 内部路径设计，暴露为脚本能力误用风险>价值 |
| 06 P2-12 cfg whoami | **已实现**（波 D3） | IniGetUser 三元组+节存在性 |
| 06 P2-13 box explore | **已实现**（波 D3） | ShellExecute 宿主 explorer |
| 06 P2-14 proc exempt | **已实现**（波 D3） | ioctl 直投 API_PROCESS_EXEMPTION_CONTROL |
| 07-P2-1 USB 接管 | **已实现**（波 D2） | docs/04 §18 |
| 07-P2-2 箱类型派生 | **已实现**（波 D3） | box info type + list --type |
| 07-P2-3 伪命令验收 | **已实现**（波 D3） | proc start 路由（§19.2.4，实测修复） |
| 07-P2-4 doctor | **已实现**（精简版） | 静态体检（JS 诊断树与无依赖约束冲突，§19.2.3） |
| 07-P2-5 浏览器模板 | **已实现**（波 D3） | template gen-browser（§19.2.2） |

### 21.4 遗留

1. **trace 栈符号化**（MonitorStackTrace=y 的 0xFFFF 标签区）维持 N-A
   （07-N-A-12，GUI 调试深水区）。
2. **Ctrl+C 真实交互复验**：bash 环境无法投递 CTRL_C_EVENT，按 log watch
   同款 handler 复核通过（§21.1.6）。
3. **SbieDrv 装卸实测**：维持前任决策不做（共享实测机驱动级注册删除风险
   不可控；SbieSvc 往返已实测）。
4. **proc info 的 flags 基值**：SbieSvc ProcInfo 路径对普通 cmd.exe 回
   flags=0（'proc' 位面与 SBIE_FLAG_VALID_PROCESS 的取值口径差异，
   02 §7 坑 4 同族含糊）——位名解码器本身正确（有位即列），基础值来源
   维持现状。
5. **06 P2-2/P2-10**：见 §21.3 处置理由（后续独立波次）。

---

## 22. 验收记录（波次 E：第三轮审计 08 微件收口，2026-09-27）

接手审计结论：前任因配额中断时两大 P1（08-P1-1 排除语义、08-P1-2 数组变体
绑定）与 P2-4/5/6 + tpl 三连的**代码主体已全部在位**（client/server 两路径、
IPC 参数、注册接线、文档回填），仅 Tmpl.Hide（08-P2-7）未动、Dispatcher 注册
表有一处中断挤压行（两条 reg 语句并作一行，语义无损）。本波次接手：修挤压行、
补 08-P2-7、跑通构建、全量实测、环境清理与基线还原、docs/08 状态列回填。

### 22.0 改动面（波次 E 累计，含前任在位部分）

| 文件 | 内容 |
|---|---|
| SbieCore\DriverApi\DriverApi.h/.cpp | +1 绑定 `SbieDll_FormatMessage`（数组变体，08-P1-2） |
| SbieCore\Model\Snapshots.h/.cpp | +`SnapshotManager::SetDefault(id)`（additive；[Current] Default 整文件 Load→改→Save，UTF-8 无 BOM） |
| sbie-cli\cli\Cli.cpp/.h | 组级 --help（PrintGroupUsage + --help 位置敏感分派，08-P2-6）；顶层 usage 增 kill-all 排除句 |
| sbie-cli\cli\Commands\proc_cmd.cpp | kill-all --all 排除 + --no-exceptions + skipped 计数（08-P1-1 client 侧） |
| sbie-cli\cli\Commands\proc_d3.cpp | suspend-box/resume-box IPC 化（08-P2-4 client 侧） |
| sbie-cli\cli\Commands\box_d3.cpp | snapshot default IPC 化（08-P2-5 client 侧）；行级改写助手并入 Model |
| sbie-cli\cli\Commands\log_cmd.cpp | FormatText 改数组变体（08-P1-2 client 侧） |
| sbie-cli\cli\Commands\template_cmd.cpp | `--all` 旗标 + 直连路径 Tmpl.Hide 过滤（08-P2-7） |
| sbie-cli\ipcc\SbieIpc.h | +kOpProcSuspendBox/ResumeBox、kOpBoxSnapDefault |
| sbie-cli\ipcc\TmplHide.h/.cpp | （新增，波 E 接手部分）Tmpl.Hide=y 模板名集合助手（Templates.ini + Sandboxie.ini 本地节；client 与 server 共用） |
| sbie-cli\server\Dispatcher.cpp | HProcKillAll 排除 + no_exceptions；HProcSuspendBox/ResumeBox；HBoxSnapDefault；HTplList/Info/Check 真实现（Stub 退役）；tpl.list all 参数 |
| sbie-cli\server\LogPump.cpp | FormatEntryText 改数组变体（08-P1-2 server 侧） |
| docs\04、docs\08 | 本节 + §4.2/4.3/4.6 规格行更新 + 08 状态列回填 |

构建：`cmd //c build_oss.bat` 0 error（/W4/WX），产物已并入
`Installer\SbiePlus_x64\sbie-cli.exe`。

### 22.1 08-P1-1：kill-all --all 排除语义（真实驱动 + SbieSvc）

前置：TestOssA 设 `ExcludeFromTerminateAll=y`（cfg set），TestOssA/B 各起
`cmd /c "ping -n 600 …"`（A 3 进程：cmd+RpcSs+DcomLaunch；B 4 进程）。

1. **IPC 全局形**：`proc kill-all --all`（transport: ipc）→
   `4 process(es) terminated (12 box(es)), 1 box(es) skipped
   (ExcludeFromTerminateAll)`；proc list 复核 A 的 3 进程**存活**、B 清空。
2. **直连全局形**：`--no-server` 同样跳过（client 侧 ExcludedFromTerminateAll
   助手，ConfigStore Get noExpand/noTemplates 只读 box 节自身）。
3. **JSON 信封**：`{"count":0,"boxes":12,"skipped":1,"message":…}`——
   skipped 字段仅全局形输出。
4. **--no-exceptions 逃生**：A 补起进程后 `kill-all --all --no-exceptions` →
   `3 process(es) terminated (13 box(es))`（无 skip 句），A 清空。
5. **单箱形不查键**（语义决策，对齐 QSbieAPI 单箱 TerminateAll
   SbieAPI.cpp:1764-1777）：`kill-all TestOssA` → `3 process(es) terminated`。
6. **USAGE 守卫**：`kill-all <box> --no-exceptions` → "--no-exceptions only
   applies to --all"；`--all` 带 box → "--all takes no box argument"；裸
   kill-all → usage（rc 2）。

### 22.2 08-P1-2：SbieDll_FormatMessage 数组变体

1. **绑定**：GetProcAddress 表 +1（与 0/1/2 定参变体并列）；类型
   `WCHAR* (CALLBACK*)(ULONG, const WCHAR**)`；约定 %N↔ins[N]、前 5 插入置
   ins[1..5]、空槽 nullptr（support.c:830-922 对照核实，定参变体内部即
   ins[1]=…/ins[2]=… 的包装）。
2. **调用点**：server LogPump.cpp FormatEntryText 与 client log_cmd.cpp
   FormatText 同构改数组（6 槽）；"%0" 类消息/无表项回退插入串直拼不变
   （1399 实测仍走回退——按设计）。
3. **导出级验证**（临时 harness，不入仓）：直调安装版 SbieDll.dll 导出，
   SBIE1317（`Blocked '%2' … '%3'`，popup/inf 全码 0x41020525）传入
   INS-A/INS-B → `SBIE1317 Blocked 'INS-A' from trying to access sandbox
   file root 'INS-B'`——**%2+%3 双插入完整渲染**；SBIE1101（evt/inf）单插入
   正常。
4. **实测修正（登记进 08 §2.2 回填）**：编译入 SbieMsg.dll 的 SBIE 1xxx/2xxx
   文案均 ≤2 插入——审计所引 %4 文案（"Failed to record…" 3311、"箱容量"
   等）属 3xxx txt 家族，未编译入 MSGTAB、不流经驱动日志队列。故本项性质
   为**契约补全**：1399 记录实携 ~6 插入（dump 的直拼回退可见），遇 %4+
   文案旧定参代码确会丢失第 3+ 串，数组变体全覆盖。
5. **活日志回归**：SandMan 退出窗内 server 泵（LOG_PUMP=yes）与 client
   直连（自任 leader）两路径 log dump 渲染正常（1399/1242 等无回归）。

### 22.3 08-P2-4/5：suspend-box / resume-box / snapshot default IPC 化

1. `proc suspend-box TestOssB`（transport: ipc）→ JSON `{box,boxes,
   suspended:true,count:3,message}`；proc info 复核 suspended=yes；
   resume-box → suspended=no。全局形 `--all` → "3 process(es) in 13 box(es)"
   （ipc）。直连降级 `--no-server` → "direct (SbieSvc SuspendResumeAll)"
   正常。server 侧 SuspendResumeAll 一律经 SvcCall 专职线程（§22.4 坑注）。
2. `box snapshot default T_snap`（读，ipc）→ `{box,current:"2",default:""}`；
   `default T_snap 1`（写，ipc）→ DEFAULT 列翻 yes、message 回显；`--clear`
   → 空回；直连降级两形态同构；`default T_snap 99` → NOT_FOUND rc5。
   写路径统一走 Model SnapshotManager::SetDefault（server/client 共用一份
   UTF-8 无 BOM 实现）。

### 22.4 08-P2-6 + tpl 三连 + 08-P2-7

1. **组级 --help**：`box --help` → 19 子命令清单（注册表字典序）+ notes
   （snapshot/recover 第三级）；`proc --help` → 10 子命令；template 组注
   Tmpl.Hide/--all 与 gen-browser 旗标；未注册组（`bogus --help`）退顶层
   用法；组名前 `--help` 仍为顶层。
2. **tpl 三连 transport**：list/info/check 全部 `transport: ipc`（Stub
   退役；此前 check 为 stub 降级 direct）。`template info OpenWinInetCache`
   ipc/direct 两路径同构；`template check DefaultBox` → config 来源行正常
   （--json 行含 exists 字段）。
3. **Tmpl.Hide**：`template list` 默认 **435** 行、`--all` **436** 行——
   差 1 = ScreenReader（Misc 类，Templates.ini 唯一 Tmpl.Hide=y 条目，
   实读 3943 行核对）；`--class WebBrowser` 组合过滤正常。server 侧
   tpl.list 增 `all` 参数（Sandboxie.ini 定位路径在 SvcCall 内取得——
   worker 线程不得自行触 SbieSvc LPC，03 §1 线程亲和）；client 直连路径
   自行定位（client 进程语境安全）。

### 22.5 回归 + 环境清理 + 基线还原

1. **回归**：status/version/box list/box info/cfg get/set/unset/proc
   list/start/kill/kill-all/force status/template list/info/check/log
   dump/server status/start/stop 全通过；JSON 信封抽查合规（§7.2）。
2. **环境清理**：TestOss、T_loc、T_tmp、T_fl、T_mix、T_snap、T_sb、T_d3fl、
   T_d3loc、TestOssA、TestOssB 共 11 箱 `box delete --files` 全净（D:\sbie_oss_t
   随 T_loc/T_d3loc 删除消失）；C:\Sandbox\Administrator 下空遗留目录 TestWA
   （无 ini 引用）一并清除；UserSettings BoxGrouping 行回写为
   `:DefaultBox,New_Box`；GlobalSettings 两笔测试杂散键（TestOss=KeyTrace *、
   TestOssA=ExcludeFromTerminateAll y，见 §22.6 注记）cfg unset 清除。
   终态 `box list` 仅 **DefaultBox + New_Box**。
3. **基线还原**：测试窗内曾 taskkill SandMan（换取会话 leader 做活日志
   路径验证）；终态按序还原——server stop → SandMan 重启（-autorun，自任
   leader）→ server start → `server status` = RUNNING / LOG_PUMP=no /
   GUARDIANS=yes，与波前一致。

### 22.6 注记与坑

1. **SbieSvc 线程亲和（复确认）**：server worker 线程直调 SvcClient LPC
   会挂起——本波 HTplList/HTplCheck/HProcSuspendBoxImpl 全部 SvcCall 包裹
   （03 §1 / §12 实测坑的再次应用）。
2. **cfg set 杂散键异常（观察，未定位根因）**：测试窗内两笔
   `cfg set <K> <V> --section <Box>` 在盘上 [GlobalSettings] 留下
   `<Box>=<K> <V>` 形杂散键（波次 D3 亦留有同形一笔 TestOss=KeyTrace *），
   且随后该 box 节内的正确键被移除（表现为 cfg unset NOT_FOUND）——疑似
   SbieSvc 写路径在特定时序（server 重启窗/SbieSvc 双实例在跑）下的错位
   写。**受控复测干净**：`cfg set ProbeKey z1 --section TestOssB` 落节
   正确、无杂散、unset 干净；未再复现。登记备查，不阻塞本波（涉及
   SbieSvc 双实例：实测机 tasklist 见两个 SbieSvc.exe——控制台实例为
   SandMan 会话的代理进程，非双服务）。
3. **MSVC 编译中文注释坑（工具链）**：临时 harness 源文件含 UTF-8 中文
   注释时 cl（系统 GBK 代码页）会错位解析导致伪错误——ASCII-only 后消失。
   本仓源文件均为 UTF-8 带 BOM 或 ASCII，不受影响（build_oss.bat 全程
   0 error 佐证）。
4. **server 自动重生**：server stop 后任意 client 命令会自动拉起新 server
   （幂等设计）；终态以"SandMan 先启动持 leader、server 后启动
   LOG_PUMP=no"还原波前形态。

### 22.7 遗留

1. 08-P2-1（box rules）/ 08-P2-2（template folder 面）——波次 F（观测增强）。
2. SBIE_INI_TEMPLATE（0x1806）不绑定：判定处置见 docs\08 §2.3 回填
   （通用 cfg 直写覆盖键等价；folder 面实现时再评估）。
3. 06 P2-2 scan（275 检测器）/ 06 P2-10 iq 协议——独立波次（08 §5.2）。
4. §22.6.2 杂散键异常根因未定位（受控不复现）——后续波次若再现优先查
   SbieSvc SET_SETTING 的 section/value 装配路径。
