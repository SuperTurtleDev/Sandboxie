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
// SandboxService：把 sbie-cli 的命令组映射为强类型操作（docs/09-gui.md 契约表）。
// 不依赖 WinUI。
// ---------------------------------------------------------------------------

using System.Text.Json;
using sbie_gui.Models;

namespace sbie_gui.Services;

/// <summary>基础沙盒管理操作（第一波范围）。</summary>
public sealed class SandboxService
{
    private readonly CliBridge _cli;

    public SandboxService(CliBridge cli)
    {
        _cli = cli;
    }

    public string CliPath => _cli.CliPath;

    // ------------------------------------------------------------------ 只读

    /// <summary>sbie-cli --json box list</summary>
    public async Task<IReadOnlyList<BoxSummary>> ListBoxesAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "box", "list" }, ct: ct).ConfigureAwait(false);
        var result = new List<BoxSummary>();
        if (data.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement row in data.EnumerateArray())
            {
                result.Add(new BoxSummary
                {
                    Name = row.GetStringOrDefault("name", ""),
                    IsEnabled = row.GetBoolOrDefault("enabled", true),
                    ActiveProcs = row.GetIntOrDefault("active_procs", 0),
                    FileRoot = row.GetStringOrDefault("file_root", ""),
                });
            }
        }
        return result;
    }

    /// <summary>sbie-cli --json status（底部状态栏轮询）</summary>
    public async Task<RuntimeStatus> GetStatusAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "status" }, ct: ct).ConfigureAwait(false);
        var st = new RuntimeStatus();
        if (data.TryGetProperty("driver", out var drv))
        {
            st.DriverAlive = drv.GetBoolOrDefault("alive", false);
            st.DriverVersion = drv.GetStringOrDefault("version", "?");
        }
        if (data.TryGetProperty("service", out var svc))
        {
            st.ServiceConnected = svc.GetBoolOrDefault("connected", false);
            st.ServiceVersion = svc.GetStringOrDefault("version", "?");
        }
        if (data.TryGetProperty("server", out var srv))
        {
            st.ServerRunning = srv.GetBoolOrDefault("running", false);
            st.ServerPid = srv.GetIntOrDefault("pid", 0);
        }
        st.BoxCount = data.GetIntOrDefault("boxes", 0);
        return st;
    }

    /// <summary>sbie-cli --json version（启动时取一次 CLI 版本）</summary>
    public async Task<CliVersions> GetVersionsAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "version" }, ct: ct).ConfigureAwait(false);
        var v = new CliVersions();
        if (data.TryGetProperty("cli", out var cliEl))
            v.CliVersion = cliEl.GetStringOrDefault("version", "?");
        if (data.TryGetProperty("driver", out var drvEl))
            v.DriverVersion = drvEl.GetStringOrDefault("version", "?");
        if (data.TryGetProperty("service", out var svcEl))
            v.ServiceConnected = svcEl.GetBoolOrDefault("connected", false);
        return v;
    }

    /// <summary>sbie-cli --json box types（新建对话框预设下拉）</summary>
    public async Task<IReadOnlyList<BoxTypeOption>> GetBoxTypesAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "box", "types" }, ct: ct).ConfigureAwait(false);
        var result = new List<BoxTypeOption>();
        if (data.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement row in data.EnumerateArray())
            {
                result.Add(new BoxTypeOption
                {
                    Type = row.GetStringOrDefault("type", ""),
                    Description = row.GetStringOrDefault("description", ""),
                });
            }
        }
        return result;
    }

    /// <summary>sbie-cli --json proc list（box 非空时在服务层过滤）</summary>
    public async Task<IReadOnlyList<ProcEntry>> ListProcsAsync(string? box = null, CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "proc", "list" }, ct: ct).ConfigureAwait(false);
        var result = new List<ProcEntry>();
        if (data.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement row in data.EnumerateArray())
            {
                string rowBox = row.GetStringOrDefault("box", "");
                if (box != null && rowBox != box)
                    continue;
                result.Add(new ProcEntry
                {
                    Pid = row.GetIntOrDefault("pid", 0),
                    Box = rowBox,
                    Image = row.GetStringOrDefault("image", ""),
                    Session = row.GetIntOrDefault("session", 0),
                    Started = row.GetStringOrDefault("started", ""),
                    Flags = row.GetLongOrDefault("flags", 0),
                });
            }
        }
        return result;
    }

    // ------------------------------------------------------------------ 写操作

    /// <summary>sbie-cli box create &lt;name&gt; --type &lt;t&gt;</summary>
    public Task CreateBoxAsync(string name, string type, CancellationToken ct = default)
    {
        return _cli.RunAsync(new[] { "box", "create", name, "--type", type }, ct: ct);
    }

    /// <summary>sbie-cli box delete &lt;name&gt; [--files]</summary>
    public Task DeleteBoxAsync(string name, bool withFiles, CancellationToken ct = default)
    {
        var args = new List<string> { "box", "delete", name };
        if (withFiles)
            args.Add("--files");
        return _cli.RunAsync(args, ct: ct);
    }

    /// <summary>sbie-cli box rename &lt;old&gt; &lt;new&gt;</summary>
    public Task RenameBoxAsync(string oldName, string newName, CancellationToken ct = default)
    {
        return _cli.RunAsync(new[] { "box", "rename", oldName, newName }, ct: ct);
    }

    /// <summary>sbie-cli box enable|disable &lt;name&gt;</summary>
    public Task SetBoxEnabledAsync(string name, bool enable, CancellationToken ct = default)
    {
        return _cli.RunAsync(new[] { "box", enable ? "enable" : "disable", name }, ct: ct);
    }

    /// <summary>
    /// sbie-cli proc start &lt;box&gt; &lt;cmd&gt;。
    /// CLI 侧把 box 之后的所有位置参数用空格 join 成一条命令行交给 Start.exe
    /// （proc_cmd.cpp CmdProcStart），因此带空格的程序路径必须整体作为一个参数并
    /// 自带引号 —— 这里在服务层完成拼接，UI 只传"程序 + 参数原文"。
    /// </summary>
    public async Task<StartResult> StartProgramAsync(string box, string program, string arguments, CancellationToken ct = default)
    {
        string cmdline = program.Trim();
        if (cmdline.Length == 0)
            throw new CliException(3, "程序路径为空");
        if (cmdline.Contains(' ') && !cmdline.StartsWith('"'))
            cmdline = "\"" + cmdline + "\"";
        string argsTail = arguments.Trim();
        if (argsTail.Length > 0)
            cmdline += " " + argsTail;

        JsonElement data = await _cli.RunAsync(
            new[] { "proc", "start", box, cmdline }, timeoutMs: 90_000, ct: ct).ConfigureAwait(false);
        return new StartResult { Pid = data.GetIntOrDefault("pid", 0) };
    }

    /// <summary>sbie-cli proc kill &lt;pid&gt;</summary>
    public Task KillProcessAsync(int pid, CancellationToken ct = default)
    {
        return _cli.RunAsync(new[] { "proc", "kill", pid.ToString(System.Globalization.CultureInfo.InvariantCulture) },
            ct: ct);
    }

    /// <summary>sbie-cli proc kill-all &lt;box&gt;（不追加 --no-exceptions；skipped 展示给用户）</summary>
    public async Task<KillAllResult> KillAllAsync(string box, CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "proc", "kill-all", box }, ct: ct).ConfigureAwait(false);
        return new KillAllResult
        {
            Count = data.GetIntOrDefault("count", 0),
            Boxes = data.GetIntOrDefault("boxes", 0),
            Skipped = data.GetIntOrDefault("skipped", 0),
            Message = data.GetStringOrDefault("message", ""),
        };
    }

    // ------------------------------------------------------------------ 设置（box dump/set + cfg set/unset）

    private static IReadOnlyList<SettingRow> MergeLines(IEnumerable<IniLine> lines)
    {
        var order = new List<string>();
        var map = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var l in lines)
        {
            if (!map.TryGetValue(l.Key, out var values))
            {
                values = new List<string>();
                map[l.Key] = values;
                order.Add(l.Key);
            }
            values.Add(l.Value);
        }
        return order.Select(k => new SettingRow { Key = k, Values = map[k]! }).ToList();
    }

    private static async Task<IReadOnlyList<SettingRow>> DumpSectionAsync(
        CliBridge cli, IReadOnlyList<string> args, CancellationToken ct)
    {
        JsonElement data = await cli.RunAsync(args, ct: ct).ConfigureAwait(false);
        var lines = new List<IniLine>();
        if (data.TryGetProperty("lines", out var arr) && arr.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement row in arr.EnumerateArray())
                lines.Add(new IniLine
                {
                    Key = row.GetStringOrDefault("key", ""),
                    Value = row.GetStringOrDefault("value", ""),
                });
        }
        return MergeLines(lines);
    }

    /// <summary>box dump &lt;box&gt;：整节键值（多值合并展示）。</summary>
    public Task<IReadOnlyList<SettingRow>> DumpBoxAsync(string box, CancellationToken ct = default) =>
        DumpSectionAsync(_cli, new[] { "box", "dump", box }, ct);

    /// <summary>cfg dump GlobalSettings：全局节键值。</summary>
    public Task<IReadOnlyList<SettingRow>> DumpGlobalAsync(CancellationToken ct = default) =>
        DumpSectionAsync(_cli, new[] { "cfg", "dump", "GlobalSettings" }, ct);

    /// <summary>box set &lt;box&gt; &lt;key&gt; &lt;value&gt; [--append|--insert|--index N]</summary>
    public Task BoxSetAsync(string box, string key, string value, string? mode = null, int index = 0,
        CancellationToken ct = default)
    {
        var args = new List<string> { "box", "set", box, key, value };
        if (mode == "append") args.Add("--append");
        else if (mode == "insert") args.Add("--insert");
        else if (index > 0) { args.Add("--index"); args.Add(index.ToString(System.Globalization.CultureInfo.InvariantCulture)); }
        return _cli.RunAsync(args, ct: ct);
    }

    /// <summary>box get &lt;box&gt; &lt;key&gt;：返回单个值（不存在抛 CliException 5）。</summary>
    public async Task<string> BoxGetAsync(string box, string key, CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "box", "get", box, key }, ct: ct).ConfigureAwait(false);
        return data.GetStringOrDefault("value", "");
    }

    /// <summary>cfg set &lt;key&gt; &lt;value&gt; --section &lt;box&gt;（全局节用 section=GlobalSettings）。</summary>
    public Task CfgSetAsync(string section, string key, string value, bool append = false,
        CancellationToken ct = default)
    {
        var args = new List<string> { "cfg", "set", key, value, "--section", section };
        if (append) args.Add("--append");
        return _cli.RunAsync(args, ct: ct);
    }

    /// <summary>cfg unset &lt;key&gt; --section &lt;section&gt; [--index N]（index=0 删整个键）。</summary>
    public Task CfgUnsetAsync(string section, string key, int index = 0, CancellationToken ct = default)
    {
        var args = new List<string> { "cfg", "unset", key, "--section", section };
        if (index > 0) { args.Add("--index"); args.Add(index.ToString(System.Globalization.CultureInfo.InvariantCulture)); }
        return _cli.RunAsync(args, ct: ct);
    }

    // ------------------------------------------------------------------ 状态卡（force/maint）

    /// <summary>force status：{"disabled","message"}</summary>
    public async Task<ForceStatus> GetForceStatusAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "force", "status" }, ct: ct).ConfigureAwait(false);
        return new ForceStatus
        {
            Disabled = data.GetBoolOrDefault("disabled", false),
            Message = data.GetStringOrDefault("message", ""),
        };
    }

    /// <summary>maint status：[{component,installed,running,state}]</summary>
    public async Task<IReadOnlyList<MaintComponent>> GetMaintStatusAsync(CancellationToken ct = default)
    {
        JsonElement data = await _cli.RunAsync(new[] { "maint", "status" }, ct: ct).ConfigureAwait(false);
        var result = new List<MaintComponent>();
        if (data.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement row in data.EnumerateArray())
            {
                result.Add(new MaintComponent
                {
                    Component = row.GetStringOrDefault("component", ""),
                    Installed = row.GetBoolOrDefault("installed", false),
                    Running = row.GetBoolOrDefault("running", false),
                    State = row.GetStringOrDefault("state", ""),
                });
            }
        }
        return result;
    }
}
