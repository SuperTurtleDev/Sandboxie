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
// CliBridge：sbie-cli.exe 子进程调用 + --json 信封解析（docs/04-modules.md §7.2）。
// 本类不依赖 WinUI（可被 console harness / SelfTest 单独使用）。
// 架构原则（docs/09-gui.md）：GUI 永不直接链接驱动/SbieCore，全部经 CLI 进程隔离。
// ---------------------------------------------------------------------------

using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace sbie_gui.Services;

/// <summary>
/// sbie-cli 报告的业务错误（信封 {"ok":false,"error":{"code","message"}}）。
/// Code 与 CLI 退出码一致（5=NOT_FOUND、3=USAGE 等）。
/// </summary>
public sealed class CliException : Exception
{
    public int Code { get; }

    public CliException(int code, string message) : base(message)
    {
        Code = code;
    }
}

/// <summary>
/// sbie-cli.exe 子进程桥。一次实例 = 一个定位好的 CLI 路径；调用本身无状态、可并发。
/// </summary>
public sealed class CliBridge
{
    public const int DefaultTimeoutMs = 45_000;

    public string CliPath { get; }

    private CliBridge(string cliPath)
    {
        CliPath = cliPath;
    }

    // ------------------------------------------------------------------ 定位

    /// <summary>
    /// 定位 sbie-cli.exe。顺序：
    ///   1) 本程序目录及各级祖先目录下的 sbie-cli.exe
    ///      （发布布局：SbiePlus_x64\sbie-gui\sbie-gui.exe -> 上一级即 SbiePlus_x64\sbie-cli.exe）
    ///   2) 各级祖先目录下 Installer\SbiePlus_x64\sbie-cli.exe（仓库开发布局）
    /// </summary>
    public static string? LocateCli()
    {
        string dir = AppContext.BaseDirectory;
        for (int i = 0; i < 10 && dir.Length > 3; i++)
        {
            string direct = Path.Combine(dir, "sbie-cli.exe");
            if (File.Exists(direct))
                return direct;

            string installer = Path.Combine(dir, "Installer", "SbiePlus_x64", "sbie-cli.exe");
            if (File.Exists(installer))
                return installer;

            string? parent = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar));
            if (parent == null || parent == dir)
                break;
            dir = parent;
        }
        return null;
    }

    /// <summary>定位失败抛 CliException（由 UI 展示）。</summary>
    public static CliBridge Create()
    {
        string? path = LocateCli();
        if (path == null)
            throw new CliException(-1, "未找到 sbie-cli.exe（请将 sbie-gui 放在 Sandboxie 安装目录的 sbie-gui 子目录中运行）");
        return new CliBridge(path);
    }

    public static CliBridge Create(string explicitPath)
    {
        if (!File.Exists(explicitPath))
            throw new CliException(-1, "sbie-cli.exe 不存在: " + explicitPath);
        return new CliBridge(Path.GetFullPath(explicitPath));
    }

    // ------------------------------------------------------------------ 执行

    /// <summary>
    /// 运行 CLI 并返回信封中的 data（JsonElement，调用方自行映射为模型）。
    /// 不使用 --no-server：让 CLI 自行拉起/复用 sbie-cli server。
    /// </summary>
    public async Task<JsonElement> RunAsync(
        IReadOnlyList<string> args,
        int timeoutMs = DefaultTimeoutMs,
        CancellationToken ct = default)
    {
        (int exitCode, string stdout, string stderr) =
            await RunRawAsync(args, timeoutMs, ct).ConfigureAwait(false);

        if (string.IsNullOrWhiteSpace(stdout))
        {
            string detail = stderr.Trim();
            throw new CliException(
                exitCode != 0 ? exitCode : -1,
                "sbie-cli 无输出（退出码 " + exitCode + (detail.Length > 0 ? "）：" + detail : "）"));
        }

        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(stdout);
        }
        catch (JsonException e)
        {
            throw new CliException(-1, "sbie-cli 输出不是合法 JSON: " + Truncate(stdout, 200) + " (" + e.Message + ")");
        }

        using (doc)
        {
            JsonElement root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new CliException(-1, "sbie-cli JSON 顶层不是对象: " + Truncate(stdout, 200));

            bool ok = root.TryGetProperty("ok", out var okEl) && okEl.ValueKind == JsonValueKind.True;

            if (!ok)
            {
                int code = -1;
                string message = "未知错误";
                if (root.TryGetProperty("error", out var errEl) && errEl.ValueKind == JsonValueKind.Object)
                {
                    code = errEl.GetIntOrDefault("code", -1);
                    message = errEl.GetStringOrDefault("message", "未知错误");
                }
                throw new CliException(code, message);
            }

            if (!root.TryGetProperty("data", out var dataEl))
                throw new CliException(-1, "sbie-cli JSON 缺少 data 字段: " + Truncate(stdout, 200));

            // JsonDocument 已被 Dispose，将 data 克隆出来供调用方继续使用
            return dataEl.Clone();
        }
    }

    /// <summary>
    /// 原始进程调用（不解析 JSON），返回 (退出码, stdout, stderr)。
    /// 超时/取消时杀整棵进程树并抛 TimeoutException/OperationCanceledException。
    /// </summary>
    public async Task<(int ExitCode, string Stdout, string Stderr)> RunRawAsync(
        IReadOnlyList<string> args,
        int timeoutMs = DefaultTimeoutMs,
        CancellationToken ct = default)
    {
        string full = BuildArgumentList(args);
        var psi = new ProcessStartInfo
        {
            FileName = CliPath,
            Arguments = full,                       // --json 已由 BuildArgumentList 前置
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, // CLI 输出恒 UTF-8（04 §7）
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = Path.GetDirectoryName(CliPath) ?? Environment.CurrentDirectory,
        };

        using var process = new Process { StartInfo = psi };

        var stdoutBuilder = new StringBuilder();
        var stderrBuilder = new StringBuilder();
        process.OutputDataReceived += (_, e) => { if (e.Data != null) stdoutBuilder.AppendLine(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) stderrBuilder.AppendLine(e.Data); };

        if (!process.Start())
            throw new CliException(-1, "sbie-cli.exe 启动失败: " + CliPath);

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(timeoutMs);
        try
        {
            // .NET 8：WaitForExitAsync 会等待异步输出读完（不会丢尾部行）
            await process.WaitForExitAsync(timeoutCts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            TryKillTree(process);
            ct.ThrowIfCancellationRequested();
            throw new TimeoutException("sbie-cli 超时（>" + timeoutMs + " ms）: " + full);
        }

        return (process.ExitCode, stdoutBuilder.ToString(), stderrBuilder.ToString());
    }

    private static void TryKillTree(Process process)
    {
        try
        {
            process.Kill(entireProcessTree: true);
        }
        catch
        {
            // 进程恰好已退出等 — 忽略
        }
    }

    // ------------------------------------------------------------------ 参数

    /// <summary>拼 "--json args..."，Windows 规则转义每个参数。</summary>
    internal static string BuildArgumentList(IReadOnlyList<string> args)
    {
        var sb = new StringBuilder();
        sb.Append(QuoteArg("--json"));
        foreach (string a in args)
        {
            sb.Append(' ').Append(QuoteArg(a));
        }
        return sb.ToString();
    }

    /// <summary>
    /// Windows 命令行参数引用（CommandLineToArgvW 规则）：
    /// 含空白/引号才加外引号；内引号转义为 \"，引号前反斜杠翻倍。
    /// </summary>
    internal static string QuoteArg(string arg)
    {
        if (arg.Length == 0)
            return "\"\"";
        bool needsQuotes = arg.Contains(' ') || arg.Contains('\t') || arg.Contains('"');
        if (!needsQuotes)
            return arg;

        var sb = new StringBuilder();
        sb.Append('"');
        int backslashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\')
            {
                backslashes++;
                continue;
            }
            if (c == '"')
            {
                sb.Append('\\', backslashes * 2 + 1).Append('"');
            }
            else
            {
                sb.Append('\\', backslashes);
                sb.Append(c);
            }
            backslashes = 0;
        }
        sb.Append('\\', backslashes * 2);
        sb.Append('"');
        return sb.ToString();
    }

    private static string Truncate(string s, int max) =>
        s.Length <= max ? s : s.Substring(0, max) + "…";
}

internal static class JsonElementExtensions
{
    public static int GetIntOrDefault(this JsonElement el, string name, int fallback)
    {
        if (el.ValueKind == JsonValueKind.Object &&
            el.TryGetProperty(name, out var v) &&
            v.ValueKind == JsonValueKind.Number &&
            v.TryGetInt32(out int i))
            return i;
        return fallback;
    }

    public static long GetLongOrDefault(this JsonElement el, string name, long fallback)
    {
        if (el.ValueKind == JsonValueKind.Object &&
            el.TryGetProperty(name, out var v) &&
            v.ValueKind == JsonValueKind.Number &&
            v.TryGetInt64(out long l))
            return l;
        return fallback;
    }

    public static string GetStringOrDefault(this JsonElement el, string name, string fallback)
    {
        if (el.ValueKind == JsonValueKind.Object &&
            el.TryGetProperty(name, out var v) &&
            v.ValueKind == JsonValueKind.String)
            return v.GetString() ?? fallback;
        return fallback;
    }

    public static bool GetBoolOrDefault(this JsonElement el, string name, bool fallback)
    {
        if (el.ValueKind == JsonValueKind.Object &&
            el.TryGetProperty(name, out var v) &&
            v.ValueKind is JsonValueKind.True or JsonValueKind.False)
            return v.GetBoolean();
        return fallback;
    }
}
