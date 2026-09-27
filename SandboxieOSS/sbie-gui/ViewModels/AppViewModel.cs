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
// AppViewModel：四页共享的全局状态（taskmgr 式导航，docs/09-gui.md §4）：
//   - 当前选中沙盒（跨页保持，重命名/删除联动）
//   - 沙盒列表 / 当前列表 / 进程列表数据
//   - 全局设置页状态卡（version/status/force/maint，10s 轮询）
//   - InfoBar 通知（页顶浮层，自动消失）
// MVVM 轻实现（零外部依赖）；对话框编排留在各 Page 代码后置。
// ---------------------------------------------------------------------------

using System.Collections.ObjectModel;
using System.Windows.Input;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml;
using sbie_gui.Models;
using sbie_gui.Services;

namespace sbie_gui.ViewModels;

public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    protected void Raise([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

public sealed class RelayCommand : ICommand
{
    private readonly Action<object?> _execute;
    private readonly Func<object?, bool>? _canExecute;

    public RelayCommand(Action<object?> execute, Func<object?, bool>? canExecute = null)
    {
        _execute = execute;
        _canExecute = canExecute;
    }

    public event EventHandler? CanExecuteChanged;

    public bool CanExecute(object? parameter) => _canExecute?.Invoke(parameter) ?? true;
    public void Execute(object? parameter) => _execute(parameter);

    public void RaiseCanExecuteChanged() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}

public sealed class AppViewModel : ObservableObject
{
    private const int PollSeconds = 10;

    public SandboxService Svc { get; }

    // ------------------------------------------------------------ 集合与全局选中

    public ObservableCollection<BoxSummary> Boxes { get; } = new();

    public ObservableCollection<ProcEntry> Procs { get; } = new();

    private BoxSummary? _selectedBox;
    /// <summary>全局"当前沙盒"：沙盒列表页选择 / 进程页 ComboBox 切换，跨页共享。</summary>
    public BoxSummary? SelectedBox
    {
        get => _selectedBox;
        set
        {
            if (SetProperty(ref _selectedBox, value))
            {
                Raise(nameof(SelectedBoxName));
                Raise(nameof(ProcPanelTitle));
                _ = RefreshProcsForBoxAsync();
            }
        }
    }

    public string SelectedBoxName => SelectedBox?.Name ?? "（未选择沙盒）";

    public string ProcPanelTitle =>
        SelectedBox == null ? "未选择沙盒" : SelectedBox.Name;

    private ProcEntry? _selectedProc;
    public ProcEntry? SelectedProc
    {
        get => _selectedProc;
        set => SetProperty(ref _selectedProc, value);
    }

    // ------------------------------------------------------------ 状态卡（全局设置页）

    private string _versionLine = "…";
    public string VersionLine { get => _versionLine; private set => SetProperty(ref _versionLine, value); }

    private string _statusLine = "…";
    public string StatusLine { get => _statusLine; private set => SetProperty(ref _statusLine, value); }

    private string _forceLine = "…";
    public string ForceLine { get => _forceLine; private set => SetProperty(ref _forceLine, value); }

    private string _maintLine = "…";
    public string MaintLine { get => _maintLine; private set => SetProperty(ref _maintLine, value); }

    // ------------------------------------------------------------ InfoBar 通知

    private string _infoMessage = "";
    public string InfoMessage { get => _infoMessage; private set => SetProperty(ref _infoMessage, value); }

    private InfoBarSeverity _infoSeverity = InfoBarSeverity.Informational;
    public InfoBarSeverity InfoSeverity { get => _infoSeverity; private set => SetProperty(ref _infoSeverity, value); }

    private bool _infoIsOpen;
    public bool InfoIsOpen { get => _infoIsOpen; set => SetProperty(ref _infoIsOpen, value); }

    private readonly DispatcherTimer? _infoTimer;

    // ------------------------------------------------------------ 忙标记

    private bool _isBusy;
    public bool IsBusy
    {
        get => _isBusy;
        private set => SetProperty(ref _isBusy, value);
    }

    public RelayCommand RefreshCommand { get; }

    private readonly SemaphoreSlim _refreshLock = new(1, 1);
    private readonly DispatcherTimer _pollTimer;

    public AppViewModel(SandboxService svc)
    {
        Svc = svc;
        RefreshCommand = new RelayCommand(_ => _ = RefreshAllAsync(), _ => !IsBusy);

        _infoTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _infoTimer.Tick += (_, _) => { _infoTimer.Stop(); InfoIsOpen = false; };

        _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(PollSeconds) };
        _pollTimer.Tick += async (_, _) => await PollTickAsync();
    }

    public void StartPolling() => _pollTimer.Start();
    public void StopPolling() => _pollTimer.Stop();

    // ------------------------------------------------------------ 通知

    /// <summary>页顶 InfoBar 通知，5s 自动关闭。</summary>
    public void ShowInfo(string message, InfoBarSeverity severity = InfoBarSeverity.Informational)
    {
        InfoMessage = message;
        InfoSeverity = severity;
        InfoIsOpen = true;
        _infoTimer?.Start();
    }

    /// <summary>CLI/系统异常 → 通知（不崩溃）。</summary>
    public void ShowError(Exception ex, string context)
    {
        string body = ex is CliException cli
            ? context + "：CLI 错误（code " + cli.Code + "）" + cli.Message
            : context + "：" + ex.Message;
        ShowInfo(body, InfoBarSeverity.Error);
    }

    // ------------------------------------------------------------ 刷新

    private async Task PollTickAsync()
    {
        if (IsBusy)
            return;
        await RefreshAllAsync();
    }

    public async Task RefreshAllAsync()
    {
        if (!await _refreshLock.WaitAsync(0))
            return; // 上一轮未完成，跳过
        try
        {
            await RefreshBoxesAsync();
            await RefreshProcsForBoxAsync();
            await RefreshStatusCardAsync();
        }
        finally
        {
            _refreshLock.Release();
        }
    }

    public async Task RefreshBoxesAsync()
    {
        try
        {
            string? keep = SelectedBox?.Name;
            var boxes = await Svc.ListBoxesAsync();

            Boxes.Clear();
            foreach (var b in boxes.OrderBy(b => b.Name, StringComparer.OrdinalIgnoreCase))
                Boxes.Add(b);

            if (keep != null && Boxes.All(b => b.Name != keep))
                keep = Boxes.FirstOrDefault()?.Name; // 被删除/重命名 -> 回退首项
            SelectedBox = keep == null ? null : Boxes.FirstOrDefault(b => b.Name == keep);
        }
        catch (Exception e)
        {
            ShowError(e, "刷新沙盒列表失败");
        }
    }

    /// <summary>当前沙盒的进程列表（供进程页）。</summary>
    public async Task RefreshProcsForBoxAsync()
    {
        try
        {
            string? box = SelectedBox?.Name;
            if (box == null)
            {
                Procs.Clear();
                return;
            }
            var procs = await Svc.ListProcsAsync(box);
            Procs.Clear();
            foreach (var p in procs)
                Procs.Add(p);
        }
        catch (Exception e)
        {
            ShowError(e, "获取进程列表失败");
        }
    }

    /// <summary>状态卡四行：version/status/force/maint（10s 轮询）。</summary>
    public async Task RefreshStatusCardAsync()
    {
        try
        {
            var v = await Svc.GetVersionsAsync();
            VersionLine = "sbie-cli v" + v.CliVersion + "  |  驱动 v" + v.DriverVersion +
                          "  |  服务 " + (v.ServiceConnected ? "已连接" : "未连接");
        }
        catch (Exception e)
        {
            VersionLine = "版本获取失败：" + e.Message;
        }

        try
        {
            var st = await Svc.GetStatusAsync();
            StatusLine = string.Format(
                "驱动 {0} {1}  |  服务 {2}  |  server {3}  |  沙盒 {4}",
                st.DriverAlive ? "✓" : "✗", st.DriverVersion,
                st.ServiceConnected ? "✓" : "✗",
                st.ServerRunning ? "✓ (pid " + st.ServerPid + ")" : "✗",
                st.BoxCount);
        }
        catch (Exception e)
        {
            StatusLine = "状态获取失败：" + e.Message;
        }

        try
        {
            var f = await Svc.GetForceStatusAsync();
            ForceLine = f.Disabled ? "强制沙盒：已禁用" : "强制沙盒：正常";
        }
        catch (Exception e)
        {
            ForceLine = "force 状态失败：" + e.Message;
        }

        try
        {
            var m = await Svc.GetMaintStatusAsync();
            MaintLine = m.Count == 0 ? "maint：无数据" : string.Join("  |  ", m.Select(c => c.Summary));
        }
        catch (Exception e)
        {
            MaintLine = "maint 状态失败：" + e.Message;
        }
    }

    // ------------------------------------------------------------ 忙锁

    /// <summary>操作级忙锁（using var _ = vm.BeginBusy();）。重入直接 no-op。</summary>
    public IDisposable BeginBusy()
    {
        if (IsBusy)
            return NoopDisposable.Instance;
        IsBusy = true;
        return new BusyScope(this);
    }

    private sealed class NoopDisposable : IDisposable
    {
        public static readonly NoopDisposable Instance = new();
        public void Dispose() { }
    }

    private sealed class BusyScope(AppViewModel vm) : IDisposable
    {
        public void Dispose()
        {
            vm.IsBusy = false;
            vm.RefreshCommand.RaiseCanExecuteChanged();
        }
    }
}
