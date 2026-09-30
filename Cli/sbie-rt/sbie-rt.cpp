// Sandboxie-OSS — Cli/sbie-rt/sbie-rt.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// sbie-rt — unified Sandboxie-OSS command line (V2/V3 merged).
// Spec: docs/10-v2-design.md + docs/12-dynamic-arch.md
//
// One binary, one box model (dynamic), two config sources:
//   direct mode     sbie-rt create <config.kv>          -> SbieBoxCreate
//                  sbie-rt exec <handle> <exe> [...]    -> SbieBoxExec
//                  sbie-rt destroy <handle>             -> SbieBoxDestroy
//                  sbie-rt drv / query <box> <setting>
//   directory mode  sbie-rt exec ./testbox [cmdline]    -> V2 ImportBox flow
//                  register/unregister/sync-config/ps/kill-box/kill/
//                  log/info/create-box/create-encbox    (V2 command face)
//
// Talks to \\Device\\SandboxieDriverApi directly through ntdll
// (NtOpenFile + NtDeviceIoControlFile); no SbieDll.dll dependency,
// so the dynamic-box path works in a bare SbieRT / ValidationOS
// deployment.
//
// The config blob handed to "create" is fully self-contained
// (config-to-kernel): each box carries its own skeleton sections
// ([TemplateDefaultPaths] / [TemplateNetworkPaths]) plus [BoxConfig];
// the driver embeds no defaults.  The presets in
// CoreRT\sdk\examples\*.kv show the box shapes.  All three dynamic-box
// IOCTLs require an elevated-admin caller (UAC-aware check in the
// driver).
//
// Internal (non user-command) entries kept from the V2 entry:
//   --version / --monitor [...] / --set-password / --ini-del
//
// Driver ABI constants below mirror CoreRT/drv/api_defs.h and
// MUST be kept in sync with it (values are frozen by the enum order).

#include "commands/Cli.h"
#include "commands/Commands.h"
#include "monitor/MonitorMain.h"
#include "DriverApi/DriverApi.h"
#include "SvcClient/SvcClient.h"
#include "Util/Status.h"
#include "Util/Utf8.h"
#include "Util/Version.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Driver ABI (mirror of api_defs.h)
// ---------------------------------------------------------------------------

static const uint32_t kApiFirst = 0x12340000;

#define API_GET_VERSION     (kApiFirst + 0x01)
#define API_QUERY_CONF      (kApiFirst + 0x0F)
#define API_BOX_CREATE      (kApiFirst + 0x50)   // dynamic-box-arch
#define API_BOX_EXEC        (kApiFirst + 0x51)
#define API_BOX_DESTROY     (kApiFirst + 0x52)

#define API_NUM_ARGS        8
#define BOXNAME_COUNT      40

// CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, 0x801, METHOD_NEITHER=3, FILE_ANY_ACCESS)
static const uint32_t kSbieDrvCtlCode = 0x00222007;

struct UnicodeString64 {
    uint16_t Length;
    uint16_t MaximumLength;
    uint32_t _pad;
    uint64_t Buffer;
};
static_assert(sizeof(UnicodeString64) == 16, "x64 UNICODE_STRING64 layout");

struct NtUnicodeString {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t* Buffer;
};

struct NtIoStatusBlock {
    union { uint32_t Status; void* Pointer; };
    uintptr_t Information;
};

struct NtObjectAttributes {
    uint32_t Length;
    void* RootDirectory;
    NtUnicodeString* ObjectName;
    uint32_t Attributes;
    void* SecurityDescriptor;
    void* SecurityQualityOfService;
};

// ---------------------------------------------------------------------------
// ntdll binding
// ---------------------------------------------------------------------------

typedef int32_t (WINAPI* P_NtOpenFile)(void**, uint32_t, NtObjectAttributes*,
                                       NtIoStatusBlock*, uint32_t, uint32_t);
typedef int32_t (WINAPI* P_NtDeviceIoControlFile)(void*, void*, void*, void*,
        NtIoStatusBlock*, uint32_t, void*, uint32_t, void*, uint32_t);

static P_NtOpenFile              pNtOpenFile;
static P_NtDeviceIoControlFile   pNtDeviceIoControlFile;

static bool BindNtdll()
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) nt = LoadLibraryW(L"ntdll.dll");
    if (!nt) return false;
    pNtOpenFile = (P_NtOpenFile)GetProcAddress(nt, "NtOpenFile");
    pNtDeviceIoControlFile =
        (P_NtDeviceIoControlFile)GetProcAddress(nt, "NtDeviceIoControlFile");
    return pNtOpenFile && pNtDeviceIoControlFile;
}

static void* g_device = INVALID_HANDLE_VALUE;

static uint32_t OpenDevice()
{
    if (g_device != INVALID_HANDLE_VALUE)
        return 0;

    NtUnicodeString name;
    name.Buffer = (wchar_t*)L"\\Device\\" L"SandboxieDriverApi";
    name.Length = (uint16_t)(wcslen(name.Buffer) * sizeof(wchar_t));
    name.MaximumLength = name.Length + sizeof(wchar_t);

    NtObjectAttributes oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &name;
    oa.Attributes = 0x40 /* OBJ_CASE_INSENSITIVE */;

    NtIoStatusBlock iosb;
    uint32_t status = pNtOpenFile(&g_device,
        0x00120089 /* FILE_GENERIC_READ */,
        &oa, &iosb,
        7 /* FILE_SHARE_READ|WRITE|DELETE */,
        0x20 /* FILE_NON_DIRECTORY_FILE */ | 0 /* SYNCHRONIZE absent: sync via wait */);
    if (status)
        g_device = INVALID_HANDLE_VALUE;
    return status;
}

// ---------------------------------------------------------------------------
// SbieApi_Ioctl equivalent
// ---------------------------------------------------------------------------

static uint32_t ApiCall(uint64_t (&parms)[API_NUM_ARGS])
{
    uint32_t status = OpenDevice();
    if (status)
        return status;

    NtIoStatusBlock iosb;
    status = pNtDeviceIoControlFile(g_device, NULL, NULL, NULL, &iosb,
                                    kSbieDrvCtlCode,
                                    parms, sizeof(uint64_t) * API_NUM_ARGS,
                                    NULL, 0);
    return status;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void PrintNtError(const char* what, uint32_t status)
{
    fprintf(stderr, "sbie-rt: %s failed: NTSTATUS 0x%08X\n", what, status);
}

static bool ReadFileToWide(const wchar_t* path, std::wstring* out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "sbie-rt: cannot open %ls (%lu)\n", path, GetLastError());
        return false;
    }
    char buf[64 * 1024 + 8];
    DWORD n = 0;
    if (!ReadFile(f, buf, sizeof(buf) - 8, &n, NULL) || n == 0) {
        fprintf(stderr, "sbie-rt: cannot read %ls\n", path);
        CloseHandle(f);
        return false;
    }
    CloseHandle(f);

    if (n >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        // UTF-16 LE with BOM
        out->assign((const wchar_t*)(buf + 2), (n - 2) / sizeof(wchar_t));
    } else {
        int cp = (n >= 3 && (unsigned char)buf[0] == 0xEF
                       && (unsigned char)buf[1] == 0xBB
                       && (unsigned char)buf[2] == 0xBF) ? CP_UTF8 : CP_ACP;
        int skip = (cp == CP_UTF8) ? 3 : 0;
        int wlen = MultiByteToWideChar(cp, 0, buf + skip, n - skip, NULL, 0);
        out->resize(wlen);
        MultiByteToWideChar(cp, 0, buf + skip, n - skip, &(*out)[0], wlen);
    }
    // normalize to \n line endings, ensure exactly one trailing newline
    std::wstring tmp;
    tmp.reserve(out->size() + 2);
    for (size_t i = 0; i < out->size(); ++i) {
        wchar_t c = (*out)[i];
        if (c == L'\r') continue;
        tmp.push_back(c);
    }
    while (!tmp.empty() && tmp[tmp.size() - 1] == L'\n')
        tmp.pop_back();
    if (tmp.empty()) {
        fprintf(stderr, "sbie-rt: config file is empty\n");
        return false;
    }
    tmp.push_back(L'\n');
    *out = tmp;
    return true;
}

static uint64_t ParseHandle(const wchar_t* s)
{
    if (_wcsnicmp(s, L"0x", 2) == 0)
        return _wcstoui64(s + 2, NULL, 16);
    return _wcstoui64(s, NULL, 10);
}

// ---------------------------------------------------------------------------
// SbieSvc runtime dependency (dynamic-box-arch injection chain)
// ---------------------------------------------------------------------------
//
// API_BOX_EXEC claims a suspended process through Process_NotifyProcess_
// Create, and Process_Low_Inject asks the SbieSvc DriverAssist port to
// inject SbieLow (embedded in SbieSvc.exe as the LOWLEVEL64/LOWLEVEL32
// resources); SbieLow then loads SbieDll.dll from the driver home path
// (the SbieDrv.sys ImagePath directory).  Without a running SbieSvc the
// boxed process dies on resume, so exec makes sure the service is up:
//
//      1. already running (any SbieSvc.exe process)      -> proceed
//      2. SCM route: StartService("SbieSvc")             -> normal hosts
//      3. detached CreateProcess of <exe dir>\SbieSvc.exe-> bare hosts
//         without the service registered (note: stock SbieSvc.exe calls
//         StartServiceCtrlDispatcher and exits if no SCM exists at all)
//

#include <tlhelp32.h>

static bool IsProcessRunning(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);

    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool StartSbieSvcViaScm()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    SC_HANDLE svc = OpenServiceW(scm, L"SbieSvc",
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    BOOL ok = StartServiceW(svc, 0, NULL);
    if (!ok && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    // wait for SERVICE_RUNNING, max ~10s
    for (int i = 0; i < 100; ++i) {
        SERVICE_STATUS st;
        memset(&st, 0, sizeof(st));
        if (QueryServiceStatus(svc, &st) &&
                st.dwCurrentState == SERVICE_RUNNING)
            break;
        Sleep(100);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    return IsProcessRunning(L"SbieSvc.exe");
}

static bool StartSbieSvcDetached()
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return false;

    wchar_t* slash = wcsrchr(self, L'\\');
    if (!slash)
        return false;
    if (!wcscpy_s(slash + 1, self + MAX_PATH - (slash + 1), L"SbieSvc.exe"))
        return false;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(self, NULL, NULL, NULL, FALSE,
                        DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "sbie-rt: CreateProcess(%ls) failed %lu\n",
                self, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // give it a moment to come up (and to fail fast if SCM rejects it)
    for (int i = 0; i < 50 && !IsProcessRunning(L"SbieSvc.exe"); ++i)
        Sleep(100);

    return IsProcessRunning(L"SbieSvc.exe");
}

static void EnsureSbieSvcRunning()
{
    if (IsProcessRunning(L"SbieSvc.exe"))
        return;

    fprintf(stderr, "sbie-rt: SbieSvc not running, starting it\n");

    if (StartSbieSvcViaScm()) {
        fprintf(stderr, "sbie-rt: SbieSvc started via SCM\n");
        return;
    }
    if (StartSbieSvcDetached()) {
        fprintf(stderr, "sbie-rt: SbieSvc launched detached\n");
        return;
    }

    fprintf(stderr,
            "sbie-rt: WARNING - could not start SbieSvc; exec will claim "
            "the process but SbieLow injection will fail\n");
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

static int CmdDriver()
{
    uint32_t status = OpenDevice();
    if (status) {
        fprintf(stderr, "sbie-rt: driver not reachable (NtOpenFile 0x%08X)\n",
                status);
        return 1;
    }

    wchar_t ver[16] = L"";
    uint32_t abi = 0;
    uint64_t parms[API_NUM_ARGS] = {0};
    parms[0] = API_GET_VERSION;
    parms[1] = (uint64_t)(uintptr_t)ver;
    parms[2] = (uint64_t)(uintptr_t)&abi;
    ver[0] = L'\0';
    status = ApiCall(parms);
    if (status) {
        PrintNtError("API_GET_VERSION", status);
        return 1;
    }
    printf("driver %ls (abi 0x%X)\n", ver, abi);

    // probe the dynamic API with an obviously invalid handle:  a driver
    // that has the handler answers STATUS_INVALID_HANDLE (or
    // ACCESS_DENIED); an old driver answers STATUS_INVALID_DEVICE_REQUEST
    memset(parms, 0, sizeof(parms));
    parms[0] = API_BOX_DESTROY;
    parms[1] = 0x7FFFFFFF7FFFFFFFULL;
    parms[2] = 0;
    status = ApiCall(parms);
    if (status == 0xC0000008 /* STATUS_INVALID_HANDLE */ ||
        status == 0xC0000022 /* STATUS_ACCESS_DENIED */)
        printf("dynamic-box API: present\n");
    else if (status == 0xC0000010 /* STATUS_INVALID_DEVICE_REQUEST */)
        printf("dynamic-box API: ABSENT (old driver loaded)\n");
    else
        printf("dynamic-box API: probe returned 0x%08X\n", status);
    return 0;
}

static int CmdCreate(const std::vector<std::wstring>& args)
{
    if (args.size() != 3) {
        fprintf(stderr, "usage: sbie-rt create <config.kv>\n");
        return 2;
    }

    std::wstring cfg;
    if (!ReadFileToWide(args[2].c_str(), &cfg))
        return 1;

    // the driver reads Length + one WCHAR; keep the buffer one WCHAR larger
    std::vector<wchar_t> buf(cfg.begin(), cfg.end());
    buf.push_back(L'\0');
    buf.push_back(L'\0');

    UnicodeString64 uni;
    uni.Length = (uint16_t)(cfg.size() * sizeof(wchar_t));
    uni.MaximumLength = uni.Length;
    uni.Buffer = (uint64_t)(uintptr_t)&buf[0];

    uint64_t handle = 0;
    wchar_t boxname[BOXNAME_COUNT] = L"";

    uint64_t parms[API_NUM_ARGS] = {0};
    parms[0] = API_BOX_CREATE;
    parms[1] = (uint64_t)(uintptr_t)&uni;
    parms[2] = (uint64_t)(uintptr_t)&handle;
    parms[3] = (uint64_t)(uintptr_t)boxname;

    uint32_t status = ApiCall(parms);
    if (status) {
        PrintNtError("API_BOX_CREATE", status);
        return 1;
    }

    printf("handle 0x%llX\nbox %ls\n",
           (unsigned long long)handle, boxname);
    return 0;
}

static int CmdExec(const std::vector<std::wstring>& args)
{
    if (args.size() < 4) {
        fprintf(stderr,
                "usage: sbie-rt exec <handle> <exe> [args...]\n");
        return 2;
    }

    uint64_t handle = ParseHandle(args[2].c_str());

    //
    // the injection chain needs SbieSvc (DriverAssist -> SbieLow ->
    // SbieDll); make sure it is up before the target process exists
    //

    EnsureSbieSvcRunning();

    std::wstring cmd = L"\"" + args[3] + L"\"";
    for (size_t i = 4; i < args.size(); ++i)
        cmd += L" " + args[i];

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "sbie-rt: CreateProcess(%ls) failed %lu\n",
                args[3].c_str(), GetLastError());
        return 1;
    }

    uint64_t outPid = 0;
    uint64_t parms[API_NUM_ARGS] = {0};
    parms[0] = API_BOX_EXEC;
    parms[1] = handle;
    parms[2] = pi.dwProcessId;
    parms[3] = 0;                       // fake_admin = FALSE
    parms[4] = (uint64_t)(uintptr_t)&outPid;

    uint32_t status = ApiCall(parms);

    if (status) {
        PrintNtError("API_BOX_EXEC", status);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("claimed pid %lu into dynamic box\n", pi.dwProcessId);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    printf("exit code %lu\n", exitCode);
    return (int)exitCode;
}

static int CmdDestroy(const std::vector<std::wstring>& args)
{
    if (args.size() < 3) {
        fprintf(stderr, "usage: sbie-rt destroy <handle>\n");
        return 2;
    }

    uint64_t handle = ParseHandle(args[2].c_str());

    uint64_t parms[API_NUM_ARGS] = {0};
    parms[0] = API_BOX_DESTROY;
    parms[1] = handle;
    parms[2] = 1;                       // kill boxed processes first

    uint32_t status = ApiCall(parms);
    if (status) {
        PrintNtError("API_BOX_DESTROY", status);
        return 1;
    }

    printf("destroyed\n");
    return 0;
}

static int CmdQuery(const std::vector<std::wstring>& args)
{
    if (args.size() != 4) {
        fprintf(stderr, "usage: sbie-rt query <boxname> <setting>\n");
        return 2;
    }

    // API_QUERY_CONF: parms[1]=section [2]=setting [3]=&index [4]=out uni [5]=&flags
    wchar_t value[2000];
    uint32_t index = 0;
    uint32_t flags = 0;
    UnicodeString64 uni;
    uni.Length = 0;
    uni.MaximumLength = sizeof(value) - sizeof(wchar_t);
    uni.Buffer = (uint64_t)(uintptr_t)value;

    uint64_t parms[API_NUM_ARGS] = {0};
    parms[0] = API_QUERY_CONF;
    parms[1] = (uint64_t)(uintptr_t)args[2].c_str();
    parms[2] = (uint64_t)(uintptr_t)args[3].c_str();
    parms[3] = (uint64_t)(uintptr_t)&index;
    parms[4] = (uint64_t)(uintptr_t)&uni;
    parms[5] = (uint64_t)(uintptr_t)&flags;

    uint32_t status = ApiCall(parms);
    if (status == 0xC000008B /* STATUS_RESOURCE_NAME_NOT_FOUND */ ||
        status == 0xC0000034 /* STATUS_OBJECT_NAME_NOT_FOUND   */) {
        printf("<unset>\n");
        return 0;
    }
    if (status) {
        PrintNtError("API_QUERY_CONF", status);
        return 1;
    }
    value[uni.Length / sizeof(wchar_t)] = L'\0';
    printf("%ls\n", value);
    return 0;
}

// ---------------------------------------------------------------------------
// unified entry: dynamic-box commands + V2 command face + internal entries
// ---------------------------------------------------------------------------

// sbie-rt <ver> / driver <ver> (abi 0x…, alive)
static int PrintVersion()
{
    std::wstring line = L"sbie-rt " + std::wstring(sbie::kCliVersionW);
    if (sbie::drv::LoadSbieDll()) {
        sbie::drv::VersionInfo v = sbie::drv::GetVersion();
        wchar_t drvAbi[16];
        swprintf_s(drvAbi, L"%X", v.abi);
        line += L"\ndriver " + v.version + L" (abi 0x" + drvAbi + L", "
                + (sbie::drv::DriverAlive() ? L"alive" : L"absent") + L")";
    }
    sbie::util::PrintLineUtf8(sbie::util::WideToUtf8(line));
    return 0;
}

// "0x1A2B" / "123456" -> handle token; "testbox" / ".\box" -> not
static bool IsHandleToken(const wchar_t* s)
{
    if (!s || !*s)
        return false;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
        s += 2;
    if (!*s)
        return false;
    for (const wchar_t* p = s; *p; ++p)
        if (!iswdigit(*p))
            return false;
    return true;
}

static void Usage()
{
    fprintf(stderr,
        "sbie-rt - unified Sandboxie-OSS command line (dynamic box model)\n"
        "\n"
        "  sbie-rt drv                        probe driver + dynamic API\n"
        "  sbie-rt create <config.kv>         create dynamic box from KV file\n"
        "  sbie-rt exec <handle> <exe> [...]  run exe in the dynamic box\n"
        "  sbie-rt destroy <handle>           destroy the dynamic box\n"
        "  sbie-rt query <box> <setting>      read a setting from the box section\n"
        "\n"
        "  sbie-rt exec <dir|*alias> [cmdline]\n"
        "                                     directory mode: register + run\n"
        "                                     (box dir carries sandbox.ini)\n"
        "  sbie-rt register|unregister|sync-config <dir|*alias>\n"
        "  sbie-rt ps [target]                list boxes / box processes\n"
        "  sbie-rt kill <pid> | kill-box <target>\n"
        "  sbie-rt log [-w] [--last N] [--type TT] [--box B] [--pid P]\n"
        "  sbie-rt info                       system overview\n"
        "  sbie-rt create-box <dir> [--type t] | create-encbox <dir>\n"
        "\n"
        "  sbie-rt --version | --monitor [...] | --help (V2 face: --help)\n");
}

int wmain(int argc, wchar_t** argv)
{
    sbie::util::InitUtf8Console();

    // args[0] is the command (argv[1]); a0 keeps the v3 convention
    // (a0[1] == command) for the dynamic-box command implementations.
    std::vector<std::wstring> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    std::vector<std::wstring> a0(argv, argv + argc);

    if (args.empty()) {
        Usage();
        return 2;
    }

    const std::wstring& cmd = args[0];

    // ---- internal entries (kept from the V2 sbie-cli entry) ----

    if (cmd == L"--version")
        return PrintVersion();

    if (cmd == L"--monitor") {
        sbie::monitor::MonitorOptions mo;
        for (size_t i = 1; i < args.size(); ++i) {
            const long v = (i + 1 < args.size())
                               ? wcstol(args[i + 1].c_str(), nullptr, 10) : -1;
            if (args[i] == L"--poll-ms" && v > 0) {
                mo.pollMs = (DWORD)v;
                ++i;
            } else if (args[i] == L"--grace-ms" && v >= 0) {
                mo.graceMs = (DWORD)v;
                ++i;
            } else if (args[i] == L"--zero-streak" && v > 0) {
                mo.zeroStreak = (int)v;
                ++i;
            } else if (args[i] == L"--empty-ticks" && v > 0) {
                mo.emptyTicks = (int)v;
                ++i;
            }
        }
        return sbie::monitor::RunMonitor(mo);
    }

    if (cmd == L"--set-password") {
        // via SbieSvc SBIE_INI SET_PASSWORD; "-" = empty password
        if (args.size() != 3) {
            sbie::util::PrintErrLineUtf8(
                "usage: sbie-rt --set-password <old|-> <new|->");
            return 2;
        }
        std::wstring oldPw = args[1] == L"-" ? std::wstring() : args[1];
        std::wstring newPw = args[2] == L"-" ? std::wstring() : args[2];
        sbie::SbieStatus st = sbie::svc::SvcClient::Instance().SetPassword(
            oldPw, newPw);
        if (st != sbie::SbieStatus::OK) {
            sbie::util::PrintErrLineUtf8("set-password failed: "
                                         + sbie::util::WideToUtf8(
                                               sbie::StatusName(st)));
            return 1;
        }
        sbie::util::PrintLineUtf8("config password updated");
        return 0;
    }

    if (cmd == L"--ini-del") {
        if (args.size() != 5) {
            sbie::util::PrintErrLineUtf8(
                "usage: sbie-rt --ini-del <section> <setting> <value> <pw|->");
            return 2;
        }
        std::wstring pw = args[4] == L"-" ? std::wstring() : args[4];
        sbie::SbieStatus st = sbie::svc::SvcClient::Instance().IniSetSetting(
            args[1], args[2], args[3], sbie::svc::SvcClient::SetMode::Delete,
            true, pw);
        if (st != sbie::SbieStatus::OK) {
            sbie::util::PrintErrLineUtf8("ini-del failed: "
                                         + sbie::util::WideToUtf8(
                                               sbie::StatusName(st)));
            return 1;
        }
        sbie::util::PrintLineUtf8("value deleted");
        return 0;
    }

    // ---- dynamic-box direct mode ----

    if (cmd == L"drv" || cmd == L"destroy" || cmd == L"query") {
        if (!BindNtdll()) {
            fprintf(stderr, "sbie-rt: cannot bind ntdll\n");
            return 1;
        }
        if (cmd == L"drv")     return CmdDriver();
        if (cmd == L"destroy") return CmdDestroy(a0);
        return CmdQuery(a0);
    }

    if (cmd == L"create") {
        if (!BindNtdll()) {
            fprintf(stderr, "sbie-rt: cannot bind ntdll\n");
            return 1;
        }
        return CmdCreate(a0);
    }

    // "exec <handle> <exe>" -> dynamic box; "exec <dir|*alias>" -> V2 face
    if (cmd == L"exec" && args.size() >= 3 && IsHandleToken(args[1].c_str())) {
        if (!BindNtdll()) {
            fprintf(stderr, "sbie-rt: cannot bind ntdll\n");
            return 1;
        }
        return CmdExec(a0);
    }

    // ---- V2 command face (directory mode) ----

    return sbie::cli::Run(args);
}
