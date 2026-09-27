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
// MainWindow：NavigationView 外壳 + 页面路由。全局状态在 MainWindow.Vm，
// 各页面经 Frame 缓存实例共享；启动流程 = 首页导航 + 初次刷新 + 启动轮询。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using sbie_gui.Pages;
using sbie_gui.Services;
using sbie_gui.ViewModels;

namespace sbie_gui;

public sealed partial class MainWindow : Window
{
    public static MainWindow? Current { get; private set; }

    public AppViewModel Vm { get; }

    private static readonly Dictionary<string, Type> PageMap = new()
    {
        ["processes"] = typeof(ProcessPage),
        ["boxes"] = typeof(SandboxListPage),
        ["boxsettings"] = typeof(BoxSettingsPage),
        ["globalsettings"] = typeof(GlobalSettingsPage),
    };

    public MainWindow(SandboxService svc)
    {
        Current = this;
        Vm = new AppViewModel(svc);
        App.LogStage("mw: vm ready");
        InitializeComponent();
        App.LogStage("mw: InitializeComponent done");

        // Win11 标准标题栏
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitle);

        Closed += (_, _) => { Vm.StopPolling(); Current = null; };
        ContentFrame.Navigated += OnFrameNavigated;

        // 首页：沙盒列表
        ContentFrame.Navigate(typeof(SandboxListPage));

        // 初次数据加载 + 启动 10s 轮询（状态卡/列表/进程）
        _ = StartupAsync();
        App.LogStage("mw: ctor done");
    }

    private async Task StartupAsync()
    {
        App.LogStage("mw: startup begin");
        await Vm.RefreshAllAsync();
        Vm.StartPolling();
        App.LogStage("mw: refresh done, polling started");
    }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag })
            NavigateTo(tag);
    }

    private void OnFrameNavigated(object sender, NavigationEventArgs e)
    {
        // 代码导航（双击跳转等）后同步 NavigationView 选中态
        string? tag = e.SourcePageType switch
        {
            var t when t == typeof(ProcessPage) => "processes",
            var t when t == typeof(SandboxListPage) => "boxes",
            var t when t == typeof(BoxSettingsPage) => "boxsettings",
            var t when t == typeof(GlobalSettingsPage) => "globalsettings",
            _ => null,
        };
        if (tag == null)
            return;
        var target = Nav.MenuItems.OfType<NavigationViewItem>().FirstOrDefault(i => (string)i.Tag == tag);
        if (target != null && !ReferenceEquals(target, Nav.SelectedItem))
            Nav.SelectedItem = target;
    }

    /// <summary>跨页导航（沙盒列表双击行 -> 进程管理页 等）。</summary>
    public void NavigateTo(string tag)
    {
        if (!PageMap.TryGetValue(tag, out var type))
            return;
        if (ContentFrame.Content?.GetType() == type)
            return;
        ContentFrame.Navigate(type);
    }
}
