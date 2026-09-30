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
// 数据模型：字段与 sbie-cli --json 输出一一对应（docs/04-modules.md §7.2，
// snake_case 契约）。本文件不依赖 WinUI，可被 console harness 单独编译。
// ---------------------------------------------------------------------------

namespace sbie_gui.Models;

/// <summary>box list 行：{"name","enabled","active_procs","file_root","reg_root","ipc_root"}</summary>
public sealed class BoxSummary
{
    public string Name { get; set; } = "";
    public bool IsEnabled { get; set; }
    public int ActiveProcs { get; set; }
    public string FileRoot { get; set; } = "";

    // ---- 仅供 XAML 绑定的展示属性 ----
    public string EnabledMark => IsEnabled ? "✓" : "✕";
    public string ProcsLabel => ActiveProcs + " 进程";
}

/// <summary>proc list 行：{"pid","box","image","session","started","flags"}</summary>
public sealed class ProcEntry
{
    public int Pid { get; set; }
    public string Box { get; set; } = "";
    public string Image { get; set; } = "";
    public int Session { get; set; }
    public string Started { get; set; } = "";
    public long Flags { get; set; }
}

/// <summary>status 顶层 data：driver/service/server/boxes</summary>
public sealed class RuntimeStatus
{
    public bool DriverAlive { get; set; }
    public string DriverVersion { get; set; } = "?";
    public bool ServiceConnected { get; set; }
    public string ServiceVersion { get; set; } = "?";
    public bool ServerRunning { get; set; }
    public int ServerPid { get; set; }
    public int BoxCount { get; set; }
}

/// <summary>version 顶层 data：cli/driver/service 版本</summary>
public sealed class CliVersions
{
    public string CliVersion { get; set; } = "?";
    public string DriverVersion { get; set; } = "?";
    public bool ServiceConnected { get; set; }
}

/// <summary>box types 行：{"type","description","keys"}</summary>
public sealed class BoxTypeOption
{
    public string Type { get; set; } = "";
    public string Description { get; set; } = "";

    public string DisplayLabel => Type + " — " + Description;
}

/// <summary>proc kill-all 结果：{"count","boxes","skipped","message"}（skipped = ExcludeFromTerminateAll 跳过）</summary>
public sealed class KillAllResult
{
    public int Count { get; set; }
    public int Boxes { get; set; }
    public int Skipped { get; set; }
    public string Message { get; set; } = "";
}

/// <summary>proc start 成功：{"pid","message"}</summary>
public sealed class StartResult
{
    public int Pid { get; set; }
}

/// <summary>box dump / cfg dump 行：{"key","value"}（多值键重复成多行）</summary>
public sealed class IniLine
{
    public string Key { get; set; } = "";
    public string Value { get; set; } = "";
}

/// <summary>设置页展示行：多值合并成一个键（值列表 join ", "）</summary>
public sealed class SettingRow
{
    public string Key { get; set; } = "";
    public IReadOnlyList<string> Values { get; set; } = Array.Empty<string>();
    public string Merged => string.Join(", ", Values);

    /// <summary>首值（编辑对话框默认内容）</summary>
    public string FirstValue => Values.Count > 0 ? Values[0] : "";
}

/// <summary>force status：{"disabled","message"}</summary>
public sealed class ForceStatus
{
    public bool Disabled { get; set; }
    public string Message { get; set; } = "";
}

/// <summary>maint status 行：{"component","installed","running","state"}</summary>
public sealed class MaintComponent
{
    public string Component { get; set; } = "";
    public bool Installed { get; set; }
    public bool Running { get; set; }
    public string State { get; set; } = "";

    public string Summary => Component + ": " + State + (Running ? " ✓" : " ✗");
}
