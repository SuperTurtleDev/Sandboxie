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
// 设置页共用逻辑（沙盒设置 / 全局设置）：键值表 + 新增/编辑/删除键。
// 编辑对话框对多值键提供 --index 选择或追加（box set --append / cfg set --append）。
// 删除键 = cfg unset <key> --section <section>（box 组无 unset 子命令）。
// ---------------------------------------------------------------------------

using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using sbie_gui.Models;
using sbie_gui.ViewModels;

namespace sbie_gui.Pages;

/// <summary>
/// 键值设置页基类：派生页提供 Load/Set/Delete 与节名。
/// 注意：XAML 根元素必须是 <c>local:SettingsPageBase</c>（分部类基类一致性）。
/// </summary>
public class SettingsPageBase : Page
{
    protected virtual string SectionLabel => "";
    protected virtual Task<IReadOnlyList<SettingRow>> LoadRowsAsync() =>
        Task.FromResult<IReadOnlyList<SettingRow>>(Array.Empty<SettingRow>());
    protected virtual Task SetRowAsync(string key, string value, bool append, int index) =>
        throw new InvalidOperationException();
    protected virtual Task DeleteRowAsync(string key) => throw new InvalidOperationException();

    public AppViewModel Vm { get; }
    public ObservableCollection<SettingRow> Rows { get; } = new();
    public SettingRow? SelectedRow { get; set; }

    public SettingsPageBase()
    {
        Vm = MainWindow.Current!.Vm;
    }

    protected async Task RefreshRowsAsync()
    {
        try
        {
            var rows = await LoadRowsAsync();
            Rows.Clear();
            foreach (var r in rows)
                Rows.Add(r);
        }
        catch (Exception e)
        {
            Vm.ShowError(e, "读取 " + SectionLabel + " 失败");
        }
    }

    protected void OnRowSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        SelectedRow = (sender as ListView)?.SelectedItem as SettingRow;
    }

    // ------------------------------------------------------------------ 新增键

    protected async Task AddKeyFlowAsync(XamlRoot xamlRoot)
    {
        if (Vm.IsBusy)
            return;
        var keyBox = new TextBox { Header = "设置名（key）", PlaceholderText = "OpenFilePath", MinWidth = 400 };
        var valueBox = new TextBox { Header = "值（value）", PlaceholderText = @"C:\path\to\dir\", MinWidth = 400 };
        var appendChk = new CheckBox { Content = "追加为新值（--append，多值键）", IsChecked = false };

        var dialog = new ContentDialog
        {
            Title = "新增设置 — " + SectionLabel,
            Content = new StackPanel { Spacing = 12, Children = { keyBox, valueBox, appendChk } },
            PrimaryButtonText = "保存",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = xamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        string key = keyBox.Text.Trim();
        if (key.Length == 0 || valueBox.Text.Length == 0)
        {
            Vm.ShowInfo("设置名与值不能为空。", InfoBarSeverity.Error);
            return;
        }

        using var _ = Vm.BeginBusy();
        try
        {
            await SetRowAsync(key, valueBox.Text, appendChk.IsChecked == true, 0);
            await RefreshRowsAsync();
            Vm.ShowInfo("已保存 " + key, InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "写入设置失败");
        }
    }

    // ------------------------------------------------------------------ 编辑键

    protected async Task EditKeyFlowAsync(XamlRoot xamlRoot)
    {
        var row = SelectedRow;
        if (row == null)
        {
            Vm.ShowInfo("请先在表格中选中一个设置。", InfoBarSeverity.Warning);
            return;
        }
        if (Vm.IsBusy)
            return;

        var valueBox = new TextBox { Header = "值（value）", Text = row.FirstValue, MinWidth = 400 };
        var keyHeader = new TextBlock
        {
            Text = row.Key,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            FontSize = 16,
        };

        // 多值键：选择替换哪个 index 或追加（--index 从 1 起）
        ComboBox? indexCombo = null;
        if (row.Values.Count > 1)
        {
            var opts = Enumerable.Range(1, row.Values.Count)
                .Select(i => "替换第 " + i + " 个值：" + Trunc(row.Values[i - 1]))
                .ToList();
            opts.Add("追加为新值（--append）");
            indexCombo = new ComboBox
            {
                Header = "多值键 — 本次写入目标",
                ItemsSource = opts,
                SelectedIndex = 0,
                HorizontalAlignment = HorizontalAlignment.Stretch,
            };
        }

        var children = new List<FrameworkElement> { keyHeader, valueBox };
        if (indexCombo != null)
            children.Add(indexCombo);
        var panel = new StackPanel { Spacing = 12 };
        foreach (var c in children)
            panel.Children.Add(c);

        var dialog = new ContentDialog
        {
            Title = "编辑设置 — " + SectionLabel,
            Content = panel,
            PrimaryButtonText = "保存",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = xamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        bool append = false;
        int index = 0;
        if (indexCombo != null)
        {
            int sel = indexCombo.SelectedIndex;
            append = sel >= row.Values.Count;
            index = append ? 0 : sel + 1;
            if (!append)
                valueBox.Text = valueBox.Text; // 保持用户输入
        }

        using var _ = Vm.BeginBusy();
        try
        {
            await SetRowAsync(row.Key, valueBox.Text, append, index);
            await RefreshRowsAsync();
            Vm.ShowInfo("已更新 " + row.Key, InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "写入设置失败");
        }
    }

    // ------------------------------------------------------------------ 删除键

    protected async Task DeleteKeyFlowAsync(XamlRoot xamlRoot)
    {
        var row = SelectedRow;
        if (row == null)
        {
            Vm.ShowInfo("请先在表格中选中一个设置。", InfoBarSeverity.Warning);
            return;
        }
        if (Vm.IsBusy)
            return;

        string multi = row.Values.Count > 1
            ? "该键有 " + row.Values.Count + " 个值，将整键删除（cfg unset）。"
            : "";
        var dialog = new ContentDialog
        {
            Title = "删除设置 " + row.Key + " ？",
            Content = new StackPanel
            {
                Spacing = 8,
                Children =
                {
                    new TextBlock { Text = "节：" + SectionLabel, FontSize = 12 },
                    new TextBlock { Text = multi, FontSize = 12, TextWrapping = TextWrapping.Wrap,
                                    Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorTertiaryBrush"] },
                },
            },
            CloseButtonText = "删除",
            PrimaryButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = xamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.None)
            return;

        using var _ = Vm.BeginBusy();
        try
        {
            await DeleteRowAsync(row.Key);
            await RefreshRowsAsync();
            Vm.ShowInfo("已删除 " + row.Key, InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            Vm.ShowError(ex, "删除设置失败");
        }
    }

    private static string Trunc(string s) => s.Length <= 40 ? s : s.Substring(0, 40) + "…";
}
