// ---------------------------------------------------------------------------
// sbie-gui - Sandboxie-OSS WinUI 3 management GUI (thin shell over sbie-cli.exe)
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SelfTest：`sbie-gui.exe --selftest` 的无 UI 端到端验收。
// 在独立线程（无 SynchronizationContext）上跑完整业务序列，结果写入
// %TEMP%\sbie-gui-selftest.log，退出码 = 失败数（0 = 全过）。
// 覆盖：CliBridge 信封/错误解析 + SandboxService 全部第一波操作 + 真实沙盒生命周期。
// ---------------------------------------------------------------------------

using System.Text;
using sbie_gui.Services;

namespace sbie_gui;

public static class SelfTest
{
    private const string Box = "TestGui";
    private const string Box2 = "TestGui2";

    private static readonly StringBuilder Log = new();
    private static int _pass;
    private static int _fail;

    public static int Run(string[] cmdlineArgs)
    {
        string logPath = Path.Combine(Path.GetTempPath(), "sbie-gui-selftest.log");
        try
        {
            RunInner(cmdlineArgs);
        }
        catch (Exception e)
        {
            Line("FATAL: " + e.GetType().Name + ": " + e.Message);
            _fail++;
        }

        string summary = "RESULT: " + _pass + " passed, " + _fail + " failed";
        Line(summary);
        File.WriteAllText(logPath, Log.ToString(), new UTF8Encoding(false));
        Console.Out.WriteLine("selftest log: " + logPath);
        Console.Out.WriteLine(summary);
        return _fail == 0 ? 0 : 1;
    }

    private static void RunInner(string[] cmdlineArgs)
    {
        Line("sbie-gui selftest " + string.Join(" ", cmdlineArgs));
        Line("start: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));

        // ---- CliBridge 定位 ----
        string? located = CliBridge.LocateCli();
        Check("LocateCli 找到 sbie-cli.exe", located != null, located ?? "(null)");
        if (located == null)
            return;

        string cliPath = cmdlineArgs.Length > 1 && File.Exists(cmdlineArgs[1]) ? cmdlineArgs[1] : located;
        var bridge = CliBridge.Create(cliPath!);
        var svc = new SandboxService(bridge);
        Line("cli: " + bridge.CliPath);

        // ---- version / status 解析 ----
        var versions = svc.GetVersionsAsync().GetAwaiter().GetResult();
        Check("version.cli 非空", versions.CliVersion.Length > 0 && versions.CliVersion != "?", versions.CliVersion);
        Check("version.driver 非空", versions.DriverVersion.Length > 0 && versions.DriverVersion != "?", versions.DriverVersion);

        var status = svc.GetStatusAsync().GetAwaiter().GetResult();
        Check("status.driver.alive", status.DriverAlive, "v" + status.DriverVersion);
        Check("status.service.connected", status.ServiceConnected, "v" + status.ServiceVersion);
        Check("status.server.running", status.ServerRunning, "pid " + status.ServerPid);
        Check("status.boxes >= 1", status.BoxCount >= 1, status.BoxCount.ToString());

        // ---- box list / types 解析 ----
        var boxes0 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("box list 非空", boxes0.Count >= 1, boxes0.Count + " box(es): " + string.Join(",", boxes0.Select(b => b.Name)));
        Check("box list 字段完整（以第一行为准）",
            boxes0.All(b => b.Name.Length > 0 && b.FileRoot.Length > 0 && b.ActiveProcs >= 0),
            "name/file_root/active_procs ok");

        var types = svc.GetBoxTypesAsync().GetAwaiter().GetResult();
        Check("box types 含 standard", types.Any(t => t.Type == "standard"),
            string.Join(",", types.Select(t => t.Type)));
        Check("box types 共 6 种", types.Count == 6, types.Count.ToString());

        // ---- 错误信封解析 ----
        bool errorPathOk = false;
        string errorDetail = "";
        try
        {
            svc.ListProcsAsync().GetAwaiter().GetResult(); // 无关调用，仅确保正常路径可用
            bridge.RunAsync(new[] { "box", "info", "NoSuchBox_sbie_gui_selftest" }).GetAwaiter().GetResult();
            errorDetail = "预期 CliException 未抛出";
        }
        catch (CliException ex)
        {
            errorPathOk = ex.Code == 5 && ex.Message.Contains("box not found");
            errorDetail = "code=" + ex.Code + " msg=" + ex.Message;
        }
        Check("错误信封 -> CliException(code=5)", errorPathOk, errorDetail);

        // ---- 生命周期：创建 -> 启停进程 -> 杀进程 -> 启用/禁用 -> 重命名 -> 删除 ----
        CleanupQuiet(svc, Box, Box2);

        svc.CreateBoxAsync(Box, "standard").GetAwaiter().GetResult();
        var boxes1 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("create 后 box list 出现 " + Box, boxes1.Any(b => b.Name == Box),
            string.Join(",", boxes1.Select(b => b.Name + (b.IsEnabled ? ":on" : ":off"))));

        var started = svc.StartProgramAsync(Box, "cmd.exe", "/c pause").GetAwaiter().GetResult();
        Check("proc start 返回 pid>0", started.Pid > 0, "pid=" + started.Pid);
        System.Threading.Thread.Sleep(1500);

        var procs1 = svc.ListProcsAsync(Box).GetAwaiter().GetResult();
        var mine = procs1.FirstOrDefault(p => p.Pid == started.Pid);
        Check("进程面板数据：proc list 含启动的 pid", mine != null,
            mine == null ? string.Join(",", procs1.Select(p => p.Pid + ":" + p.Image)) : mine.Image);
        Check("proc 字段完整", mine == null || (mine.Box == Box && mine.Session >= 0 && mine.Started.Length > 0),
            mine == null ? "-" : mine.Box + " sess=" + mine.Session + " started=" + mine.Started);

        svc.KillProcessAsync(started.Pid).GetAwaiter().GetResult();
        System.Threading.Thread.Sleep(800);
        var procs2 = svc.ListProcsAsync(Box).GetAwaiter().GetResult();
        Check("proc kill 后 pid 消失", procs2.All(p => p.Pid != started.Pid),
            string.Join(",", procs2.Select(p => p.Pid + ":" + p.Image)));

        // kill-all（cmd 挂起留给 kill-all 收尾）
        var started2 = svc.StartProgramAsync(Box, "cmd.exe", "/c pause").GetAwaiter().GetResult();
        System.Threading.Thread.Sleep(1500);
        var killAll = svc.KillAllAsync(Box).GetAwaiter().GetResult();
        Check("kill-all count>=1", killAll.Count >= 1, "count=" + killAll.Count + " skipped=" + killAll.Skipped);
        System.Threading.Thread.Sleep(1000);
        var procs3 = svc.ListProcsAsync(Box).GetAwaiter().GetResult();
        Check("kill-all 后进程面板为空", procs3.Count == 0,
            string.Join(",", procs3.Select(p => p.Pid + ":" + p.Image)));

        // 带空格路径 + 引号拼接（QuoteArg + CLI join 语义回归）
        string spaceDir = Path.Combine(Path.GetTempPath(), "SbieGui SelfTest Dir");
        string spaceCmd = Path.Combine(spaceDir, "p.cmd");
        Directory.CreateDirectory(spaceDir);
        File.WriteAllText(spaceCmd, "@pause\r\n");
        try
        {
            var spaced = svc.StartProgramAsync(Box, spaceCmd, "").GetAwaiter().GetResult();
            Check("带空格路径 proc start 成功", spaced.Pid > 0, "pid=" + spaced.Pid);
            System.Threading.Thread.Sleep(1500);
            var procsSp = svc.ListProcsAsync(Box).GetAwaiter().GetResult();
            Check("带空格路径进程已运行", procsSp.Any(p => p.Image.Equals("cmd.exe", StringComparison.OrdinalIgnoreCase)),
                string.Join(",", procsSp.Select(p => p.Image)));
            svc.KillAllAsync(Box).GetAwaiter().GetResult();
            System.Threading.Thread.Sleep(800);
        }
        finally
        {
            try { Directory.Delete(spaceDir, true); } catch { /* 忽略 */ }
        }

        // 禁用/启用
        svc.SetBoxEnabledAsync(Box, false).GetAwaiter().GetResult();
        var boxes2 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("disable 后 enabled=false", boxes2.First(b => b.Name == Box).IsEnabled == false, "ok");
        svc.SetBoxEnabledAsync(Box, true).GetAwaiter().GetResult();
        var boxes3 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("enable 后 enabled=true", boxes3.First(b => b.Name == Box).IsEnabled, "ok");

        // 设置链路：box dump / box set(+append) / cfg unset(section=box) / cfg dump / force / maint
        var dump0 = svc.DumpBoxAsync(Box).GetAwaiter().GetResult();
        Check("box dump 含 Enabled 键", dump0.Any(r => r.Key == "Enabled"),
            dump0.Count + " rows: " + string.Join(",", dump0.Select(r => r.Key).Take(6)));

        svc.BoxSetAsync(Box, "SbieGuiKey", "v1").GetAwaiter().GetResult();
        svc.BoxSetAsync(Box, "SbieGuiKey", "v2", mode: "append").GetAwaiter().GetResult();
        var dump1 = svc.DumpBoxAsync(Box).GetAwaiter().GetResult();
        var row1 = dump1.FirstOrDefault(r => r.Key == "SbieGuiKey");
        Check("box set + append 多值合并", row1 != null && row1.Values.Count == 2 && row1.Merged == "v1, v2",
            row1?.Merged ?? "(missing)");

        svc.CfgUnsetAsync(Box, "SbieGuiKey").GetAwaiter().GetResult();
        var dump2 = svc.DumpBoxAsync(Box).GetAwaiter().GetResult();
        Check("cfg unset(section=box) 删键", dump2.All(r => r.Key != "SbieGuiKey"),
            string.Join(",", dump2.Select(r => r.Key)));

        var gdump = svc.DumpGlobalAsync().GetAwaiter().GetResult();
        Check("cfg dump GlobalSettings 含 Template", gdump.Any(r => r.Key == "Template"),
            gdump.Count + " rows");

        var force = svc.GetForceStatusAsync().GetAwaiter().GetResult();
        Check("force status disabled=false", !force.Disabled, force.Message);

        var maint = svc.GetMaintStatusAsync().GetAwaiter().GetResult();
        Check("maint status >= 2 组件", maint.Count >= 2,
            string.Join(",", maint.Select(m => m.Component + ":" + m.State)));

        // 重命名
        svc.RenameBoxAsync(Box, Box2).GetAwaiter().GetResult();
        var boxes4 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("rename 后 " + Box2 + " 出现、" + Box + " 消失",
            boxes4.Any(b => b.Name == Box2) && boxes4.All(b => b.Name != Box),
            string.Join(",", boxes4.Select(b => b.Name)));

        // 删除（含内容）
        svc.DeleteBoxAsync(Box2, withFiles: true).GetAwaiter().GetResult();
        var boxes5 = svc.ListBoxesAsync().GetAwaiter().GetResult();
        Check("delete --files 后 " + Box2 + " 消失", boxes5.All(b => b.Name != Box2),
            string.Join(",", boxes5.Select(b => b.Name)));
    }

    private static void CleanupQuiet(SandboxService svc, params string[] boxes)
    {
        foreach (string b in boxes)
        {
            try { svc.KillAllAsync(b).GetAwaiter().GetResult(); } catch { /* 不存在则忽略 */ }
            try { svc.DeleteBoxAsync(b, withFiles: true).GetAwaiter().GetResult(); } catch { /* 忽略 */ }
        }
    }

    private static void Check(string name, bool ok, string detail)
    {
        if (ok) _pass++; else _fail++;
        Line((ok ? "PASS " : "FAIL ") + name + (detail.Length > 0 ? "  [" + detail + "]" : ""));
    }

    private static void Line(string s)
    {
        Log.AppendLine(s);
        Console.Out.WriteLine(s);
    }
}
