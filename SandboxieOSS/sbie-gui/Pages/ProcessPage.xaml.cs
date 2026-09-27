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
// 进程管理页：当前沙盒进程表 + 启动程序（路径+浏览）/结束进程/全部结束。
// kill-all 结果含 ExcludeFromTerminateAll 跳过数提示。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using sbie_gui.Models;
using sbie_gui.ViewModels;
using Windows.Storage.Pickers;

namespace sbie_gui.Pages;

public sealed partial class ProcessPage : Page
{
    public AppViewModel Vm { get; }

    public ProcessPage()
    {
        Vm = MainWindow.Current!.Vm;
        InitializeComponent();
        Vm.PropertyChanged += OnVmPropertyChanged;
        Loaded += async (_, _) =>
        {
            // 首次进入页面：把 ComboBox 同步到全局当前沙盒（选择发生在别的页时不触发 PropertyChanged）
            if (!ReferenceEquals(BoxCombo.SelectedItem, Vm.SelectedBox))
                BoxCombo.SelectedItem = Vm.SelectedBox;
            UpdateEnabled();
            await Vm.RefreshProcsForBoxAsync();
        };
    }

    private void OnVmPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(AppViewModel.SelectedBox) ||
            e.PropertyName == nameof(AppViewModel.Boxes))
        {
            // 全局当前沙盒变化（列表页选择/删除/重命名）-> 同步 ComboBox
            if (!ReferenceEquals(BoxCombo.SelectedItem, Vm.SelectedBox))
                BoxCombo.SelectedItem = Vm.SelectedBox;
            UpdateEnabled();
        }
        if (e.PropertyName == nameof(AppViewModel.IsBusy))
            UpdateEnabled();
    }

    private void UpdateEnabled()
    {
        bool box = Vm.SelectedBox != null;
        BtnStart.IsEnabled = box && !Vm.IsBusy;
        BtnKillAll.IsEnabled = box && !Vm.IsBusy;
        // BtnKillProc 由选中行决定
        if (ProcList.SelectedItem == null)
            BtnKillProc.IsEnabled = false;
        else
            BtnKillProc.IsEnabled = !Vm.IsBusy;
    }

    // ------------------------------------------------------------------ 切换/选择

    private async void OnBoxComboChanged(object sender, SelectionChangedEventArgs e)
    {
        if (BoxCombo.SelectedItem is BoxSummary box && !ReferenceEquals(box, Vm.SelectedBox))
            Vm.SelectedBox = box; // setter 内部会刷新进程表
        else if (Vm.SelectedBox == null)
            await Vm.RefreshProcsForBoxAsync();
    }

    private void OnProcSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        Vm.SelectedProc = ProcList.SelectedItem as ProcEntry;
        BtnKillProc.IsEnabled = Vm.SelectedProc != null && !Vm.IsBusy;
    }

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await Vm.RefreshBoxesAsync();
        await Vm.RefreshProcsForBoxAsync();
    }

    // ------------------------------------------------------------------ 启动程序

    private async void OnStartProgram(object sender, RoutedEventArgs e)
    {
        var box = Vm.SelectedBox;
        if (box == null || Vm.IsBusy)
            return;

        var programBox = new TextBox
        {
            Header = "程序（完整路径，可带空格）",
            PlaceholderText = @"C:\Windows\System32\notepad.exe",
            MinWidth = 420,
        };
        var browseBtn = new Button { Content = "浏览…" };
        var argsBox = new TextBox { Header = "参数（可选，原文追加）", PlaceholderText = "/c pause", MinWidth = 420 };
        browseBtn.Click += async (_, _) =>
        {
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
            picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker,
                WinRT.Interop.WindowNative.GetWindowHandle(MainWindow.Current!));
            var file = await picker.PickSingleFileAsync();
            if (file != null)
                programBox.Text = file.Path;
        };
        var hint = new TextBlock
        {
            Text = box.ActiveProcs == 0
                ? "提示：该沙盒当前没有进程，首个进程会初始化沙箱目录。"
                : "沙盒现有 " + box.ActiveProcs + " 个进程。",
            FontSize = 12,
            Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorTertiaryBrush"],
            TextWrapping = TextWrapping.Wrap,
        };

        var dialog = new ContentDialog
        {
            Title = "在沙盒 " + box.Name + " 中启动程序",
            Content = new StackPanel { Spacing = 10, MinWidth = 420, Children = { programBox, browseBtn, argsBox, hint } },
            PrimaryButtonText = "启动",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        string program = programBox.Text.Trim();
        if (program.Length == 0)
        {
            Vm.ShowInfo("请填写程序路径。", InfoBarSeverity.Error);
            return;
        }

        using var _ = Vm.BeginBusy();
        try
        {
            var result = await Vm.Svc.StartProgramAsync(box.Name, program, argsBox.Text);
            await Task.Delay(1200);
            await Vm.RefreshBoxesAsync();
            await Vm.RefreshProcsForBoxAsync();
            Vm.ShowInfo("已在 " + box.Name + " 中启动（pid " + result.Pid + "）", InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "启动失败");
        }
    }

    // ------------------------------------------------------------------ 结束进程 / 全部结束

    private async void OnKillProc(object sender, RoutedEventArgs e)
    {
        var proc = Vm.SelectedProc;
        var box = Vm.SelectedBox;
        if (proc == null || box == null || Vm.IsBusy)
            return;

        var dialog = new ContentDialog
        {
            Title = "结束进程 " + proc.Image + " (pid " + proc.Pid + ") ？",
            Content = new TextBlock { Text = "沙盒内该进程将被立即终止，未保存数据会丢失。", TextWrapping = TextWrapping.Wrap },
            CloseButtonText = "结束",
            PrimaryButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.None)
            return;

        using var _ = Vm.BeginBusy();
        try
        {
            await Vm.Svc.KillProcessAsync(proc.Pid);
            await Task.Delay(600);
            await Vm.RefreshBoxesAsync();
            await Vm.RefreshProcsForBoxAsync();
            Vm.ShowInfo("已结束 pid " + proc.Pid, InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "结束进程失败");
        }
    }

    private async void OnKillAll(object sender, RoutedEventArgs e)
    {
        var box = Vm.SelectedBox;
        if (box == null || Vm.IsBusy)
            return;

        var dialog = new ContentDialog
        {
            Title = "结束沙盒 " + box.Name + " 内的全部进程 ？",
            Content = new TextBlock
            {
                Text = "包括隐藏的系统服务进程。设置了 ExcludeFromTerminateAll=y 的进程会被跳过（结果中将显示跳过数）。",
                TextWrapping = TextWrapping.Wrap,
            },
            CloseButtonText = "全部结束",
            PrimaryButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.None)
            return;

        using var _ = Vm.BeginBusy();
        try
        {
            var result = await Vm.Svc.KillAllAsync(box.Name);
            await Task.Delay(800);
            await Vm.RefreshBoxesAsync();
            await Vm.RefreshProcsForBoxAsync();
            string note = result.Skipped > 0
                ? "（跳过 " + result.Skipped + " 个受 ExcludeFromTerminateAll 保护的进程）"
                : "";
            Vm.ShowInfo("kill-all " + box.Name + ": " + result.Message + note, InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "kill-all 失败");
        }
    }
}
