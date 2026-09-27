// Sandboxie-OSS — SbieCore/DriverApi/DriverApi.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（按 01-license-map §2 允许范围）：
//   - Sandboxie\core\dll\sbieapi.c（GPLv3）：包装函数的参数编排（只经 SbieDll
//     导出转发，不重拼 IOCTL）。
//   - SandboxiePlus\QSbieAPI\SbieAPI.cpp（LGPL-2.1，仅参考）：两段式
//     QueryBoxPath（:1603-1658）、EnumProcess count+pids（:1479-1517）、
//     IsBox（:2203-2217）、GetFeatureFlags（:2287-2311）。

#include "DriverApi.h"
#include "../Util/Ntdll.h"
#include "../Util/Version.h"

// vendor ABI 头（api_defs.h → defines_win32.h + api_flags.h：BOXNAME_COUNT、
// CONF_INDEX_MASK/CONF_GET_*、SBIE_CONF_FLAG_RECONFIGURE）
#include "api_defs.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <vector>

namespace sbie::drv {

namespace {

struct State {
    HMODULE dll = nullptr;
    Api api{};
    bool loaded = false;
    VersionInfo version;        // GetVersionEx 缓存
    std::wstring loadError;
    std::wstring loadedFrom;
    LONG lastNt = 0;
};

State& S()
{
    static State st;
    return st;
}

bool BindAll(HMODULE h, std::wstring* missing)
{
    Api& a = S().api;
    struct Bind {
        const char* name;
        FARPROC* slot;
    };
    // 注意：成员名与导出名一致，取址即得对应槽位
    Bind binds[] = {
        { "SbieApi_GetVersionEx",           (FARPROC*)&a.SbieApi_GetVersionEx },
        { "SbieApi_QueryDrvInfo",           (FARPROC*)&a.SbieApi_QueryDrvInfo },
        { "SbieApi_GetHomePath",            (FARPROC*)&a.SbieApi_GetHomePath },
        { "SbieApi_EnumBoxesEx",            (FARPROC*)&a.SbieApi_EnumBoxesEx },
        { "SbieApi_IsBoxEnabled",           (FARPROC*)&a.SbieApi_IsBoxEnabled },
        { "SbieApi_QueryBoxPath",           (FARPROC*)&a.SbieApi_QueryBoxPath },
        { "SbieApi_QueryProcessPath",       (FARPROC*)&a.SbieApi_QueryProcessPath },
        { "SbieApi_QueryProcess",           (FARPROC*)&a.SbieApi_QueryProcess },
        { "SbieApi_QueryProcessEx2",        (FARPROC*)&a.SbieApi_QueryProcessEx2 },
        { "SbieApi_QueryProcessInfo",       (FARPROC*)&a.SbieApi_QueryProcessInfo },
        { "SbieApi_QueryProcessInfoStr",    (FARPROC*)&a.SbieApi_QueryProcessInfoStr },
        { "SbieApi_EnumProcessEx",          (FARPROC*)&a.SbieApi_EnumProcessEx },
        { "SbieApi_OpenProcess",            (FARPROC*)&a.SbieApi_OpenProcess },
        { "SbieApi_QueryConf",              (FARPROC*)&a.SbieApi_QueryConf },
        { "SbieApi_QueryConfBool",          (FARPROC*)&a.SbieApi_QueryConfBool },
        { "SbieApi_QueryConfNumber",        (FARPROC*)&a.SbieApi_QueryConfNumber },
        { "SbieApi_QueryConfNumber64",      (FARPROC*)&a.SbieApi_QueryConfNumber64 },
        { "SbieApi_UpdateConf",             (FARPROC*)&a.SbieApi_UpdateConf },
        { "SbieApi_ReloadConf",             (FARPROC*)&a.SbieApi_ReloadConf },
        { "SbieApi_SessionLeader",          (FARPROC*)&a.SbieApi_SessionLeader },
        { "SbieApi_GetMessage",             (FARPROC*)&a.SbieApi_GetMessage },
        { "SbieApi_MonitorControl",         (FARPROC*)&a.SbieApi_MonitorControl },
        { "SbieApi_MonitorGetEx",           (FARPROC*)&a.SbieApi_MonitorGetEx },
        { "SbieApi_Call",                   (FARPROC*)&a.SbieApi_Call },
        { "SbieApi_Ioctl",                  (FARPROC*)&a.SbieApi_Ioctl },
        { "SbieDll_PortName",               (FARPROC*)&a.SbieDll_PortName },
        { "SbieDll_RunStartExe",            (FARPROC*)&a.SbieDll_RunStartExe },
        { "SbieDll_RunFromHome",            (FARPROC*)&a.SbieDll_RunFromHome },
        { "SbieDll_FormatMessage0",         (FARPROC*)&a.SbieDll_FormatMessage0 },
        { "SbieDll_FormatMessage1",         (FARPROC*)&a.SbieDll_FormatMessage1 },
        { "SbieDll_FormatMessage2",         (FARPROC*)&a.SbieDll_FormatMessage2 },
        { "SbieDll_FormatMessage",          (FARPROC*)&a.SbieDll_FormatMessage },
        { "SbieDll_TranslateNtToDosPath",   (FARPROC*)&a.SbieDll_TranslateNtToDosPath },
    };
    *missing = L"missing exports:";
    bool all = true;
    for (auto& b : binds) {
        FARPROC p = GetProcAddress(h, b.name);
        if (!p) {
            // 逐项报缺失符号名（02 §1：GetLastError()==127）
            wchar_t wname[128];
            size_t n = 0;
            for (; b.name[n] && n < 127; ++n)
                wname[n] = (wchar_t)b.name[n];
            wname[n] = L'\0';
            *missing += L" " + std::wstring(wname);
            all = false;
        } else {
            *b.slot = p;
        }
    }
    return all;
}

// 就地去掉最后一段（免 shlwapi 依赖：保持零附加导入库）
bool StripFileSpec(wchar_t* path)
{
    wchar_t* last = nullptr;
    for (wchar_t* p = path; *p; ++p)
        if (*p == L'\\' && p[1])
            last = p;
    if (!last)
        return false;
    *last = L'\0';
    return true;
}

// 注册表取 SbieSvc 服务 ImagePath → 目录（HKLM\SYSTEM\CCS\Services\SbieSvc）
std::wstring SbieSvcDirFromRegistry()
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\SbieSvc",
                      0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    LSTATUS rc = RegQueryValueExW(hk, L"ImagePath", nullptr, nullptr,
                                  (LPBYTE)buf, &cb);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || cb == 0)
        return L"";
    buf[cb / sizeof(wchar_t)] = L'\0';
    // ImagePath 形如 "C:\Program Files\Sandboxie-Plus\SbieSvc.exe"（可能带引号）
    wchar_t* s = buf;
    if (*s == L'"') {
        ++s;
        wchar_t* end = wcschr(s, L'"');
        if (end)
            *end = L'\0';
    }
    if (!StripFileSpec(s))
        return L"";
    return s;
}

bool TryLoadFrom(const std::wstring& file, std::wstring* why)
{
    // LOAD_WITH_ALTERED_SEARCH_PATH：依赖只从该显式路径解析（02 §1）
    HMODULE h = LoadLibraryExW(file.c_str(), nullptr,
                               LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        DWORD e = GetLastError();
        wchar_t buf[MAX_PATH * 4];
        swprintf_s(buf, L"LoadLibraryEx(%ls) failed win32=%lu", file.c_str(), e);
        *why = buf;
        return false;
    }
    std::wstring missing;
    if (!BindAll(h, &missing)) {
        FreeLibrary(h);
        *why = file + L": " + missing;
        return false;
    }
    S().dll = h;
    S().loadedFrom = file;
    S().loaded = true;
    // 立即取版本（失败时 GetVersionEx 写 L"unknown"/0，sbieapi.c:252-255）
    S().version = GetVersion();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 契约实现
// ---------------------------------------------------------------------------

bool LoadSbieDll(const std::wstring& explicitDir)
{
    if (S().loaded)
        return true;

    S().loadError.clear();
    std::vector<std::wstring> candidates;

    if (!explicitDir.empty())
        candidates.push_back(explicitDir + L"\\SbieDll.dll");

    // 1) 本 exe 同目录（sbie-cli.exe 与 SbieDll.dll 同目录分发）
    {
        wchar_t self[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, self, (DWORD)std::size(self));
        if (n > 0 && n < std::size(self)) {
            if (StripFileSpec(self))
                candidates.push_back(std::wstring(self) + L"\\SbieDll.dll");
        }
    }

    // 2) 标准安装目录（含 32 位视图退化情形；Sandboxie-Plus 仅有 x64 安装）
    {
        wchar_t pf[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, (DWORD)std::size(pf));
        if (n > 0 && n < std::size(pf))
            candidates.push_back(std::wstring(pf) + L"\\Sandboxie-Plus\\SbieDll.dll");
        candidates.push_back(L"C:\\Program Files\\Sandboxie-Plus\\SbieDll.dll");
    }

    // 3) 注册表 Sandboxie 安装位置（经 SbieSvc 服务 ImagePath）
    {
        std::wstring dir = SbieSvcDirFromRegistry();
        if (!dir.empty())
            candidates.push_back(dir + L"\\SbieDll.dll");
    }

    // 4) 默认搜索（PATH 等）
    candidates.push_back(L"SbieDll.dll");

    std::wstring why;
    for (auto& c : candidates) {
        if (TryLoadFrom(c, &why))
            return true;
        S().loadError = why;   // 保留最后一次失败原因
    }
    return false;
}

bool Loaded()
{
    return S().loaded;
}

VersionInfo GetVersion()
{
    if (S().loaded && S().api.SbieApi_GetVersionEx) {
        WCHAR ver[16] = L"";
        ULONG abi = 0;
        S().api.SbieApi_GetVersionEx(ver, &abi);
        return VersionInfo{ ver, abi };
    }
    return VersionInfo{ L"unknown", 0 };
}

Api* ApiP()
{
    return &S().api;
}

bool InSandbox()
{
    if (!Loaded() && !LoadSbieDll())
        return false; // SbieDll 不可得时无法判定，按"不在沙箱内"处理（调用方
                      // 会因 ERR_SBIEDLL 拒绝一切操作，等效安全）
    LONG rc = S().api.SbieApi_QueryProcess((HANDLE)(ULONG_PTR)GetCurrentProcessId(),
                                           nullptr, nullptr, nullptr, nullptr);
    return rc >= 0; // STATUS_SUCCESS ⇒ 自身在沙箱内（02 §6）
}

bool DriverAlive()
{
    // 尝试打开 \Device\SandboxieDriverApi（FILE_GENERIC_READ，share RWD——
    // sbieapi.c:99-189 的打开语义；失败=未装/未跑）
    nt::NtdllApi* nt = nt::Get();
    if (!nt || !nt->NtOpenFile)
        return false;

    _SBIE_UNICODE_STRING name;
    name.Buffer = (PWSTR)L"\\Device\\SandboxieDriverApi";
    name.Length = (USHORT)(wcslen(name.Buffer) * sizeof(WCHAR));
    name.MaximumLength = name.Length + sizeof(WCHAR);

    nt::SBIE_OBJECT_ATTRIBUTES oa;
    nt::InitObjectAttributes(&oa, &name, 0 /*无 OBJ_CASE_INSENSITIVE 需要*/,
                             nullptr, nullptr);
    nt::SBIE_IO_STATUS_BLOCK iosb{};
    HANDLE h = nullptr;
    // DesiredAccess=FILE_GENERIC_READ(0x120089)；Share=READ|WRITE|DELETE(7)
    const ULONG kFileGenericRead = 0x120089;
    LONG rc = nt->NtOpenFile(&h, kFileGenericRead, &oa, &iosb,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             0 /*非目录，无需 FILE_DIRECTORY_FILE*/);
    if (rc >= 0 && h) {
        nt->NtClose(h);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 薄封装
// ---------------------------------------------------------------------------

LONG LastNtStatus()
{
    return S().lastNt;
}

SbieStatus QueryDriverInfo(ULONG infoClass, void* data, ULONG size)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    LONG rc = S().api.SbieApi_QueryDrvInfo(infoClass, data, size);
    S().lastNt = rc;
    if (rc < 0)
        memset(data, 0, size); // QSbieAPI GetDriverInfo 同款清零行为
    return FromNtStatus(rc);
}

SbieStatus EnumBoxes(std::vector<std::wstring>* boxes, bool allSections)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    boxes->clear();
    LONG index = -1;
    int guard = 0;
    while (guard++ < 4096) {
        WCHAR name[BOXNAME_COUNT] = L"";
        index = S().api.SbieApi_EnumBoxesEx(index, name, allSections ? TRUE : FALSE);
        if (index == -1 || name[0] == L'\0')
            break; // 迭代结束（返回 -1）
        boxes->push_back(name);
    }
    return SbieStatus::OK;
}

SbieStatus IsBoxEnabled(const std::wstring& box, bool* enabled, bool* exists)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (box.empty() || box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    LONG rc = S().api.SbieApi_IsBoxEnabled(box.c_str());
    S().lastNt = rc;
    if (rc >= 0) {
        *enabled = true;
        *exists = true;
        return SbieStatus::OK;
    }
    if ((unsigned long)rc == 0xC000006EL /*STATUS_ACCOUNT_RESTRICTION*/) {
        *enabled = false;
        *exists = true;   // QSbieAPI IsBox 约定（SbieAPI.cpp:2203-2217）
        return SbieStatus::OK;
    }
    *enabled = false;
    *exists = false;
    return SbieStatus::OK;
}

SbieStatus QueryBoxPath(const std::wstring& box,
                        std::wstring* fileRoot, std::wstring* regRoot,
                        std::wstring* ipcRoot)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (box.empty() || box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;

    // 两段式：先取字节长度（QSbieAPI UpdateBoxPaths，SbieAPI.cpp:1603-1658）
    ULONG fl = 0, kl = 0, il = 0;
    LONG rc = S().api.SbieApi_QueryBoxPath(box.c_str(), nullptr, nullptr, nullptr,
                                           &fl, &kl, &il);
    S().lastNt = rc;
    if (rc < 0)
        return FromNtStatus(rc);

    std::vector<WCHAR> f(fl / sizeof(WCHAR) + 1, L'\0');
    std::vector<WCHAR> k(kl / sizeof(WCHAR) + 1, L'\0');
    std::vector<WCHAR> i(il / sizeof(WCHAR) + 1, L'\0');
    rc = S().api.SbieApi_QueryBoxPath(
        box.c_str(),
        fileRoot ? f.data() : nullptr,
        regRoot  ? k.data() : nullptr,
        ipcRoot  ? i.data() : nullptr,
        &fl, &kl, &il);
    S().lastNt = rc;
    if (rc < 0)
        return FromNtStatus(rc);
    if (fileRoot) fileRoot->assign(f.data());
    if (regRoot)  regRoot->assign(k.data());
    if (ipcRoot)  ipcRoot->assign(i.data());
    return SbieStatus::OK;
}

SbieStatus EnumBoxProcesses(const std::wstring& box, bool allSessions,
                            std::vector<ULONG>* pids)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    pids->clear();

    const WCHAR* boxName = box.empty() ? nullptr : box.c_str();
    ULONG count = 0;
    LONG rc = S().api.SbieApi_EnumProcessEx(boxName, allSessions ? TRUE : FALSE,
                                            (ULONG)-1, nullptr, &count);
    S().lastNt = rc;
    if (rc < 0)
        return FromNtStatus(rc);

    std::vector<ULONG> buf((size_t)count + 128, 0); // +128 余量（QSbieAPI 同款）
    rc = S().api.SbieApi_EnumProcessEx(boxName, allSessions ? TRUE : FALSE,
                                       (ULONG)-1, buf.data(), &count);
    S().lastNt = rc;
    if (rc < 0)
        return FromNtStatus(rc);
    pids->assign(buf.begin(), buf.begin() + count);
    return SbieStatus::OK;
}

SbieStatus QueryProcessById(ULONG pid, ProcQuery* out)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    WCHAR box[BOXNAME_COUNT] = L"";
    WCHAR image[MAX_PATH] = L"";
    WCHAR sid[96] = L"";
    ULONG session = 0;
    ULONG64 created = 0;
    LONG rc = S().api.SbieApi_QueryProcessEx2((HANDLE)(ULONG_PTR)pid,
                                              (ULONG)std::size(image),
                                              box, image, sid, &session, &created);
    S().lastNt = rc;
    if (rc < 0) {
        // 失败清零语义由 SbieDll 完成（sbieapi.c:613-632）
        out->box.clear();
        out->image.clear();
        out->sid.clear();
        out->sessionId = 0;
        out->createTime = 0;
        return FromNtStatus(rc);
    }
    out->box = box;
    out->image = image;
    out->sid = sid;
    out->sessionId = session;
    out->createTime = created;
    return SbieStatus::OK;
}

SbieStatus QueryConfText(const std::wstring& section, const std::wstring& setting,
                         ULONG index, bool noExpand, bool noTemplates,
                         std::wstring* value)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    if (section.size() > 64 || setting.size() > 64)
        return SbieStatus::INVALID; // x_section[66] 截断防御
    ULONG flags = index & CONF_INDEX_MASK;
    if (noExpand)   flags |= CONF_GET_NO_EXPAND;
    if (noTemplates)flags |= CONF_GET_NO_TEMPLS;
    WCHAR buf[CONF_LINE_LEN];
    buf[0] = L'\0';
    // 注意：nullptr 仅用于"枚举节"（两者都空）；此处恒传字符串（空 section
    // 对驱动而言即 GlobalSettings 缺省语义，见 conf.c Conf_Api_Query）
    LONG rc = S().api.SbieApi_QueryConf(section.c_str(), setting.c_str(),
                                        flags, buf, (ULONG)sizeof(buf));
    S().lastNt = rc;
    value->assign(buf);
    if (rc < 0)
        return FromNtStatus(rc);
    return SbieStatus::OK;
}

SbieStatus QueryConfList(const std::wstring& section, const std::wstring& setting,
                         bool noExpand, bool noTemplates,
                         std::vector<std::wstring>* values)
{
    values->clear();
    for (ULONG i = 0; i < 512 /*防病态配置*/; ++i) {
        std::wstring v;
        SbieStatus st = QueryConfText(section, setting, i, noExpand, noTemplates, &v);
        if (st != SbieStatus::OK)
            return st == SbieStatus::NOT_FOUND ? SbieStatus::OK : st;
        if (v.empty())
            break; // 越过末尾（驱动返回空串）
        values->push_back(std::move(v));
    }
    return SbieStatus::OK;
}

SbieStatus EnumConfSections(std::vector<std::wstring>* sections)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    sections->clear();
    for (LONG i = 0; i < 4096; ++i) {
        WCHAR buf[BOXNAME_COUNT] = L"";
        // 枚举节：section=setting=NULL（SbieAPI.cpp:1180-1182）
        LONG rc = S().api.SbieApi_QueryConf(
            nullptr, nullptr,
            (ULONG)i | CONF_GET_NO_TEMPLS | CONF_GET_NO_EXPAND,
            buf, (ULONG)sizeof(buf));
        S().lastNt = rc;
        if (buf[0] == L'\0')
            break;
        sections->push_back(buf);
    }
    return SbieStatus::OK;
}

SbieStatus ReloadConf(unsigned long flags, bool reconfigure)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    LONG rc = S().api.SbieApi_ReloadConf((ULONG)-1,
                                         flags | (reconfigure ? SBIE_CONF_FLAG_RECONFIGURE : 0));
    S().lastNt = rc;
    return FromNtStatus(rc);
}

SbieStatus DisableForceProcess(ULONG* newState, ULONG* oldState)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    // 参数编排对齐 sbieapi.c SbieApi_DisableForceProcess（:916-940）：
    // parms[0]=func_code，其后为 set_flag/get_flag 指针（API_ARGS_FIELD 展开为
    // union{ULONG64 val64; T val;}，写 .val 即 64 位槽位）
    __declspec(align(8)) ULONG64 parms[API_NUM_ARGS];
    API_DISABLE_FORCE_PROCESS_ARGS* args =
        (API_DISABLE_FORCE_PROCESS_ARGS*)parms;
    memset(parms, 0, sizeof(parms));
    args->func_code     = API_DISABLE_FORCE_PROCESS;
    args->set_flag.val  = newState;
    args->get_flag.val  = oldState;
    LONG rc = S().api.SbieApi_Ioctl(parms);
    S().lastNt = rc;
    if (rc < 0 && oldState)
        *oldState = FALSE;   // 失败清 FALSE（sbieapi.c 同款）
    return FromNtStatus(rc);
}

// ---------------------------------------------------------------------------
// 监控/trace（波 D1；02 §3.6/§7 坑 1——MONITOR_GET2 无 SbieDll 导出，Ioctl 直投）
// ---------------------------------------------------------------------------

SbieStatus MonitorControl(ULONG* newState, ULONG* oldState)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    // 参数编排对齐 QSbieAPI CSbieAPI__MonitorControl（SbieAPI.cpp:2936-2952）
    __declspec(align(8)) ULONG64 parms[API_NUM_ARGS];
    API_MONITOR_CONTROL_ARGS* args = (API_MONITOR_CONTROL_ARGS*)parms;
    memset(parms, 0, sizeof(parms));
    args->func_code   = API_MONITOR_CONTROL;
    args->set_flag.val = newState;
    args->get_flag.val = oldState;
    LONG rc = S().api.SbieApi_Ioctl(parms);
    S().lastNt = rc;
    return FromNtStatus(rc);
}

MonitorFetch MonitorGet2(void* buffer8Aligned, ULONG* bufferLen, bool* moreEntries)
{
    if (moreEntries)
        *moreEntries = false;
    if (!Loaded() && !LoadSbieDll())
        return MonitorFetch::Error;
    if (!buffer8Aligned || !bufferLen || *bufferLen == 0)
        return MonitorFetch::Error;

    __declspec(align(8)) ULONG64 parms[API_NUM_ARGS];
    API_MONITOR_GET2_ARGS* args = (API_MONITOR_GET2_ARGS*)parms;
    memset(parms, 0, sizeof(parms));
    args->func_code    = API_MONITOR_GET2;
    args->buffer_ptr.val = (WCHAR*)buffer8Aligned;
    args->buffer_len.val = bufferLen;
    LONG rc = S().api.SbieApi_Ioctl(parms);
    S().lastNt = rc;
    // 驱动端在入口即清零 *buffer_len，任何失败路径下均无部分写入可解析
    if (rc == 0 || rc == 0x00000105L /*STATUS_MORE_ENTRIES*/) {
        if (moreEntries)
            *moreEntries = (rc == 0x00000105L);
        return MonitorFetch::Ok;
    }
    if ((unsigned long)rc == 0x8000001AUL /*STATUS_NO_MORE_ENTRIES*/)
        return MonitorFetch::Empty;
    if ((unsigned long)rc == 0xC00000A3UL /*STATUS_DEVICE_NOT_READY*/)
        return MonitorFetch::NotEnabled;
    return MonitorFetch::Error;
}

SbieStatus GetHomePath(std::wstring* ntPath, std::wstring* dosPath)
{
    if (!Loaded() && !LoadSbieDll())
        return SbieStatus::ERR_SBIEDLL;
    WCHAR nt[512] = L"", dos[512] = L"";
    LONG rc = S().api.SbieApi_GetHomePath(nt, 512, dos, 512);
    S().lastNt = rc;
    if (rc < 0)
        return FromNtStatus(rc);
    if (ntPath) ntPath->assign(nt);
    if (dosPath) dosPath->assign(dos);
    return SbieStatus::OK;
}

// ---------------------------------------------------------------------------
// 诊断辅助
// ---------------------------------------------------------------------------

std::wstring LastLoadError()
{
    return S().loadError;
}

std::wstring LoadedFrom()
{
    return S().loadedFrom;
}

bool AbiMatches()
{
    return S().loaded && S().version.abi == kExpectedAbi;
}

} // namespace sbie::drv
