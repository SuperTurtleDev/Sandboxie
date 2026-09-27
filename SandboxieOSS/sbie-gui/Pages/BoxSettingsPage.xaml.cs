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
// 沙盒设置页：box dump 读、box set 写（--index/--append）、cfg unset 删。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using sbie_gui.Models;
using sbie_gui.ViewModels;

namespace sbie_gui.Pages;

public sealed partial class BoxSettingsPage : SettingsPageBase
{
    protected override string SectionLabel => Vm.SelectedBox?.Name ?? "（未选择沙盒）";

    public BoxSettingsPage()
    {
        InitializeComponent();
        Vm.PropertyChanged += OnVmBoxChanged;
    }

    private void OnVmBoxChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(AppViewModel.SelectedBox))
            _ = RefreshRowsAsync();
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        await RefreshRowsAsync();
    }

    protected override Task<IReadOnlyList<SettingRow>> LoadRowsAsync() =>
        Vm.SelectedBox == null
            ? Task.FromResult<IReadOnlyList<SettingRow>>(Array.Empty<SettingRow>())
            : Vm.Svc.DumpBoxAsync(Vm.SelectedBox.Name);

    protected override Task SetRowAsync(string key, string value, bool append, int index)
    {
        var box = Vm.SelectedBox ?? throw new InvalidOperationException("未选择沙盒");
        return append
            ? Vm.Svc.BoxSetAsync(box.Name, key, value, mode: "append")
            : Vm.Svc.BoxSetAsync(box.Name, key, value, index: index);
    }

    protected override Task DeleteRowAsync(string key)
    {
        var box = Vm.SelectedBox ?? throw new InvalidOperationException("未选择沙盒");
        // box 组无 unset 子命令 -> cfg unset --section <box>
        return Vm.Svc.CfgUnsetAsync(box.Name, key);
    }

    private async void OnAddKey(object sender, RoutedEventArgs e) => await AddKeyFlowAsync(XamlRoot);
    private async void OnEditKey(object sender, RoutedEventArgs e) => await EditKeyFlowAsync(XamlRoot);
    private async void OnDeleteKey(object sender, RoutedEventArgs e) => await DeleteKeyFlowAsync(XamlRoot);
    private async void OnRefresh(object sender, RoutedEventArgs e) => await RefreshRowsAsync();
}
