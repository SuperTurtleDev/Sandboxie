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
// 全局设置页：cfg dump GlobalSettings 读、cfg set（--append）写、cfg unset 删。
// 系统状态卡由 AppViewModel 10s 轮询驱动（version/status/force/maint）。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Navigation;
using sbie_gui.Models;

namespace sbie_gui.Pages;

public sealed partial class GlobalSettingsPage : SettingsPageBase
{
    private const string Section = "GlobalSettings";

    protected override string SectionLabel => Section;

    public GlobalSettingsPage()
    {
        InitializeComponent();
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        await RefreshRowsAsync();
        _ = Vm.RefreshStatusCardAsync();
    }

    protected override Task<IReadOnlyList<SettingRow>> LoadRowsAsync() => Vm.Svc.DumpGlobalAsync();

    protected override Task SetRowAsync(string key, string value, bool append, int index) =>
        Vm.Svc.CfgSetAsync(Section, key, value, append);

    protected override Task DeleteRowAsync(string key) => Vm.Svc.CfgUnsetAsync(Section, key);

    private async void OnAddKey(object sender, RoutedEventArgs e) => await AddKeyFlowAsync(XamlRoot);
    private async void OnEditKey(object sender, RoutedEventArgs e) => await EditKeyFlowAsync(XamlRoot);
    private async void OnDeleteKey(object sender, RoutedEventArgs e) => await DeleteKeyFlowAsync(XamlRoot);

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await RefreshRowsAsync();
        await Vm.RefreshStatusCardAsync();
    }
}
