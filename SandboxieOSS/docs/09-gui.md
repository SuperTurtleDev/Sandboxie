# 09 — sbie-gui（WinUI 3 管理界面，第一波）

状态：已交付（第一波 + 两轮 UI 整改）。GPLv3；独立实现，未复制 SandMan/Sandboxie-Plus 代码
（行为参考 04-modules.md 命令契约，代码自研）。

## 1. 定位与架构

sbie-gui 是 Sandboxie-OSS 的图形管理界面（C# / WinUI 3 / .NET 8 / **unpackaged**）。
**薄壳架构**：GUI 永不直接链接驱动 / SbieCore / QSbieAPI，全部功能经子进程调用
`sbie-cli.exe --json` 完成——UI 进程与核心进程隔离，CLI 崩溃/UI 崩溃互不牵连，
命令语义与退出码全部复用 CLI 已有契约（04 §7.2 信封）。

```
sbie-gui.exe (WinUI3, unpackaged, Mica)
   │  Services/CliBridge      —— 定位 sbie-cli.exe、Process 异步调用、信封解析、超时杀树
   │  Services/SandboxService —— 强类型操作（box/proc/cfg/force/maint 全组）
   │  ViewModels/AppViewModel —— 四页共享全局状态（当前沙盒/列表/进程/状态卡/InfoBar）
   ▼
sbie-cli.exe --json <group> <cmd>   （自拉起 sbie-cli server，不用 --no-server）
   ▼
SbieSvc / SbieDrv
```

目录：`SandboxieOSS/sbie-gui/`（源码）；`SandboxieOSS/build_gui.bat`（构建+发布）；
发布布局 `Installer/SbiePlus_x64/sbie-gui/`（235 文件 ≈139 MB，含 self-contained
Windows App SDK；双击 sbie-gui.exe 即用，仅依赖 .NET 8 Desktop Runtime）。

## 2. unpackaged 启动链（官方做法核实）

- csproj：`WindowsPackageType=None` + `WindowsAppSdkSelfContained=true` +
  `SelfContained=false` + `EnableMsixTooling=true`、TFM `net8.0-windows10.0.19041`、
  `Microsoft.WindowsAppSDK 1.8.260921001`（2026-09 最新 1.x 稳定版；2.x 需更新 .NET，未采用）。
- 引导：按 MS Learn《deploy-unpackaged-apps / tutorial-unpackaged-deployment》，
  1.x 官方 unpackaged 做法即 `WindowsPackageType=None` 触发的**自动引导初始化**
  （模块初始化器调 Bootstrapper API）；显式等价物是
  `Microsoft.Windows.ApplicationModel.DynamicDependency.Bootstrap.Initialize(0x00010008)`
  （.NET 包装已随主 NuGet 提供，旧独立包 `Microsoft.WindowsAppRuntime.Bootstrap` 已下线）。
  本项目采用自动初始化（self-contained 布局下必成功），不重复显式调用
  （二次调用返回 ERROR_INVALID_STATE）。App.xaml.cs 有注释存档。
- `--selftest` 模式：无 UI 自测（独立线程无同步上下文，避免 async 续体死锁），
  日志 `%TEMP%\sbie-gui-selftest.log`，退出码=失败数。
- 诊断日志 `%TEMP%\sbie-gui-crash.log`（App.UnhandledException + 启动阶段点）。

## 3. CliBridge 契约（与 04 §7.2 一一对应）

信封：`{"ok":true,"data":…}` / `{"ok":false,"error":{"code","message"}}`；
失败抛 `CliException(code,message)`（code=CLI 退出码）。全局默认超时 45 s
（proc start 90 s），超时/取消杀整棵进程树；stdout 恒 UTF-8 解码；空输出/坏 JSON
降级为带 stderr 摘要的异常——**UI 层只显示 message，不崩溃**。

定位顺序：exe 目录及各级祖先 `sbie-cli.exe` → 祖先 `Installer\SbiePlus_x64\sbie-cli.exe`
（同时覆盖发布布局与仓库开发布局）。

### 3.1 GUI 用到的 CLI 命令（契约快照，2026-09-27 实测）

| GUI 操作 | CLI | data 形态 |
| --- | --- | --- |
| 沙盒列表 | `box list` | `[{name,enabled,active_procs,file_root,…}]` |
| 新建 | `box create <n> --type <t>` | `{message}`；类型取 `box types`（6 种） |
| 启停 | `box enable/disable <n>` | `{message}` |
| 重命名 | `box rename <a> <b>` | `{message}` |
| 删除 | `box delete <n> [--files]` | `{message}` |
| 进程表 | `proc list`（服务端按 box 过滤） | `[{pid,box,image,session,started,flags}]` |
| 启动 | `proc start <box> <cmd>` | `{pid,message}` |
| 单杀 | `proc kill <pid>` | `{message}` |
| 全部结束 | `proc kill-all <box>` | `{count,boxes,skipped,message}`（skipped=ExcludeFromTerminateAll 跳过，提示给用户） |
| 沙盒设置表 | `box dump <n>` | `{section,count,lines:[{key,value}]}`（多值键重复成行，服务层合并展示） |
| 设置写入 | `box set <n> <k> <v> [--append\|--insert\|--index i]` | `{message}`（index 从 1 起） |
| 键删除 | `cfg unset <k> --section <box>` | `{message}`（**box 组无 unset**，统一走 cfg） |
| 全局设置表 | `cfg dump GlobalSettings` | 同 box dump |
| 全局写入/删 | `cfg set <k> <v> [--section s] [--append]` / `cfg unset <k>` | `{message}` |
| 状态卡 | `version` + `status` + `force status` + `maint status` | 见下 |

状态卡四行（全局设置页，10 s 轮询）：
`version`→{cli,driver,service}；`status`→{driver.alive,service.connected,server.running/pid,boxes}；
`force status`→{disabled,message}；`maint status`→[{component,installed,running,state}]。

**proc start 引号语义（关键坑）**：CLI 把 box 之后所有位置参数以空格 join 成一条
命令行交 Start.exe（proc_cmd.cpp CmdProcStart）。带空格的程序路径必须整体作为
**一个**参数并自带引号——服务层 `StartProgramAsync` 完成"程序路径补引号 + 参数原文
拼接"，再经 Windows 规则转义（`QuoteArg`：内引号 `\"`、引号前反斜杠翻倍）传入。
selftest 含带空格路径回归用例。

## 4. 布局：taskmgr 式四页导航（追加规格，2026-09-27）

左侧 `NavigationView`（Left 模式，可折叠）：进程管理 `\uE9D9`、沙盒列表 `\uE8E5`（默认首页）、
沙盒设置 `\uE713`、全局设置 `\uF8B0`。全局"当前沙盒"存于 AppViewModel（跨页保持）；
沙盒删除/重命名后 `RefreshBoxesAsync` 按名回退联动（列表选中、进程页 ComboBox、
设置页标题同时更新）。四页均 `NavigationCacheMode=Required`（Frame 缓存实例）。

| 页 | 内容 | 工具条 |
| --- | --- | --- |
| 进程管理 | 顶部"当前沙盒"ComboBox 快速切换；进程表 PID（等宽 Cascadia）/镜像/会话/启动时间 | 启动程序（路径+浏览+参数）、结束进程（选中行）、全部结束（含 skipped 提示）、刷新 |
| 沙盒列表（首页） | 表格 名称/启用✓✕/进程数/文件根路径；行选中=设全局当前沙盒；双击行→进程页 | 新建（名称+六类型）、刷新、启用、禁用、重命名、删除（确认+“同时删除内容”复选）；行右键菜单同 |
| 沙盒设置 | 标题=当前沙盒名；键值表（多值合并 `, ` 展示，等宽值列） | 新增键（含 --append 追加）、编辑选中（多值键可选替换第 i 个或追加）、删除键（cfg unset）、刷新 |
| 全局设置 | 顶部系统状态卡四行小字（version/status/force/maint，10 s 轮询）；GlobalSettings 键值表 | 新增键/编辑选中/删除键/刷新 |

taskmgr 视觉规范执行：每页=页首大标题（Subtitle 样式）+右上命令按钮组+数据表占满；
数据表细行高（ListViewItem MinHeight=32，无卡片包裹）、表头浅灰
（`LayerFillColorDefaultBrush`）、系统 hover/选中高亮、PID/值列等宽字体；
页顶 InfoBar 统一状态/错误提示（5 s 自动关闭）。

### 4.1 Win11 风格整改（两轮，前后差异要点）

第一轮（用户验收反馈）+ 第二轮（四页布局）合并落地：

| 项 | 整改前（首版） | 整改后 |
| --- | --- | --- |
| 材质 | 系统默认纯色窗口 | `Window.SystemBackdrop = MicaBackdrop`；卡片/状态条用半透明 `CardBackgroundFillColorDefaultBrush`，Mica 透出 |
| 标题栏 | 系统默认 | `ExtendsContentIntoTitleBar=true` + `SetTitleBar(AppTitle)`（48 px 行，Caption 样式） |
| 颜色 | 硬编码 `#F2F2F2`/`Gray`/绿红 | 全部 ThemeResource（`TextFillColor*`/`SystemFillColorSuccess/Critical`/`CardStroke*`…），深浅色跟随系统（未设 RequestedTheme） |
| 图标 | Symbol 枚举（曾踩 Block 不存在） | 统一 FontIcon Segoe Fluent（E710/E72C/E768/E73E/E738/E8AC/E74D/E7B8/E9D9/E8E5/E713/F8B0） |
| 对话框 | Primary=破坏性操作 | ContentDialog + Win11 破坏性按钮规范：**破坏操作在左侧 Close 位**（返回值 `ContentDialogResult.None` 才执行），取消为 Primary 默认焦点 |
| 列表行 | 默认行高卡片 | 32 px 细行、无卡片、系统选中圆角高亮 |
| 状态提示 | 底部状态栏单行文本 | 各页 InfoBar 浮层（Success/Warning/Error 分级）+ 全局设置页状态卡 |
| 布局 | 单窗口主从两栏 | NavigationView 四页 + 全局当前沙盒联动 |

未做（记录为遗留）：表头排序（规格标注"可选"）、InfoBar 之外的操作级 ProgressRing
（Busy 仅禁用按钮）。

## 5. 构建

```
SandboxieOSS\build_gui.bat
  1) dotnet restore（失败自动以 https://api.nuget.org/v3/index.json 重试）
  2) dotnet build -c Release          （0 error 0 warning）
  3) dotnet publish -r win-x64 --self-contained false -o ..\..\Installer\SbiePlus_x64\sbie-gui
```

产物即双击可运行；首次构建约 1.5 min（NuGet 还原 WinAppSDK 为主）。
注意：`.bat` 必须 CRLF（本仓库交付版已转，见 §7 坑 9）。GUI 下一波再入 dist。

## 6. 验收记录（2026-09-27，本机实跑）

### 6.1 SelfTest（`sbie-gui.exe --selftest`，即 CliBridge 的 console harness）

31/31 PASS，序列：定位 CLI→version/status→box list/types（6 种）→错误信封
（`box info NoSuchBox` → CliException code=5）→创建 TestGui(standard)→启动
cmd /c pause→proc list 含 pid→单杀→再启动→kill-all（count≥1,skipped=0）→
**带空格路径启动回归**→禁用/启用（enabled 字段翻转）→box dump→box set+--append
（多值合并 "v1, v2"）→cfg unset(section=box) 删键→cfg dump GlobalSettings→
force/maint→重命名→delete --files→消失。日志：`%TEMP%\sbie-gui-selftest.log`。
即任务书要求的"CliBridge 逻辑用单元式 harness 核对 JSON 解析"以**成品 exe 内建
模式**完成（同二进制、同代码路径，强于临时 console 工程）。

### 6.2 GUI 实跑（UI Automation 驱动真实点击，taskmgr 版四页）

浅色 18/19 PASS + 深色 18/19 PASS（同一脚本仅切系统主题，HKCU AppsUseLightTheme
0/1，验后已还原）：四页导航齐全→首页表格 DefaultBox/New_Box→新建对话框（填名+
选类型→创建→表格出现）→选中行→进程页 ComboBox 联动→启动程序对话框（填 cmd.exe
→启动→表格出现 cmd.exe 行）→全部结束（Close 位破坏按钮+确认→cmd.exe 消失，
InfoBar 显示 kill-all 结果）→沙盒设置页（标题=当前沙盒、Enabled 键）→全局设置页
（状态卡四行、Template 键）→回首页删除（勾"同时删除内容"→消失）→工具条按钮
IsKeyboardFocusable（Tab focus visual）✓。
唯一 FAIL 项"ComboBox 联动"为主脚本 UIA 取值方法问题：WinUI3 ComboBox 不外露
选中项文本子元素，改用 `SelectionPattern.GetSelection()` 单独验证 **PASS**
（`combo selected = [TestVE]`；期间还发现并修复一个真 bug：首次进进程页
ComboBox 未同步 Loaded 前的全局选中）。

### 6.3 截图核验（浅/深各 9 张，`%TEMP%\sbie-gui-{light,dark}-*.png`）

AI 视觉核验（light-1-home / dark-7-globalsettings）确认：窗口标题正确；左侧导航
四项+汉堡折叠；表格列名与行数据（DefaultBox ✓ 0、New_Box ✓ 0、TestVE ✓ 3）正确；
深色主题整体深底浅字、状态卡四行与键值表可读、无白闪/无 Win32 灰边。

### 6.4 基线还原

验收后 `box list` = DefaultBox/New_Box/TestVE（TestVE 保留原基线 3 进程
51168/29100/97844，未动）；期间误写 GlobalSettings 的 `SbSetProbe` 键已 `cfg unset`
清回 9 键基线。GUI 侧 `/c/Users/Administrator/AppData/Local/Temp/1/sbie-gui-*.ps1`
为验收脚本（临时产物，不在仓库）。

## 7. 坑记录

1. **XAML 注释不能含 `--`**：装饰性 `<!---->` 分隔线让 XamlCompiler 报 WMC9999
   （行号误导性指向首行）；注释里出现 `cfg unset --section` 同炸。改 `====`。
2. **`Icon="Block"` 不存在**：Symbol 枚举无 Block，运行时 XamlParseException
   （0xc000027b stowed exception，编译期不报）。全部改 FontIcon 字形。
3. **WinUI3 Window 无 `Resources` 属性**（WPF 直觉害人）：资源放 `Grid.Resources`。
4. **忘记 ItemsSource**：编译通过、列表恒空——UIA 树断言才发现。
5. **ContentDialog 关闭按钮返回 `ContentDialogResult.None`**（无 Close 成员）。
6. **x:Bind 反向 bool**：`IsEnabled=False` 语法不存在；用静态函数绑定
   `local:XamlHelpers.VisibleWhen(IsEnabled)`；int→string、bool→Visibility 的隐式
   转换可用。
7. **XAML 分部类基类**：页面继承基类时 XAML 根元素必须是 `<local:SettingsPageBase>`
   （否则 g.cs 生成 `: Page` 与代码后置冲突 CS0263）；基类因此去 abstract、虚方法默认实现。
8. **`box unset` 不存在**：box 组只有 get/set；删键统一 `cfg unset --section <box>`。
   顺手实测 `cfg set` 位置参数为 `<setting> <value>`（section 是旗标）——探针误写
   GlobalSettings 后已清理（坑+恢复路径都记录在 §6.4）。
9. **.bat 必须 CRLF**：LF 版被 cmd 按字节误切（`'publish' 不是内部或外部命令`）。
10. **WinUI3 UIA 盲区**：ComboBox 选中值不暴露为文本子元素（Name 空、无
    ValuePattern、展开弹层也取不到 ListItems）——用 `SelectionPattern.GetSelection()`
    读；ListView 行内容取 ListItem 首个 Text 子元素。ContentDialog 按钮与工具条
    AppBarButton 同为 ControlType.Button，靠 ClassName（`Button` vs `AppBarButton`）区分。
11. **PS 5.1 无三元/`if` 表达式**，`.ps1` 无 BOM 时中文按 GBK 读出乱码——验收脚本
    需 BOM + 显式 if。
12. **值日坑**：`Icon` 值、`Window.SystemBackdrop`（MicaBackdrop 需 WinAppSDK≥1.3）、
    `ListViewItem` 默认样式键名 `DefaultListViewItemStyle`（BasedOn 细行高样式）。

## 8. 下一波建议（接面已预留）

- **设置编辑器增强**：设置页接 `box list-setting`（列全键名）、模板（`template list/
  check/apply`）与 `box snapshot` 组；键值表加搜索框；危险键（网络/删除类）标色。
- **Trace 面板**：新页接 `trace watch --json`（流式，需 CliBridge 增 streaming 通道
  ——当前 RunRawAsync 行缓冲已可扩展）；类型/box/pid 过滤。
- **恢复面板**：`box recover list/copy/add`（含 --move 与 OnFileRecovery checker 语义），
  与"沙盒列表→右键→恢复"接面。
- **dist 集成**：sbie-gui 进 `make_dist.bat` 布局；写 `SandboxieOSS.sln`（dotnet sln）。
- 其他低垂：表头排序、`box explore/copy/export/import` 入工具条、`doctor` 一键体检
  入状态卡、图标（app.manifest/资产）、多语言资源（.resw）。
