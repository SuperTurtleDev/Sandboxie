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
// App 入口。unpackaged 启动链（docs/09-gui.md §2）：
//   csproj WindowsPackageType=None -> WinAppSDK 自动引导初始化（官方 1.x 做法）
//     -> Application.Start -> App..ctor -> OnLaunched
// 显式引导等价物为 Microsoft.Windows.ApplicationModel.DynamicDependency.Bootstrap
// .Initialize(0x00010008)（随 Microsoft.WindowsAppSDK NuGet 提供）；因自动初始化
// 已覆盖 self-contained 布局，本工程不重复调用（二次调用会得到
// ERROR_INVALID_STATE）。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using sbie_gui.Services;

namespace sbie_gui;

public partial class App : Application
{
    public static MainWindow? MainWin { get; private set; }

    /// <summary>崩溃/异常日志（诊断 GUI 启动失败；正常路径也记阶段点）。</summary>
    public static string CrashLogPath => Path.Combine(Path.GetTempPath(), "sbie-gui-crash.log");

    public static void LogStage(string stage)
    {
        try
        {
            File.AppendAllText(CrashLogPath,
                DateTime.Now.ToString("HH:mm:ss.fff") + " " + stage + Environment.NewLine);
        }
        catch
        {
            // 日志失败不影响运行
        }
    }

    public App()
    {
        InitializeComponent();
        UnhandledException += OnUnhandledException;
    }

    private void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        try
        {
            File.AppendAllText(CrashLogPath,
                DateTime.Now.ToString("HH:mm:ss.fff") + " UNHANDLED (" + e.Exception.HResult.ToString("X8") + "): " +
                e.Exception + Environment.NewLine);
        }
        catch
        {
            // 忽略
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        string[] cmdline = Environment.GetCommandLineArgs();

        // 无 UI 自测模式：在无 SynchronizationContext 的独立线程上同步跑完，
        // 避免阻塞 UI 线程与 CliBridge 异步续体死锁。
        if (cmdline.Any(a => a.Equals("--selftest", StringComparison.OrdinalIgnoreCase)))
        {
            int exitCode = 2;
            var thread = new Thread(() => { exitCode = SelfTest.Run(cmdline); });
            thread.Start();
            thread.Join();
            Environment.ExitCode = exitCode;
            Environment.Exit(exitCode);
        }

        LogStage("launched normal mode");
        var service = new SandboxService(CliBridge.Create());
        LogStage("service ready: " + service.CliPath);
        MainWin = new MainWindow(service);
        LogStage("mainwindow ctor done");
        MainWin.Activate();
        LogStage("activated");
    }
}
