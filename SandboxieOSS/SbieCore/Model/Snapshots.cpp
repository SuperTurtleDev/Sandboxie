// Sandboxie-OSS — SbieCore/Model/Snapshots.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 快照实现（契约：04-modules.md §2.4；行为对齐 QSbieAPI SandBox.cpp:353-505，
// LGPL 仅参考未复制）。
//
// 导出表核实结论（2026-09-27，本机 5.73.5 x64 SbieDll.dll，137 个命名导出）：
//   无任何 SbieDll_*/SbieApi_* 快照导出 —— 快照完全是 GUI 层文件约定：
//   <FileRoot>\Snapshots.ini + <FileRoot>\snapshot-<ID>\{drive,user,share,
//   RegHive,RegPaths.dat,FilePaths.dat}。沙箱内 SbieDll 读 Snapshots.ini 的
//   [Current]/Snapshot 构建 current→parent 链，经 File_FindSnapshotPath
//   （core\dll\file_snapshots.c）把根目录缺失文件的读取回退到快照目录——
//   因此 select 不搬快照目录、只清根目录；remove(current) 则必须把内容并回根。
//
// ini 文件格式兼容性：QSettings::IniFormat（UTF-8 无 BOM，Key=Value，特殊值
// 双引号包裹）与 GetPrivateProfileStringW（首=分割、外层引号剥除）公共子集。

#include "Snapshots.h"
#include "../DriverApi/DriverApi.h"
#include "../Util/PathMapper.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace sbie::model {

namespace {

// ── 布局常量（对齐 SandBox.cpp 的 CSandBox__BoxSubFolders / __BoxDataFiles）──

const wchar_t* const kSubFolders[] = { L"drive", L"user", L"share" };

struct DataFile {
    const wchar_t* name;
    bool required;     // 复制失败即失败（RegHive）
    bool recursive;    // 增量文件（FilePaths.dat）：复制后删除源
};

const DataFile kDataFiles[] = {
    { L"RegHive",     true,  false },
    { L"RegPaths.dat", false, false },
    { L"FilePaths.dat", false, true },
};

const wchar_t* kIniName = L"Snapshots.ini";
const wchar_t* kSnapPrefix = L"Snapshot_";      // 节名前缀
const wchar_t* kSnapDirPrefix = L"snapshot-";   // 目录名前缀
const size_t kMaxSnapshotId = 17;               // FILE_MAX_SNAPSHOT_ID

// ── 最小有序 ini（读/写 QSettings 兼容子集）────────────────────────────────

struct IniFile {
    struct Entry { std::wstring key, value; };
    struct Section {
        std::wstring name;
        std::vector<Entry> entries;
        std::wstring Get(const std::wstring& key) const
        {
            for (const Entry& e : entries)
                if (_wcsicmp(e.key.c_str(), key.c_str()) == 0)
                    return e.value;
            return L"";
        }
    };
    std::vector<Section> sections;

    const Section* Find(const std::wstring& name) const
    {
        for (const Section& s : sections)
            if (_wcsicmp(s.name.c_str(), name.c_str()) == 0)
                return &s;
        return nullptr;
    }
    Section* Find(const std::wstring& name)
    {
        for (Section& s : sections)
            if (_wcsicmp(s.name.c_str(), name.c_str()) == 0)
                return &s;
        return nullptr;
    }
    Section& Get(const std::wstring& name)
    {
        if (Section* s = Find(name))
            return *s;
        sections.push_back(Section{ name, {} });
        return sections.back();
    }
    bool RemoveSection(const std::wstring& name)
    {
        for (auto it = sections.begin(); it != sections.end(); ++it)
            if (_wcsicmp(it->name.c_str(), name.c_str()) == 0) {
                sections.erase(it);
                return true;
            }
        return false;
    }
    std::wstring Get(const std::wstring& sec, const std::wstring& key) const
    {
        const Section* s = Find(sec);
        return s ? s->Get(key) : L"";
    }
    void Set(const std::wstring& sec, const std::wstring& key,
             const std::wstring& value)
    {
        Section& s = Get(sec);
        for (Entry& e : s.entries)
            if (_wcsicmp(e.key.c_str(), key.c_str()) == 0) {
                e.value = value;
                return;
            }
        s.entries.push_back(Entry{ key, value });
    }
    bool HasSection(const std::wstring& sec) const { return Find(sec) != nullptr; }
};

// 值解码：剥外层引号 + QSettings 反转义（\\ \" \n \; \, \=）
std::wstring DecodeValue(std::wstring v)
{
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    std::wstring out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t c = v[++i];
            if (c == L'n') out += L'\n';
            else if (c == L';' || c == L',' || c == L'=' || c == L'\\'
                     || c == L'"')
                out += c;
            else out += L'\\', out += c;   // 未知转义原样保留
        } else {
            out += v[i];
        }
    }
    return out;
}

// 值编码：含 ; 开头/结尾空白/控制字符时引号包裹（QSettings 子集）
std::wstring EncodeValue(const std::wstring& v)
{
    bool needQuote = false;
    for (wchar_t c : v)
        if (c < 0x20 || c == L';')
            needQuote = true;
    if (!v.empty() && (v.front() == L' ' || v.back() == L' '
                       || v.front() == L'"'))
        needQuote = true;
    if (!needQuote)
        return v;
    std::wstring out = L"\"";
    for (wchar_t c : v) {
        if (c == L'\\' || c == L'"') out += L'\\', out += c;
        else if (c == L'\n') out += L"\\n";
        else if (c == L';' || c == L',') out += L'\\', out += c;
        else out += c;
    }
    out += L'"';
    return out;
}

bool LoadIniFile(const std::wstring& path, IniFile* out)
{
    out->sections.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    ULONG size = GetFileSize(h, nullptr);
    if (size == 0xFFFFFFFF) {
        CloseHandle(h);
        return false;
    }
    if (size == 0) {
        CloseHandle(h);
        return true;   // 空文件 = 无节
    }
    if (size > 16u * 1024u * 1024u) {
        CloseHandle(h);
        return false;   // 异常体积，拒绝解析（防误写坏真实快照集）
    }
    std::vector<char> raw(size);
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw.data(), size, &read, nullptr);
    CloseHandle(h);
    if (!ok)
        return false;
    raw.resize(read);

    // 编码：UTF-16 BOM → UCS2；否则按 UTF-8（无 BOM）读（QSettings 惯例）
    std::wstring text;
    if (raw.size() >= 2
        && ((unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)) {
        text.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2),
                    (raw.size() - 2) / sizeof(wchar_t));
    } else {
        int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(),
                                    nullptr, 0);
        text.resize((size_t)n);
        if (n)
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(),
                                text.data(), n);
    }

    // 行拆分
    IniFile::Section* cur = nullptr;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(
            pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '
                                 || line.back() == L'\t'))
            line.pop_back();
        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos)
            continue;
        line = line.substr(b);
        if (line[0] == L'[') {
            size_t close = line.find(L']');
            std::wstring name = (close == std::wstring::npos)
                ? line.substr(1) : line.substr(1, close - 1);
            cur = &out->Get(name);
            continue;
        }
        if (line[0] == L';' || line[0] == L'#')
            continue;   // 注释
        if (!cur)
            continue;   // 节外行忽略
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            cur->entries.push_back({ line, L"" });
        } else {
            std::wstring k = line.substr(0, eq);
            size_t ke = k.size();
            while (ke > 0 && (k[ke - 1] == L' ' || k[ke - 1] == L'\t'))
                --ke;
            k.resize(ke);
            size_t vs = eq + 1;
            while (vs < line.size() && (line[vs] == L' ' || line[vs] == L'\t'))
                ++vs;
            cur->entries.push_back({ k, DecodeValue(line.substr(vs)) });
        }
    }
    return true;
}

bool SaveIniFile(const std::wstring& path, const IniFile& ini)
{
    std::wstring text;
    bool first = true;
    for (const IniFile::Section& s : ini.sections) {
        if (!first)
            text += L"\n";
        first = false;
        text += L"[" + s.name + L"]\n";
        for (const IniFile::Entry& e : s.entries)
            text += e.key + L"=" + EncodeValue(e.value) + L"\n";
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string bytes;
    if (n > 0) {
        bytes.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                            bytes.data(), n, nullptr, nullptr);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = bytes.empty() ? TRUE
                            : WriteFile(h, bytes.data(), (DWORD)bytes.size(),
                                        &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// ── 文件系统小工具 ────────────────────────────────────────────────────────

bool DirExists(const std::wstring& path)
{
    DWORD at = GetFileAttributesW(path.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path)
{
    DWORD at = GetFileAttributesW(path.c_str());
    return at != INVALID_FILE_ATTRIBUTES
           && (at & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDir(const std::wstring& path)
{
    if (DirExists(path))
        return true;
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE;
}

// 递归删除目录（清只读/系统属性后删；对齐 NtIo_DeleteFolderRecursively 行为）
SbieStatus DeleteDirRecursive(const std::wstring& dir)
{
    if (!DirExists(dir))
        return SbieStatus::OK;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            st = DeleteDirRecursive(full);
        } else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (st != SbieStatus::OK)
        return st;
    // 目录可能带 READ-ONLY 属性（box 树内由 Sandboxie 设置）——先清再删
    // （坑 §8.18：RemoveDirectoryW 对 R 目录返回 ACCESS_DENIED）
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(dir.c_str()))
        return SbieStatus::GENERIC;
    return SbieStatus::OK;
}

// 递归合并复制 src→dst：文件冲突时 src（较新状态）覆盖 dst；目录递归合并。
// （简化版 NtIo_MergeFolder：不处理 FilePaths.dat 墓碑语义，坑记录 §）
SbieStatus MergeCopyDir(const std::wstring& src, const std::wstring& dst)
{
    if (!DirExists(src))
        return SbieStatus::OK;   // 无可合并（参考：merge 源缺失 = no-op）
    if (!EnsureDir(dst))
        return SbieStatus::GENERIC;
    std::wstring pattern = src + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return SbieStatus::GENERIC;
    SbieStatus st = SbieStatus::OK;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring s = src + L"\\" + name;
        std::wstring d = dst + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            st = MergeCopyDir(s, d);
        } else if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
            st = SbieStatus::GENERIC;
        }
        if (st != SbieStatus::OK)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return st;
}

bool MoveEntry(const std::wstring& src, const std::wstring& dst)
{
    if (!MoveFileExW(src.c_str(), dst.c_str(),
                     MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return true;   // 参考实现容忍缺失（NtIo_RenameFolder 同款）
        return false;
    }
    return true;
}

void MoveDataFiles(const std::wstring& fromDir, const std::wstring& toDir)
{
    for (const DataFile& df : kDataFiles) {
        std::wstring src = fromDir + L"\\" + df.name;
        std::wstring dst = toDir + L"\\" + df.name;
        if (!FileExists(src))
            continue;
        DeleteFileW(dst.c_str());
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH);
    }
}

ULONGLONG NowUnixSeconds()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ull) / 10000000ull;
}

// 快照 ID 必须是纯十进制数字、无前导零（防路径穿越；ID 由 1 递增生成，
// 对齐 FILE_MAX_SNAPSHOT_ID=17）
bool ValidSnapshotId(const std::wstring& id)
{
    if (id.empty() || id.size() > kMaxSnapshotId)
        return false;
    for (wchar_t c : id)
        if (c < L'0' || c > L'9')
            return false;
    return id[0] != L'0';
}

unsigned long long IdToUll(const std::wstring& id)
{
    return _wcstoui64(id.c_str(), nullptr, 10);
}

std::wstring SnapDir(const std::wstring& root, const std::wstring& id)
{
    return root + L"\\" + kSnapDirPrefix + id;
}

} // namespace

// ---------------------------------------------------------------------------
// SnapshotManager
// ---------------------------------------------------------------------------

SnapshotManager::SnapshotManager(const BoxInfo& box)
    : box_(box)
{
    // 防御：传入的可能是 NT 形态路径（GetInfo 已 DOS 化，但直接构造者未必）
    util::NtToDosPath(&box_.fileRoot);
    // 剥结尾反斜杠，统一 join 语义
    while (!box_.fileRoot.empty()
           && box_.fileRoot.back() == L'\\')
        box_.fileRoot.pop_back();
}

std::vector<SnapshotInfo> SnapshotManager::List(std::wstring* currentId,
                                                std::wstring* defaultId)
{
    std::vector<SnapshotInfo> out;
    IniFile ini;
    LoadIniFile(box_.fileRoot + L"\\" + kIniName, &ini);
    for (const IniFile::Section& s : ini.sections) {
        if (_wcsnicmp(s.name.c_str(), kSnapPrefix, 9) != 0)
            continue;
        SnapshotInfo si;
        si.id = s.name.substr(9);
        if (!ValidSnapshotId(si.id))
            continue;
        si.parentId = s.Get(L"Parent");
        si.name = s.Get(L"Name");
        si.info = s.Get(L"Description");
        si.date = _wcstoui64(s.Get(L"SnapshotDate").c_str(), nullptr, 10);
        out.push_back(std::move(si));
    }
    std::sort(out.begin(), out.end(), [](const SnapshotInfo& a,
                                         const SnapshotInfo& b) {
        return IdToUll(a.id) < IdToUll(b.id);
    });
    if (currentId)
        *currentId = ini.Get(L"Current", L"Snapshot");
    if (defaultId)
        *defaultId = ini.Get(L"Current", L"Default");
    return out;
}

bool SnapshotManager::HasAny()
{
    return FileExists(box_.fileRoot + L"\\" + kIniName);
}

SbieStatus SnapshotManager::Take(const std::wstring& name)
{
    if (box_.fileRoot.empty())
        return SbieStatus::GENERIC;

    // 要求沙箱内无进程（SB_SnapIsRunning 语义 → BOX_BUSY）
    {
        std::vector<ULONG> pids;
        SbieStatus st = drv::EnumBoxProcesses(box_.name, false, &pids);
        if (st != SbieStatus::OK)
            return st;   // 驱动不可用等
        if (!pids.empty())
            return SbieStatus::BOX_BUSY;
    }

    // 要求沙箱已初始化（SB_SnapIsEmpty 语义；IsInitialized = 任一子目录或
    // RegHive 存在）
    bool initialized = FileExists(box_.fileRoot + L"\\RegHive");
    for (const wchar_t* sub : kSubFolders)
        initialized = initialized || DirExists(box_.fileRoot + L"\\" + sub);
    if (!initialized)
        return SbieStatus::GENERIC;

    IniFile ini;
    LoadIniFile(box_.fileRoot + L"\\" + kIniName, &ini);

    // 递增分配空闲 ID（1 起）
    std::wstring id;
    for (unsigned long long i = 1;; ++i) {
        wchar_t buf[24];
        swprintf_s(buf, L"%llu", i);
        if (!ini.HasSection(kSnapPrefix + std::wstring(buf))) {
            id = buf;
            break;
        }
    }
    const std::wstring snapDir = SnapDir(box_.fileRoot, id);
    if (!EnsureDir(snapDir))
        return SbieStatus::GENERIC;

    // 数据文件：RegHive 必需 / RegPaths.dat 可选 / FilePaths.dat 增量（复制后删源）
    for (const DataFile& df : kDataFiles) {
        std::wstring src = box_.fileRoot + L"\\" + df.name;
        std::wstring dst = snapDir + L"\\" + df.name;
        if (!FileExists(src)) {
            if (df.required)
                return SbieStatus::GENERIC;   // SB_SnapCopyDatFail
            continue;
        }
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            return SbieStatus::GENERIC;
        if (df.recursive)
            DeleteFileW(src.c_str());
    }

    const std::wstring current = ini.Get(L"Current", L"Snapshot");
    const std::wstring sec = kSnapPrefix + id;
    ini.Set(sec, L"Name", name);
    ini.Set(sec, L"SnapshotDate", std::to_wstring(NowUnixSeconds()));
    if (!current.empty())
        ini.Set(sec, L"Parent", current);
    ini.Set(L"Current", L"Snapshot", id);
    if (!SaveIniFile(box_.fileRoot + L"\\" + kIniName, ini))
        return SbieStatus::GENERIC;

    // 活动状态（根目录子目录）移入快照目录 —— 冻结
    for (const wchar_t* sub : kSubFolders) {
        if (!MoveEntry(box_.fileRoot + L"\\" + sub, snapDir + L"\\" + sub))
            return SbieStatus::GENERIC;
    }
    return SbieStatus::OK;
}

SbieStatus SnapshotManager::Remove(const std::wstring& id)
{
    if (box_.fileRoot.empty() || !ValidSnapshotId(id))
        return SbieStatus::NOT_FOUND;

    {
        std::vector<ULONG> pids;
        SbieStatus st = drv::EnumBoxProcesses(box_.name, false, &pids);
        if (st != SbieStatus::OK)
            return st;
        if (!pids.empty())
            return SbieStatus::BOX_BUSY;
    }

    IniFile ini;
    LoadIniFile(box_.fileRoot + L"\\" + kIniName, &ini);
    const std::wstring sec = kSnapPrefix + id;
    if (!ini.HasSection(sec))
        return SbieStatus::NOT_FOUND;

    // 子快照（Parent == id）
    std::vector<std::wstring> children;
    for (const IniFile::Section& s : ini.sections) {
        if (_wcsnicmp(s.name.c_str(), kSnapPrefix, 9) != 0)
            continue;
        if (_wcsicmp(s.Get(L"Parent").c_str(), id.c_str()) == 0)
            children.push_back(s.name.substr(9));
    }
    const std::wstring current = ini.Get(L"Current", L"Snapshot");
    const bool isCurrent = _wcsicmp(current.c_str(), id.c_str()) == 0;

    if (children.size() >= 2 || (children.size() == 1 && isCurrent))
        return SbieStatus::GENERIC;   // SB_SnapIsShared：共享父不可删

    const std::wstring targetDir = SnapDir(box_.fileRoot, id);
    SbieStatus st = SbieStatus::OK;

    if (children.size() == 1) {
        // 删除的是某子快照的父：子快照存活 —— 把子并入目标（子=较新，胜出），
        // 删除子目录，目标目录改名为子快照名，子继承目标的 Parent。
        const std::wstring child = children[0];
        const std::wstring childDir = SnapDir(box_.fileRoot, child);
        st = MergeCopyDir(childDir, targetDir);
        if (st == SbieStatus::OK)
            st = DeleteDirRecursive(childDir);
        if (st == SbieStatus::OK && DirExists(targetDir)) {
            if (!MoveFileExW(targetDir.c_str(), childDir.c_str(),
                             MOVEFILE_WRITE_THROUGH))
                st = SbieStatus::GENERIC;
        }
        if (st == SbieStatus::OK) {
            ini.Set(kSnapPrefix + child, L"Parent", ini.Get(sec, L"Parent"));
            ini.RemoveSection(sec);
        }
    } else if (isCurrent) {
        // 删除当前快照：先把活动根并入快照目录（活动状态胜出），再整体搬回
        // 根目录，Current/Snapshot 回退到被删快照的 Parent。
        std::wstring rootDat = box_.fileRoot + L"\\FilePaths.dat";
        std::wstring snapDat = targetDir + L"\\FilePaths.dat";
        if (FileExists(rootDat)) {
            // 增量日志按字节追加进目标（参考 MergeSnapshotAsync 的 dat 合并）
            HANDLE hSrc = CreateFileW(rootDat.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (hSrc != INVALID_HANDLE_VALUE) {
                ULONG sz = GetFileSize(hSrc, nullptr);
                if (sz > 0 && sz != 0xFFFFFFFF) {
                    std::vector<char> buf(sz);
                    DWORD rd = 0;
                    if (ReadFile(hSrc, buf.data(), sz, &rd, nullptr) && rd) {
                        HANDLE hDst = CreateFileW(
                            snapDat.c_str(), FILE_APPEND_DATA, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hDst != INVALID_HANDLE_VALUE) {
                            DWORD wr = 0;
                            WriteFile(hDst, buf.data(), rd, &wr, nullptr);
                            CloseHandle(hDst);
                        }
                    }
                }
                CloseHandle(hSrc);
            }
            DeleteFileW(rootDat.c_str());
        }
        for (const wchar_t* sub : kSubFolders) {
            std::wstring rootSub = box_.fileRoot + L"\\" + sub;
            std::wstring snapSub = targetDir + L"\\" + sub;
            st = MergeCopyDir(rootSub, snapSub);        // 活动 → 快照（活动胜）
            if (st != SbieStatus::OK)
                break;
            st = DeleteDirRecursive(rootSub);
            if (st != SbieStatus::OK)
                break;
            if (!MoveEntry(snapSub, rootSub)) {         // 合并后搬回根
                st = SbieStatus::GENERIC;
                break;
            }
        }
        if (st == SbieStatus::OK) {
            MoveDataFiles(targetDir, box_.fileRoot);
            // 清残留数据文件后删除快照目录（应已空）
            for (const DataFile& df : kDataFiles)
                DeleteFileW((targetDir + L"\\" + df.name).c_str());
            if (DirExists(targetDir) && !RemoveDirectoryW(targetDir.c_str()))
                st = SbieStatus::GENERIC;
        }
        if (st == SbieStatus::OK) {
            ini.Set(L"Current", L"Snapshot", ini.Get(sec, L"Parent"));
            ini.RemoveSection(sec);
        }
    } else {
        // 叶子快照且非当前：直接删目录 + 删节
        st = DeleteDirRecursive(targetDir);
        if (st == SbieStatus::OK)
            ini.RemoveSection(sec);
    }

    if (st == SbieStatus::OK) {
        if (!SaveIniFile(box_.fileRoot + L"\\" + kIniName, ini))
            return SbieStatus::GENERIC;
    }
    return st;
}

SbieStatus SnapshotManager::Select(const std::wstring& id)
{
    if (box_.fileRoot.empty())
        return SbieStatus::GENERIC;
    if (!id.empty() && !ValidSnapshotId(id))
        return SbieStatus::NOT_FOUND;

    {
        std::vector<ULONG> pids;
        SbieStatus st = drv::EnumBoxProcesses(box_.name, false, &pids);
        if (st != SbieStatus::OK)
            return st;
        if (!pids.empty())
            return SbieStatus::BOX_BUSY;
    }

    IniFile ini;
    LoadIniFile(box_.fileRoot + L"\\" + kIniName, &ini);
    if (!id.empty() && !ini.HasSection(kSnapPrefix + id))
        return SbieStatus::NOT_FOUND;

    // 根目录数据文件全部丢弃；从目标快照恢复非增量数据文件
    // （FilePaths.dat 为增量日志，不回放——参考 SelectSnapshot）
    for (const DataFile& df : kDataFiles)
        DeleteFileW((box_.fileRoot + L"\\" + df.name).c_str());
    if (!id.empty()) {
        const std::wstring snapDir = SnapDir(box_.fileRoot, id);
        for (const DataFile& df : kDataFiles) {
            if (df.recursive)
                continue;
            std::wstring src = snapDir + L"\\" + df.name;
            if (!FileExists(src)) {
                if (df.required)
                    return SbieStatus::GENERIC;   // SB_SnapCopyDatFail
                continue;
            }
            if (!CopyFileW(src.c_str(),
                           (box_.fileRoot + L"\\" + df.name).c_str(), FALSE))
                return SbieStatus::GENERIC;
        }
    }

    ini.Set(L"Current", L"Snapshot", id);
    if (!SaveIniFile(box_.fileRoot + L"\\" + kIniName, ini))
        return SbieStatus::GENERIC;

    // 丢弃活动状态（根子目录）；此后沙箱读取经 SbieDll 回退链落到快照目录
    for (const wchar_t* sub : kSubFolders) {
        SbieStatus st = DeleteDirRecursive(box_.fileRoot + L"\\" + sub);
        if (st != SbieStatus::OK)
            return st;
    }
    return SbieStatus::OK;
}

SbieStatus SnapshotManager::SetInfo(const std::wstring& id,
                                    std::optional<std::wstring> name,
                                    std::optional<std::wstring> info)
{
    if (box_.fileRoot.empty() || !ValidSnapshotId(id))
        return SbieStatus::NOT_FOUND;
    IniFile ini;
    LoadIniFile(box_.fileRoot + L"\\" + kIniName, &ini);
    const std::wstring sec = kSnapPrefix + id;
    if (!ini.HasSection(sec))
        return SbieStatus::NOT_FOUND;
    if (name.has_value())
        ini.Set(sec, L"Name", *name);
    if (info.has_value())
        ini.Set(sec, L"Description", *info);
    if (!SaveIniFile(box_.fileRoot + L"\\" + kIniName, ini))
        return SbieStatus::GENERIC;
    return SbieStatus::OK;
}

} // namespace sbie::model
