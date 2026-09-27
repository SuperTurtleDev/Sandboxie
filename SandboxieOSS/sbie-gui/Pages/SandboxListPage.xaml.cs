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
// 沙盒列表页（默认首页）：行选中 = 全局当前沙盒；新建/删除/启停/重命名；
// 双击行 -> 进程管理页。破坏性操作 Close 位（Win11 规范）。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using sbie_gui.Models;
using sbie_gui.ViewModels;

namespace sbie_gui.Pages;

public sealed partial class SandboxListPage : Page
{
    public AppViewModel Vm { get; }

    public SandboxListPage()
    {
        Vm = MainWindow.Current!.Vm;
        InitializeComponent();
        Loaded += OnFirstLoad;
    }

    private async void OnFirstLoad(object sender, RoutedEventArgs e)
    {
        Loaded -= OnFirstLoad;
        if (Vm.Boxes.Count == 0)
            await Vm.RefreshAllAsync();
    }

    // ------------------------------------------------------------------ 选择联动

    private bool _syncing;

    private void OnBoxSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing)
            return;
        var box = BoxList.SelectedItem as BoxSummary;
        if (box != null && !ReferenceEquals(box, Vm.SelectedBox))
            Vm.SelectedBox = box;
    }

    private void OnBoxDoubleTapped(object sender, Microsoft.UI.Xaml.Input.DoubleTappedRoutedEventArgs e)
    {
        MainWindow.Current?.NavigateTo("processes");
    }

    // ------------------------------------------------------------------ 新建

    private async void OnNewBox(object sender, RoutedEventArgs e) => await NewBoxFlowAsync();

    private async Task NewBoxFlowAsync()
    {
        var nameBox = new TextBox
        {
            Header = "沙盒名称（不含空格）",
            PlaceholderText = "TestBox",
            MaxLength = 32,
        };
        var types = await SafeGetTypesAsync();
        var typeCombo = new ComboBox
        {
            Header = "沙盒类型（box types 预设）",
            HorizontalAlignment = HorizontalAlignment.Stretch,
            ItemsSource = types.Select(t => t.DisplayLabel).ToList(),
            SelectedIndex = -1,
        };
        int stdIndex = types.ToList().FindIndex(t => t.Type == "standard");
        typeCombo.SelectedIndex = stdIndex >= 0 ? stdIndex : 0;

        var dialog = new ContentDialog
        {
            Title = "新建沙盒",
            Content = new StackPanel { Spacing = 12, MinWidth = 360, Children = { nameBox, typeCombo } },
            PrimaryButtonText = "创建",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        string name = nameBox.Text.Trim();
        if (!IsValidBoxName(name))
        {
            Vm.ShowInfo("名称无效：不能为空、含空格或 []/\\= 字符。", InfoBarSeverity.Error);
            return;
        }
        string? typeName = types.ElementAtOrDefault(typeCombo.SelectedIndex)?.Type;

        using var _ = Vm.BeginBusy();
        try
        {
            await Vm.Svc.CreateBoxAsync(name, typeName ?? "standard");
            await Vm.RefreshBoxesAsync();
            Vm.SelectedBox = Vm.Boxes.FirstOrDefault(b => b.Name == name);
            _syncing = true;
            BoxList.SelectedItem = Vm.SelectedBox;
            _syncing = false;
            Vm.ShowInfo("已创建沙盒 " + name);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "新建沙盒失败");
        }
    }

    private async Task<IReadOnlyList<BoxTypeOption>> SafeGetTypesAsync()
    {
        try
        {
            return await Vm.Svc.GetBoxTypesAsync();
        }
        catch
        {
            return new[] { new BoxTypeOption { Type = "standard", Description = "Standard sandbox" } };
        }
    }

    // ------------------------------------------------------------------ 启用/禁用/重命名/删除

    private async void OnEnableBox(object sender, RoutedEventArgs e) => await SetBoxEnabledFlowAsync(true);
    private async void OnCtxEnable(object sender, RoutedEventArgs e) => await SetBoxEnabledFlowAsync(true, sender);
    private async void OnDisableBox(object sender, RoutedEventArgs e) => await SetBoxEnabledFlowAsync(false);
    private async void OnCtxDisable(object sender, RoutedEventArgs e) => await SetBoxEnabledFlowAsync(false, sender);

    private async Task SetBoxEnabledFlowAsync(bool enable, object? ctx = null)
    {
        var box = BoxFromContext(ctx) ?? Vm.SelectedBox;
        if (box == null || Vm.IsBusy)
            return;
        using var _ = Vm.BeginBusy();
        try
        {
            await Vm.Svc.SetBoxEnabledAsync(box.Name, enable);
            await Vm.RefreshBoxesAsync();
            Vm.ShowInfo("沙盒 " + box.Name + (enable ? " 已启用" : " 已禁用"),
                enable ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, (enable ? "启用" : "禁用") + "失败");
        }
    }

    private async void OnRenameBox(object sender, RoutedEventArgs e) => await RenameFlowAsync();
    private async void OnCtxRename(object sender, RoutedEventArgs e) => await RenameFlowAsync(sender);

    private async Task RenameFlowAsync(object? ctx = null)
    {
        var box = BoxFromContext(ctx) ?? Vm.SelectedBox;
        if (box == null || Vm.IsBusy)
            return;

        var nameBox = new TextBox { Header = "新名称", Text = box.Name, MaxLength = 32 };
        var dialog = new ContentDialog
        {
            Title = "重命名沙盒 " + box.Name,
            Content = nameBox,
            PrimaryButtonText = "重命名",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        string newName = nameBox.Text.Trim();
        if (!IsValidBoxName(newName))
        {
            Vm.ShowInfo("名称无效：不能为空、含空格或 []/\\= 字符。", InfoBarSeverity.Error);
            return;
        }
        if (newName == box.Name)
            return;

        using var _ = Vm.BeginBusy();
        try
        {
            await Vm.Svc.RenameBoxAsync(box.Name, newName);
            await Vm.RefreshBoxesAsync();
            Vm.ShowInfo(box.Name + " 已重命名为 " + newName);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "重命名失败");
        }
    }

    private async void OnDeleteBox(object sender, RoutedEventArgs e) => await DeleteFlowAsync();
    private async void OnCtxDelete(object sender, RoutedEventArgs e) => await DeleteFlowAsync(sender);

    private async Task DeleteFlowAsync(object? ctx = null)
    {
        var box = BoxFromContext(ctx) ?? Vm.SelectedBox;
        if (box == null || Vm.IsBusy)
            return;

        var withFiles = new CheckBox { Content = "同时删除沙箱内容（FileRoot 下的全部文件）", IsChecked = false };
        var dialog = new ContentDialog
        {
            Title = "删除沙盒 " + box.Name + " ？",
            Content = new StackPanel
            {
                Spacing = 10,
                MinWidth = 380,
                Children =
                {
                    new TextBlock { Text = "该操作将把此沙盒从 Sandboxie.ini 中移除，不可撤销。", TextWrapping = TextWrapping.Wrap },
                    withFiles,
                    new TextBlock { Text = box.FileRoot, FontSize = 11, TextWrapping = TextWrapping.Wrap,
                                    Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorTertiaryBrush"] },
                },
            },
            CloseButtonText = "删除",
            PrimaryButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.None)
            return;

        using var _ = Vm.BeginBusy();
        try
        {
            await Vm.Svc.DeleteBoxAsync(box.Name, withFiles.IsChecked == true);
            await Vm.RefreshBoxesAsync();
            Vm.ShowInfo("已删除沙盒 " + box.Name + (withFiles.IsChecked == true ? "（含内容）" : ""));
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "删除失败");
        }
    }

    private void OnCtxOpenProcesses(object sender, RoutedEventArgs e)
    {
        MainWindow.Current?.NavigateTo("processes");
    }

    // ------------------------------------------------------------------ 辅助

    private static BoxSummary? BoxFromContext(object? sender) =>
        (sender as FrameworkElement)?.DataContext as BoxSummary;

    internal static bool IsValidBoxName(string name)
    {
        if (name.Length == 0 || name.Length > 64)
            return false;
        if (name.StartsWith('[') || name.EndsWith(']'))
            return false;
        foreach (char c in name)
        {
            if (char.IsWhiteSpace(c) || c is '[' or ']' or '/' or '\\' or '=')
                return false;
        }
        return true;
    }
}
