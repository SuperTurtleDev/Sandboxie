// Sandboxie-OSS — SbieCore/Model/Recovery.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 文件恢复实现（06 §P0-12；行为基准见 Recovery.h 文件头）。
// 路径映射的实读对照：
//   * QSbieAPI SbieAPI.cpp GetBoxedPath（:2064-2120）/GetRealPath（:2127-2172）
//     ——用户三类目录（user\current\user\all\user\public）仅 SeparateUserFolders
//     （缺省 y）时参与映射，且优先于盘符；\share 对应 \Device\Mup/UNC；
//   * apps\control\BoxFile.cpp CreateQuickRecoveryFolders（:166-200）——
//     RecoverFolder 逐值 QueryConf（含模板、默认展开）、尾随 \ 剥离、去重；
//   * core\dll\file_recovery.c——SbieDll 内的 RecoverFolder 结构（自动恢复
//     判定用），CLI 侧不需要该结构，仅对齐配置面。

#include "Recovery.h"
#include "ConfigStore.h"
#include "../Util/PathMapper.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace sbie::model {

namespace {

// 大小写不敏感前缀匹配：s 以 root 开头且边界在 '\'（或恰等）时返回
// s 的剩余部分（含开头 '\'；恰等返回空串）。
bool PrefixSuffix(const std::wstring& s, const std::wstring& root,
                  std::wstring* suffix)
{
    if (s.size() < root.size()
        || _wcsnicmp(s.c_str(), root.c_str(), root.size()) != 0)
        return false;
    if (s.size() > root.size() && s[root.size()] != L'\\')
        return false;
    *suffix = s.substr(root.size());
    return true;
}

// 环境变量取目录（client 与 server 同用户会话，值一致）；缺失返回空串 =
// 该目录不参与映射。
std::wstring EnvDir(const wchar_t* name)
{
    wchar_t buf[MAX_PATH + 1] = L"";
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH + 1);
    if (n > 0 && n <= MAX_PATH)
        return buf;
    return std::wstring();
}

void TrimTrailingBackslash(std::wstring* s)
{
    while (s->size() > 3 && (*s)[s->size() - 1] == L'\\')
        s->pop_back();
}

// 递归建目录（目录链逐级 CreateDirectoryW；已存在不视为错误）
bool EnsureDirRecursive(const std::wstring& dir)
{
    if (dir.size() < 2)
        return false;
    const DWORD at = GetFileAttributesW(dir.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        return (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t cut = dir.find_last_of(L"\\");
    if (cut != std::wstring::npos && cut > 2) {   // 盘根（C:\）不再向上
        if (!EnsureDirRecursive(dir.substr(0, cut)))
            return false;
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != 0
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

SbieStatus Win32ToStatus(DWORD e)
{
    switch (e) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return SbieStatus::ACCESS_DENIED;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return SbieStatus::NOT_FOUND;
    default:
        return SbieStatus::GENERIC;
    }
}

// Copy 的失败短路：填 outcome（可空）并返回语义码
SbieStatus FailCopy(RecoverCopyOutcome* out, SbieStatus st,
                    const std::wstring& failedPath, DWORD win32Err)
{
    if (out) {
        out->status = st;
        out->failedPath = failedPath;
        out->win32Error = win32Err;
    }
    return st;
}

// 递归收集目录树内文件（重解析点跳过，与 BoxUsage 口径一致）
void CollectFiles(const std::wstring& dir, const std::wstring& fileRoot,
                  std::vector<RecoverEntry>* out)
{
    std::wstring findPath = dir + L"\\*";
    if (findPath.size() > MAX_PATH - 12)
        findPath = L"\\\\?\\" + findPath;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(findPath.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFiles(full, fileRoot, out);
            continue;
        }
        RecoverEntry e;
        e.sandboxPath = full;
        e.boxPath = full.substr(fileRoot.size());
        e.size = ((unsigned long long)fd.nFileSizeHigh << 32)
                 | fd.nFileSizeLow;
        out->push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 单个展开后的 RecoverFolder 值 → 沙箱内目录（GetBoxedPath 规则）；
// 不可映射返回空串。
std::wstring MapFolderToSandbox(const std::wstring& value,
                                const std::wstring& root,
                                const std::wstring& box)
{
    std::wstring sandbox;
    if (value.size() >= 11 && _wcsnicmp(value.c_str(), L"\\Device\\Mup", 11) == 0
        && (value.size() == 11 || value[11] == L'\\')) {
        return root + L"\\share" + value.substr(11);   // UNC 重定向器
    }

    std::wstring dos = value;
    util::NtToDosPath(&dos);
    if (dos.size() >= 2 && dos[0] == L'\\' && dos[1] == L'\\')
        return root + L"\\share\\" + dos.substr(2);
    if (dos.size() < 3 || dos[1] != L':')
        return std::wstring();   // \RPC Control\… 等不可映射

    // SeparateUserFolders（缺省 y）：用户三类目录优先于盘符
    auto sep = ConfigStore().Get(box, L"SeparateUserFolders", 0, true, false);
    const bool separate =
        !sep.has_value() || (*sep != L"n" && *sep != L"N");
    if (separate) {
        std::wstring suffix;
        const std::wstring prof = EnvDir(L"USERPROFILE");
        const std::wstring pd = EnvDir(L"ProgramData").empty()
                                    ? EnvDir(L"ALLUSERSPROFILE")
                                    : EnvDir(L"ProgramData");
        const std::wstring pub = EnvDir(L"PUBLIC");
        if (!prof.empty() && PrefixSuffix(dos, prof, &suffix))
            return root + L"\\user\\current" + suffix;
        if (!pd.empty() && PrefixSuffix(dos, pd, &suffix))
            return root + L"\\user\\all" + suffix;
        if (!pub.empty() && PrefixSuffix(dos, pub, &suffix))
            return root + L"\\user\\public" + suffix;
    }
    return root + L"\\drive\\" + dos[0] + dos.substr(2);
}

} // namespace

RecoveryManager::RecoveryManager(const BoxInfo& box) : box_(box) {}

std::vector<RecoverEntry> RecoveryManager::List()
{
    std::vector<RecoverEntry> out;
    if (box_.name.empty() || box_.fileRoot.empty())
        return out;   // 未初始化沙箱
    std::wstring root = box_.fileRoot;
    TrimTrailingBackslash(&root);

    // RecoverFolder 值：含模板注入（BoxFile.cpp 语义）、驱动展开 %env%
    //（展开产物为 NT 路径，如 \Device\HarddiskVolume3\Users\…\Documents）
    std::vector<std::wstring> folders =
        ConfigStore().GetList(box_.name, L"RecoverFolder", false, false);

    // 各值 → 沙箱内路径（GetBoxedPath 规则）+ 去重（qrPathDup 语义）
    std::vector<std::wstring> scanRoots;
    for (std::wstring v : folders) {
        TrimTrailingBackslash(&v);
        if (v.empty())
            continue;
        std::wstring sandbox = MapFolderToSandbox(v, root, box_.name);
        if (sandbox.empty())
            continue;
        bool dup = false;
        for (const std::wstring& s : scanRoots)
            if (_wcsicmp(s.c_str(), sandbox.c_str()) == 0) {
                dup = true;
                break;
            }
        if (!dup)
            scanRoots.push_back(std::move(sandbox));
    }

    for (const std::wstring& sr : scanRoots) {
        const DWORD at = GetFileAttributesW(sr.c_str());
        if (at == INVALID_FILE_ATTRIBUTES
            || !(at & FILE_ATTRIBUTE_DIRECTORY))
            continue;   // 沙箱内该目录无内容（无可恢复文件）
        CollectFiles(sr, root, &out);
    }

    // 排序（索引稳定）+ 去重（多个 RecoverFolder 目录相互包含时）
    std::sort(out.begin(), out.end(),
              [](const RecoverEntry& a, const RecoverEntry& b) {
                  return _wcsicmp(a.sandboxPath.c_str(), b.sandboxPath.c_str())
                         < 0;
              });
    std::vector<RecoverEntry> uniq;
    for (RecoverEntry& e : out) {
        if (!uniq.empty() && _wcsicmp(uniq.back().sandboxPath.c_str(),
                                      e.sandboxPath.c_str()) == 0)
            continue;
        MapToRealPath(e.sandboxPath, &e.targetPath);
        uniq.push_back(std::move(e));
    }
    return uniq;
}

bool RecoveryManager::MapToRealPath(const std::wstring& sandboxPath,
                                    std::wstring* out) const
{
    if (!out || box_.fileRoot.empty())
        return false;
    std::wstring root = box_.fileRoot;
    TrimTrailingBackslash(&root);
    std::wstring rel;
    if (!PrefixSuffix(sandboxPath, root, &rel) || rel.empty())
        return false;

    // 剥 \snapshot-<id>\ 前缀（GetRealPath 语义；当前扫描根不进入快照目录，
    // 此处为对直接给出快照内路径的防御）
    if (rel.size() > 10 && _wcsnicmp(rel.c_str(), L"\\snapshot-", 10) == 0) {
        const size_t bs = rel.find(L'\\', 1);
        if (bs != std::wstring::npos)
            rel = rel.substr(bs);
        if (rel.empty())
            return false;
    }

    if (_wcsnicmp(rel.c_str(), L"\\drive\\", 7) == 0 && rel.size() >= 8) {
        const std::wstring letter(1, (wchar_t)towlower(rel[7]));
        if (rel.size() == 8)
            *out = letter + L":\\";
        else
            *out = letter + L":" + rel.substr(8);
        return true;
    }
    if (rel.size() >= 7 && _wcsnicmp(rel.c_str(), L"\\share\\", 7) == 0) {
        *out = L"\\\\" + rel.substr(7);
        return true;
    }
    if (rel.size() >= 14 && _wcsnicmp(rel.c_str(), L"\\user\\current\\", 14) == 0) {
        const std::wstring prof = EnvDir(L"USERPROFILE");
        if (!prof.empty()) {
            *out = prof + rel.substr(13);
            return true;
        }
    }
    if (rel.size() >= 10 && _wcsnicmp(rel.c_str(), L"\\user\\all\\", 10) == 0) {
        std::wstring pd = EnvDir(L"ProgramData");
        if (pd.empty())
            pd = EnvDir(L"ALLUSERSPROFILE");
        if (!pd.empty()) {
            *out = pd + rel.substr(9);
            return true;
        }
    }
    if (rel.size() >= 13 && _wcsnicmp(rel.c_str(), L"\\user\\public\\", 13) == 0) {
        const std::wstring pub = EnvDir(L"PUBLIC");
        if (!pub.empty()) {
            *out = pub + rel.substr(12);
            return true;
        }
    }
    return false;
}

SbieStatus RecoveryManager::Copy(const std::vector<std::wstring>& sandboxPaths,
                                 const std::wstring& toDir, bool overwrite,
                                 RecoverCopyOutcome* out)
{
    // 原冻结签名行为保持：拷贝语义、无检查器（CLI/IPC 路径均走 CopyEx，
    // 07-P1-3 的 move/检查器由调用方经 RecoverCopyOptions 开启）
    RecoverCopyOptions legacy{};
    legacy.move = false;
    legacy.runCheckers = false;
    return CopyEx(sandboxPaths, toDir, overwrite, legacy, out);
}

// OnFileRecovery 检查器（07-P1-3）：cmd 形态 = 键值（%SANDBOX% 展开）+
// 空格 + 带引号的沙箱路径（SandMan CheckFilesAsync 的命令拼装语义——
// SandManRecovery.cpp:196-228 记录于 docs/07 §2/§3.2）。宿主执行、
// CREATE_NO_WINDOW、等待 ≤15s；非零退出（含启动失败/超时）= 拒绝。
bool RunFileChecker(const std::wstring& cmd, const std::wstring& sandboxPath)
{
    std::wstring line = cmd;
    if (!line.empty() && line.back() != L' ')
        line += L' ';
    line += L"\"" + sandboxPath + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;   // 启动失败 = 检查不通过（拒绝）
    const bool ok = WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (ok)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok && code == 0;   // 超时 = 拒绝（不杀进程，RunBoxTriggers 同款）
}

SbieStatus RecoveryManager::CopyEx(const std::vector<std::wstring>& sandboxPaths,
                                   const std::wstring& toDir, bool overwrite,
                                   const RecoverCopyOptions& opts,
                                   RecoverCopyOutcome* out)
{
    if (out)
        *out = RecoverCopyOutcome{};
    if (box_.fileRoot.empty())
        return FailCopy(out, SbieStatus::NOT_FOUND, L"", 0);

    std::wstring root = box_.fileRoot;
    TrimTrailingBackslash(&root);

    // 检查器命令集（一次读出；键不存在 = 空集 = 无校验）
    std::vector<std::wstring> checkers;
    if (opts.runCheckers)
        checkers = ConfigStore().GetList(box_.name, L"OnFileRecovery");

    for (const std::wstring& src : sandboxPaths) {
        std::wstring rel;
        if (!PrefixSuffix(src, root, &rel))
            return FailCopy(out, SbieStatus::INVALID, src, 0);

        const DWORD at = GetFileAttributesW(src.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY))
            return FailCopy(out, SbieStatus::NOT_FOUND, src, 0);

        // 恢复前校验（07-P1-3）：任一检查器非零 = 拒绝该文件（跳过、列出）
        if (!checkers.empty()) {
            bool rejected = false;
            for (const std::wstring& raw : checkers) {
                const std::wstring cmd = ExpandSandboxVar(raw, box_.name);
                if (cmd.empty())
                    continue;
                if (!RunFileChecker(cmd, src)) {
                    rejected = true;
                    break;
                }
            }
            if (rejected) {
                if (out) {
                    ++out->skippedFiles;
                    out->skippedPaths.push_back(src);
                }
                continue;
            }
        }

        std::wstring dest;
        if (!toDir.empty()) {
            dest = toDir + rel;   // 保留 FileRoot 相对结构（建目录）
        } else if (!MapToRealPath(src, &dest)) {
            return FailCopy(out, SbieStatus::INVALID, src, 0);
        }

        const size_t cut = dest.find_last_of(L"\\");
        if (cut != std::wstring::npos
            && !EnsureDirRecursive(dest.substr(0, cut)))
            return FailCopy(out, SbieStatus::GENERIC, dest, GetLastError());

        if (!overwrite
            && GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
            return FailCopy(out, SbieStatus::GENERIC, dest, ERROR_FILE_EXISTS);

        // CopyFileW 保留 LastWriteTime（mtime）；FALSE = 允许覆盖既有目标
        if (!CopyFileW(src.c_str(), dest.c_str(), FALSE))
            return FailCopy(out, Win32ToStatus(GetLastError()), dest,
                            GetLastError());

        // 尺寸采集在删源之前（move 后源已不存在）
        unsigned long long szBytes = 0;
        {
            LARGE_INTEGER sz{};
            HANDLE hf = CreateFileW(src.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE
                                        | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                if (GetFileSizeEx(hf, &sz))
                    szBytes = (unsigned long long)sz.QuadPart;
                CloseHandle(hf);
            }
        }

        // 移动语义（07-P1-3）：拷贝成功后删沙箱源；失败 = 该条目失败
        //（目标已拷出、源保留——重试安全）
        if (opts.move && !DeleteFileW(src.c_str()))
            return FailCopy(out, Win32ToStatus(GetLastError()), src,
                            GetLastError());

        if (out) {
            ++out->copiedFiles;
            out->copiedBytes += szBytes;
        }
    }
    return SbieStatus::OK;
}

SbieStatus RecoverAddFolder(const std::wstring& box, const std::wstring& folder,
                            const std::wstring& password)
{
    if (folder.empty())
        return SbieStatus::INVALID;

    std::wstring value = folder;
    TrimTrailingBackslash(&value);

    // 形态校验（存储 = 原样，与 SandMan OnAddFolder 写 DOS 形态一致——
    // 驱动读路径的 Conf_Expand 对 DOS 路径自动转 NT（conf_expand.c
    // File_TranslateDosToNt 分支），两形态等价；RecoveryManager::List 的
    // 正向映射对 DOS/NT/UNC 三形态均识别）
    const bool okForm =
        value[0] == L'%'                                    // %var% 引用
        || (value.size() >= 3 && value[1] == L':'
            && value[2] == L'\\')                           // DOS 盘符
        || (value.size() >= 2 && value[0] == L'\\'
            && value[1] == L'\\')                           // UNC（\\server\…）
        || (value.size() >= 8
            && _wcsnicmp(value.c_str(), L"\\Device\\", 8) == 0);   // NT
    if (!okForm)
        return SbieStatus::INVALID;

    return ConfigStore().SetAppend(box, L"RecoverFolder", value, true, password);
}

} // namespace sbie::model
