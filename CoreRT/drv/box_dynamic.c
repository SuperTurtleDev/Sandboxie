/*
 * Copyright 2026 Sandboxie-OSS contributors
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// Dynamic Box Management (dynamic-box-arch, docs/12-dynamic-arch.md)
//
// A dynamic box is referenced by handle, not by a Conf_Data section.
// The configuration blob is parsed into a private KV store hung off
// the BOX_DYN_ENTRY:
//
//      - the handle <-> box-name mapping (BOX_DYN_ENTRY list)
//      - the per-box KV pairs (box section + the two skeleton
//        sections), read via BoxDyn_Lookup
//      - the live-process count of each box, for auto teardown
//
// NOTHING is written to Conf_Data.  Conf_Get routes lookups whose
// section name is a live  BoxConfig_<id>  through BoxDyn_Lookup
// (plan A: one entry branch in Conf_GetEx, zero changes at the ~200
// Conf_Get call sites), so the whole driver keeps working on a
// section name as a box name while the storage lives in the entry:
//
//      SbieBoxCreate(blob)   -> parse KV into entry->kv_list
//      proc->box->name       = "BoxConfig_<id>"   (name only)
//      Conf_Get("BoxConfig_<id>", key, index)
//                            -> BoxDyn_Lookup -> entry->kv_list
//      skeleton sections     -> Process_GetPaths routes
//                               (BoxDyn_Lookup with the box name)
//
// API surface (see api_defs.h):
//      API_BOX_CREATE   SbieBoxCreate(configpath)
//      API_BOX_EXEC     SbieBoxExec(handle, exe, ...)
//      API_BOX_DESTROY  SbieBoxDestroy(handle)
//
// Configuration blob (config-to-kernel model):
//      The API_BOX_CREATE text is a multi-section ini-style blob.  Every
//      section the box needs travels with the box -- the driver embeds no
//      default tables and needs no Templates.ini:
//
//          [TemplateDefaultPaths]   skeleton IPC/file rules (per box)
//          [TemplateNetworkPaths]   network device rules (per box)
//          [BoxConfig]              the box section itself (stored under
//                                   the generated BoxConfig_<id> name)
//
//      A flat "Key=Value" blob with no section headers keeps working and
//      lands entirely in the box section (legacy single-section form).
//
// Privilege model:
//      CREATE / EXEC / DESTROY require an elevated administrator caller
//      (UAC-aware: the Administrators SID must be enabled, not deny-only).
//      SYSTEM -- SbieSvc -- passes.  A future SandboxieUsers group check
//      slots into BoxDyn_CheckAccess.
//
// Auto teardown:
//      - last boxed process exits           -> Process_Delete hook
//      - creator process exits (no destroy) -> Process_NotifyProcess_Delete hook
//---------------------------------------------------------------------------


#include "box_dynamic.h"
#include "api.h"
#include "conf.h"
#include "util.h"
#include "session.h"
#include "token.h"
#include "file.h"
#include "key.h"
#include "ipc.h"


//---------------------------------------------------------------------------
// Types
//---------------------------------------------------------------------------


// one value of a (possibly repeated) setting.  values of the same
// setting name are chained in blob parse order, so the ini list
// semantics of Conf_Get(key, index) survive the hash storage.

typedef struct _BOX_DYN_VALUE {

    struct _BOX_DYN_VALUE *next;
    WCHAR *value;

} BOX_DYN_VALUE;


// one setting name of the blob:  owns the name buffer (the map key
// content) and the value chain.

typedef struct _BOX_DYN_KEY {

    WCHAR *name;
    BOX_DYN_VALUE *values;
    BOX_DYN_VALUE *values_tail;

} BOX_DYN_KEY;


typedef struct _BOX_DYN_ENTRY {

    LIST_ELEM list_elem;

    ULONG64 handle;                     // == id used in the box name
    WCHAR section[BOXNAME_COUNT];       // L"BoxConfig_<id>"

    //
    // handle-direct configuration store:  hash map keyed by setting
    // name (content match) -> BOX_DYN_KEY.  The blob's [BoxConfig],
    // [TemplateDefaultPaths] and [TemplateNetworkPaths] sections all
    // merge here -- exactly like classic template expansion merges a
    // template's keys into the box section.  Nothing lands in
    // Conf_Data.
    //

    map_base_t settings_map;            // WCHAR* name -> BOX_DYN_KEY*

    UINT64 bool_bits[2];                // BD_BIT(set, n) -> [set] bit n

    HANDLE creator_pid;                 // process that called BOX_CREATE
    ULONG session_id;                   // captured at create time
    WCHAR *sid;                         // Driver_Pool, captured at create
    ULONG sid_len;

    LONG proc_count;                    // live boxed processes
    BOOLEAN destroying;                 // teardown in progress / done
    BOOLEAN unlinked;                   // removed from BoxDyn_List

} BOX_DYN_ENTRY;


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


static BOX_DYN_ENTRY *BoxDyn_Find_locked(ULONG64 handle);

static BOX_DYN_ENTRY *BoxDyn_FindBySection_locked(const WCHAR *section);

static void BoxDyn_TeardownEntry(BOX_DYN_ENTRY *entry);

static BOOLEAN BoxDyn_TerminatePid(HANDLE pid);

static void BoxDyn_KillBox(BOX_DYN_ENTRY *entry);

static NTSTATUS BoxDyn_GetSidAndSession(
    UNICODE_STRING *SidString, ULONG *SessionId);

static NTSTATUS BoxDyn_CheckAccess(void);

static NTSTATUS BoxDyn_AppendSetting(
    BOX_DYN_ENTRY *entry, const WCHAR *name, const WCHAR *value);

static void BoxDyn_FreeSettings(BOX_DYN_ENTRY *entry);

static const WCHAR *BoxDyn_GetValue_locked(
    BOX_DYN_ENTRY *entry, const WCHAR *setting, ULONG index);

static NTSTATUS BoxDyn_LoadConfigText(
    BOX_DYN_ENTRY *entry, WCHAR *text, BOOLEAN *enabled_seen);


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


static LIST BoxDyn_List;                // all live entries (creator sweeps)
static HASH_MAP BoxDyn_HandleMap;       // ULONG64 handle  -> BOX_DYN_ENTRY*
static HASH_MAP BoxDyn_NameMap;         // section name    -> BOX_DYN_ENTRY*
static PERESOURCE BoxDyn_Lock = NULL;
static BOOLEAN BoxDyn_Initialized = FALSE;

static volatile LONG64 BoxDyn_NextId = 0;

static const WCHAR *BoxDyn_Enabled = L"Enabled";
static const WCHAR *BoxDyn_Yes = L"y";


//---------------------------------------------------------------------------
// map helpers for the name map (string-content keys, like the maps
// conf.c builds for Conf_Data)
//---------------------------------------------------------------------------


_FX unsigned int BoxDyn_NameHash(const void *key, size_t size)
{
    //
    // size is ignored:  the map stores the WCHAR* by value and hands
    // the stored/query pointers to these callbacks (same contract as
    // conf.c's str_map_hash)
    //

    const WCHAR *s = *(const WCHAR * const *)key;
    unsigned int hash = 2166136261;

    while (*s) {
        WCHAR c = *s;
        if (c >= L'a' && c <= L'z')
            c -= L'a' - L'A';
        hash = (hash ^ (unsigned int)c) * 16777619;
        ++s;
    }

    return hash;
}


_FX BOOLEAN BoxDyn_NameMatch(const void *key1, const void *key2)
{
    const WCHAR *s1 = *(const WCHAR * const *)key1;
    const WCHAR *s2 = *(const WCHAR * const *)key2;
    return (_wcsicmp(s1, s2) == 0);
}


//---------------------------------------------------------------------------
// BoxDynamic_Init
//---------------------------------------------------------------------------


_FX BOOLEAN BoxDynamic_Init(void)
{
    List_Init(&BoxDyn_List);

    map_init(&BoxDyn_HandleMap, Driver_Pool);           // key by value
    map_resize(&BoxDyn_HandleMap, 64);

    map_init(&BoxDyn_NameMap, Driver_Pool);             // key = WCHAR* by value,
    BoxDyn_NameMap.func_match_key = &BoxDyn_NameMatch;  // content compared
    BoxDyn_NameMap.func_hash_key = &BoxDyn_NameHash;
    map_resize(&BoxDyn_NameMap, 64);

    if (! Mem_GetLockResource(&BoxDyn_Lock, TRUE))
        return FALSE;

    BoxDyn_Initialized = TRUE;

    Api_SetFunction(API_BOX_CREATE,  BoxDynamic_Api_Create);
    Api_SetFunction(API_BOX_EXEC,    BoxDynamic_Api_Exec);
    Api_SetFunction(API_BOX_DESTROY, BoxDynamic_Api_Destroy);

    return TRUE;
}


//---------------------------------------------------------------------------
// BoxDynamic_Unload
//---------------------------------------------------------------------------


_FX void BoxDynamic_Unload(void)
{
    BOX_DYN_ENTRY *entry;

    if (! BoxDyn_Initialized)
        return;

    BoxDyn_Initialized = FALSE;

    entry = List_Head(&BoxDyn_List);
    while (entry) {

        BOX_DYN_ENTRY *next_entry = List_Next(entry);

        BoxDyn_FreeSettings(entry);

        if (entry->sid)
            Mem_Free(entry->sid, entry->sid_len);
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));

        entry = next_entry;
    }

    List_Init(&BoxDyn_List);

    map_clear(&BoxDyn_HandleMap);
    map_clear(&BoxDyn_NameMap);

    Mem_FreeLockResource(&BoxDyn_Lock);
    BoxDyn_Lock = NULL;
}


//---------------------------------------------------------------------------
// BoxDyn_Find_locked
//---------------------------------------------------------------------------


_FX BOX_DYN_ENTRY *BoxDyn_Find_locked(ULONG64 handle)
{
    return (BOX_DYN_ENTRY *)map_get(&BoxDyn_HandleMap, (void *)handle);
}


//---------------------------------------------------------------------------
// BoxDyn_FindBySection_locked
//---------------------------------------------------------------------------


_FX BOX_DYN_ENTRY *BoxDyn_FindBySection_locked(const WCHAR *section)
{
    return (BOX_DYN_ENTRY *)map_get(&BoxDyn_NameMap, section);
}


//---------------------------------------------------------------------------
// BoxDyn_TeardownEntry
//---------------------------------------------------------------------------
//
// Remove the entry from the handle table, free its KV pairs, and free
// the entry.  Idempotent (guarded by entry->unlinked); safe to call
// from the API path and from the process-exit hooks.
//


_FX void BoxDyn_TeardownEntry(BOX_DYN_ENTRY *entry)
{
    BOOLEAN unlink;

    KIRQL irql;
    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    if (entry->unlinked)
        unlink = FALSE;
    else {
        entry->unlinked = TRUE;
        List_Remove(&BoxDyn_List, entry);
        map_remove(&BoxDyn_HandleMap, (void *)entry->handle);
        map_remove(&BoxDyn_NameMap, entry->section);
        unlink = TRUE;
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! unlink)
        return;

    //
    // the configuration lives in the entry (hash map + bool bits),
    // not in Conf_Data -- unlinking above already made it invisible
    // to BoxDyn_Lookup, so freeing is all that is left
    //

    DbgPrint("SbieDynBox: destroyed dynamic box %S\n", entry->section);

    BoxDyn_FreeSettings(entry);

    if (entry->sid)
        Mem_Free(entry->sid, entry->sid_len);
    Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
}


//---------------------------------------------------------------------------
// BoxDyn_TerminatePid
//---------------------------------------------------------------------------
//
// Terminate one sandboxed process.  Mirrors the core of Process_Api_Kill:
// clear a critical-process flag if set, then ZwTerminateProcess.
//


_FX BOOLEAN BoxDyn_TerminatePid(HANDLE pid)
{
    NTSTATUS status;
    PEPROCESS ProcessObject = NULL;
    HANDLE handle = NULL;

    status = PsLookupProcessByProcessId(pid, &ProcessObject);
    if (! NT_SUCCESS(status))
        return FALSE;

    status = ObOpenObjectByPointer(
        ProcessObject, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
        NULL, KernelMode, &handle);

    ObDereferenceObject(ProcessObject);

    if (NT_SUCCESS(status)) {

        ULONG breakOnTermination;
        status = ZwQueryInformationProcess(
            handle, ProcessBreakOnTermination,
            &breakOnTermination, sizeof(ULONG), NULL);
        if (NT_SUCCESS(status) && breakOnTermination) {
            breakOnTermination = 0;
            ZwSetInformationProcess(
                handle, ProcessBreakOnTermination,
                &breakOnTermination, sizeof(ULONG));
        }

        status = ZwTerminateProcess(handle, DBG_TERMINATE_PROCESS);
        ZwClose(handle);
    }

    return NT_SUCCESS(status);
}


//---------------------------------------------------------------------------
// BoxDyn_KillBox
//---------------------------------------------------------------------------
//
// Terminate every process in the dynamic box.  Sweeps:  collect pids
// under a shared lock on the process list, terminate without any lock
// held, wait briefly, repeat, until the box is empty (or sweeps run out).
//


_FX void BoxDyn_KillBox(BOX_DYN_ENTRY *entry)
{
    ULONG sweep;

    for (sweep = 0; sweep < 12; ++sweep) {

        HANDLE pids[BOX_DYN_MAX_PIDS_PER_SWEEP];
        ULONG count = 0;
        BOOLEAN overflow = FALSE;
        KIRQL irql;

        KeRaiseIrql(APC_LEVEL, &irql);
        ExAcquireResourceSharedLite(Process_ListLock, TRUE);

        __try {

            map_iter_t iter = map_iter();
            while (map_next(&Process_Map, &iter)) {

                PROCESS *proc = iter.value;
                if (proc && proc->box && (! proc->bHostInject)
                        && _wcsicmp(proc->box->name, entry->section) == 0) {

                    if (count < BOX_DYN_MAX_PIDS_PER_SWEEP)
                        pids[count] = proc->pid;
                    else
                        overflow = TRUE;
                    ++count;
                }
            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count = 0;
        }

        ExReleaseResourceLite(Process_ListLock);
        KeLowerIrql(irql);

        if (count == 0)
            break;

        DbgPrint("SbieDynBox: kill sweep %d for %S, %d processes\n",
                 sweep, entry->section, count);

        {
            ULONG n = overflow ? BOX_DYN_MAX_PIDS_PER_SWEEP : count;
            ULONG i;
            for (i = 0; i < n; ++i)
                BoxDyn_TerminatePid(pids[i]);
        }

        {
            LARGE_INTEGER time;
            time.QuadPart = -(100 * 1000 * 10);    // 100ms
            KeDelayExecutionThread(KernelMode, FALSE, &time);
        }
    }
}


//---------------------------------------------------------------------------
// BoxDyn_GetSidAndSession
//---------------------------------------------------------------------------
//
// Capture the box identity.  Prefers the calling thread's impersonation
// token (so a service can create a box on behalf of a user), falls back
// to the primary token of the calling process.
//


_FX NTSTATUS BoxDyn_GetSidAndSession(
    UNICODE_STRING *SidString, ULONG *SessionId)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    void *TokenObject;
    BOOLEAN CopyOnOpen;
    BOOLEAN EffectiveOnly;
    SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;

    TokenObject = PsReferenceImpersonationToken(
                    PsGetCurrentThread(),
                    &CopyOnOpen, &EffectiveOnly, &ImpersonationLevel);

    if (TokenObject) {

        status = SeQuerySessionIdToken(TokenObject, SessionId);
        if (NT_SUCCESS(status))
            status = Token_QuerySidString(TokenObject, SidString);

        PsDereferenceImpersonationToken(TokenObject);

        if (NT_SUCCESS(status))
            return status;

        // fall through to the primary token
    }

    return Process_GetSidStringAndSessionId(
                NtCurrentProcess(), NULL, SidString, SessionId);
}


//---------------------------------------------------------------------------
// BoxDyn_CheckAccess
//---------------------------------------------------------------------------
//
// Privilege gate for the three dynamic-box API entry points
// (dynamic-box-arch):
//
//      - elevated administrators pass.  UAC-aware: the caller's token must
//        carry BUILTIN\Administrators with SE_GROUP_ENABLED and WITHOUT
//        SE_GROUP_USE_FOR_DENY_ONLY -- an admin account on a filtered
//        (non-elevated) token carries the SID deny-only and is rejected.
//      - SYSTEM (SbieSvc) passes: its token has Administrators enabled.
//      - a standard user is rejected with STATUS_ACCESS_DENIED.
//
// TODO (SandboxieUsers): also accept members of the SandboxieUsers local
// group; needs a group-SID lookup (LSA) kernel-side, or the SID handed to
// the driver by SbieSvc at service start.
//


_FX NTSTATUS BoxDyn_CheckAccess(void)
{
    //
    // BUILTIN\Administrators (S-1-5-32-544), built byte-wise like
    // token.c's Token_AdministratorsSid: the Rtl SID helpers are not in
    // the kernel import set this driver links against.
    //

    static UCHAR AdminSid[16] = {
        1,                                      // Revision
        2,                                      // SubAuthorityCount
        0,0,0,0,0,5, // SECURITY_NT_AUTHORITY   // IdentifierAuthority
        SECURITY_BUILTIN_DOMAIN_RID,0,0,0,      // SubAuthority 1
        (DOMAIN_ALIAS_RID_ADMINS & 0xFF),       // SubAuthority 2
            ((DOMAIN_ALIAS_RID_ADMINS & 0xFF00) >> 8),0,0
    };

    PACCESS_TOKEN token;
    PTOKEN_GROUPS groups;
    NTSTATUS status;
    BOOLEAN is_admin = FALSE;
    ULONG i;

    token = PsReferencePrimaryToken(PsGetCurrentProcess());
    if (! token)
        return STATUS_ACCESS_DENIED;

    status = SeQueryInformationToken(token, TokenGroups, &groups);

    PsDereferencePrimaryToken(token);

    if (! NT_SUCCESS(status))
        return status;

    for (i = 0; i < groups->GroupCount; ++i) {

        ULONG attrs = groups->Groups[i].Attributes;

        if (RtlEqualSid(groups->Groups[i].Sid, AdminSid)) {
            if ((attrs & SE_GROUP_ENABLED) &&
                ! (attrs & SE_GROUP_USE_FOR_DENY_ONLY))
                is_admin = TRUE;
            break;
        }
    }

    ExFreePool(groups);

    return (is_admin ? STATUS_SUCCESS : STATUS_ACCESS_DENIED);
}


//---------------------------------------------------------------------------
// BoxDyn_TemplateSections
//---------------------------------------------------------------------------
//
// The two per-box skeleton sections a blob may carry.  Fixed set: anything
// else in a [header] is rejected, so a blob cannot address arbitrary
// sections (in particular not another BoxConfig_* box).
//


static const WCHAR *BoxDyn_TemplateSections[2] = {
    L"TemplateDefaultPaths", L"TemplateNetworkPaths"
};


//---------------------------------------------------------------------------
// BoxDyn_BoolKeys
//---------------------------------------------------------------------------
//
// y/n keys go into the entry's bool_bits bitset (O(1) read: one AND).
// The string copy still lands in the settings map so the name-based
// query paths (Conf_GetEx branch / API_QUERY_CONF from SbieDll and
// sbie-rt query) keep seeing the textual value.
//


typedef struct _BOX_DYN_BOOLKEY {
    const WCHAR *name;
    UINT64 bit;
} BOX_DYN_BOOLKEY;

static const BOX_DYN_BOOLKEY BoxDyn_BoolKeys[BOX_DYN_BOOL_COUNT] = {
    { L"AlertStartRunAccessDenied",      BD_BOOL_ALERT_START_RUN_ACCESS_DENIED },
    { L"AllowBoxedJobs",                 BD_BOOL_ALLOW_BOXED_JOBS },
    { L"AllowForceImmersive",            BD_BOOL_ALLOW_FORCE_IMMERSIVE },
    { L"AllowForceSystem",               BD_BOOL_ALLOW_FORCE_SYSTEM },
    { L"AllowRawDiskRead",               BD_BOOL_ALLOW_RAW_DISK_READ },
    { L"AllowSpoolerPrintToFile",        BD_BOOL_ALLOW_SPOOLER_PRINT_TO_FILE },
    { L"AlwaysCloseForBoxed",            BD_BOOL_ALWAYS_CLOSE_FOR_BOXED },
    { L"AnonymousLogon",                 BD_BOOL_ANONYMOUS_LOGON },
    { L"BlockIEEmbedding",               BD_BOOL_BLOCK_IE_EMBEDDING },
    { L"BlockNetworkFiles",              BD_BOOL_BLOCK_NETWORK_FILES },
    { L"BlockPassword",                  BD_BOOL_BLOCK_PASSWORD },
    { L"ConfidentialBox",                BD_BOOL_CONFIDENTIAL_BOX },
    { L"CopyDeviceGroups",               BD_BOOL_COPY_DEVICE_GROUPS },
    { L"CopyTokenAttributes",            BD_BOOL_COPY_TOKEN_ATTRIBUTES },
    { L"DisableFileFilter",              BD_BOOL_DISABLE_FILE_FILTER },
    { L"DisableForceRules",              BD_BOOL_DISABLE_FORCE_RULES },
    { L"DisableKeyFilter",               BD_BOOL_DISABLE_KEY_FILTER },
    { L"DisableObjectFilter",            BD_BOOL_DISABLE_OBJECT_FILTER },
    { L"DisableResourceMonitor",         BD_BOOL_DISABLE_RESOURCE_MONITOR },
    { L"DontOpenForBoxed",               BD_BOOL_DONT_OPEN_FOR_BOXED },
    { L"EnableObjectFiltering",          BD_BOOL_ENABLE_OBJECT_FILTERING },
    { L"EnableWin32kHooks",              BD_BOOL_ENABLE_WIN32K_HOOKS },
    { L"Enabled",                        BD_BOOL_ENABLED },
    { L"ForceBoxDocs",                   BD_BOOL_FORCE_BOX_DOCS },
    { L"ForceExplorerChild",             BD_BOOL_FORCE_EXPLORER_CHILD },
    { L"ForceMarkOfTheWeb",              BD_BOOL_FORCE_MARK_OF_THE_WEB },
    { L"IgnoreWin32HookBlacklist",       BD_BOOL_IGNORE_WIN32_HOOK_BLACKLIST },
    { L"KeepLogonSession",               BD_BOOL_KEEP_LOGON_SESSION },
    { L"KeepTokenIntegrity",             BD_BOOL_KEEP_TOKEN_INTEGRITY },
    { L"KeepUserGroup",                  BD_BOOL_KEEP_USER_GROUP },
    { L"LogMessageEvents",               BD_BOOL_LOG_MESSAGE_EVENTS },
    { L"MonitorStackTrace",              BD_BOOL_MONITOR_STACK_TRACE },
    { L"NetworkEnableWFP",               BD_BOOL_NETWORK_ENABLE_WFP },
    { L"NoAddProcessToJob",              BD_BOOL_NO_ADD_PROCESS_TO_JOB },
    { L"NoSandboxieConsole",             BD_BOOL_NO_SANDBOXIE_CONSOLE },
    { L"NoSecurityFiltering",            BD_BOOL_NO_SECURITY_FILTERING },
    { L"NoSecurityIsolation",            BD_BOOL_NO_SECURITY_ISOLATION },
    { L"NoUntrustedToken",               BD_BOOL_NO_UNTRUSTED_TOKEN },
    { L"NotifyBoxProtected",             BD_BOOL_NOTIFY_BOX_PROTECTED },
    { L"NotifyDirectDiskAccess",         BD_BOOL_NOTIFY_DIRECT_DISK_ACCESS },
    { L"NotifyForceProcessDisabled",     BD_BOOL_NOTIFY_FORCE_PROCESS_DISABLED },
    { L"NotifyForceProcessEnabled",      BD_BOOL_NOTIFY_FORCE_PROCESS_ENABLED },
    { L"NotifyImageLoadDenied",          BD_BOOL_NOTIFY_IMAGE_LOAD_DENIED },
    { L"NotifyInternetAccessDenied",     BD_BOOL_NOTIFY_INTERNET_ACCESS_DENIED },
    { L"NotifyProcessAccessDenied",      BD_BOOL_NOTIFY_PROCESS_ACCESS_DENIED },
    { L"NotifyRootProtected",            BD_BOOL_NOTIFY_ROOT_PROTECTED },
    { L"NotifyStartRunAccessDenied",     BD_BOOL_NOTIFY_START_RUN_ACCESS_DENIED },
    { L"NtNamespaceIsolation",           BD_BOOL_NT_NAMESPACE_ISOLATION },
    { L"OpenAllSysCalls",                BD_BOOL_OPEN_ALL_SYS_CALLS },
    { L"OpenDevCMApi",                   BD_BOOL_OPEN_DEV_CM_API },
    { L"OpenLsaEndpoint",                BD_BOOL_OPEN_LSA_ENDPOINT },
    { L"OpenPrintSpooler",               BD_BOOL_OPEN_PRINT_SPOOLER },
    { L"OpenSamEndpoint",                BD_BOOL_OPEN_SAM_ENDPOINT },
    { L"OpenWndStation",                 BD_BOOL_OPEN_WND_STATION },
    { L"OriginalToken",                  BD_BOOL_ORIGINAL_TOKEN },
    { L"ProtectHostImages",              BD_BOOL_PROTECT_HOST_IMAGES },
    { L"ReplicateToken",                 BD_BOOL_REPLICATE_TOKEN },
    { L"RestrictDevices",                BD_BOOL_RESTRICT_DEVICES },
    { L"SandboxieAllGroup",              BD_BOOL_SANDBOXIE_ALL_GROUP },
    { L"StartRunAlertDenied",            BD_BOOL_START_RUN_ALERT_DENIED },
    { L"SysCallLockDown",                BD_BOOL_SYS_CALL_LOCK_DOWN },
    { L"TerminateUsingService",          BD_BOOL_TERMINATE_USING_SERVICE },
    { L"UnfilteredToken",                BD_BOOL_UNFILTERED_TOKEN },
    { L"UnrestrictedToken",              BD_BOOL_UNRESTRICTED_TOKEN },
    { L"UnstrippedToken",                BD_BOOL_UNSTRIPPED_TOKEN },
    { L"UseCreateToken",                 BD_BOOL_USE_CREATE_TOKEN },
    { L"UsePrivacyMode",                 BD_BOOL_USE_PRIVACY_MODE },
    { L"UseRuleSpecificity",             BD_BOOL_USE_RULE_SPECIFICITY },
    { L"UseWin32kFilterTable",           BD_BOOL_USE_WIN32K_FILTER_TABLE },
};

//---------------------------------------------------------------------------
// BoxDyn_AppendSetting
//---------------------------------------------------------------------------


_FX NTSTATUS BoxDyn_AppendSetting(
    BOX_DYN_ENTRY *entry, const WCHAR *name, const WCHAR *value)
{
    ULONG name_len = (wcslen(name) + 1) * sizeof(WCHAR);
    ULONG value_len = (wcslen(value) + 1) * sizeof(WCHAR);

    BOX_DYN_VALUE *node;
    BOX_DYN_KEY *key;

    //
    // bool keys additionally set their bitset bit (y/1) -- the map
    // copy below still keeps the textual value for name-based queries
    //

    if (value[0] == L'y' || value[0] == L'Y'
            || value[0] == L'1' || _wcsicmp(value, L"true") == 0) {
        ULONG i;
        for (i = 0; i < BOX_DYN_BOOL_COUNT; ++i) {
            if (_wcsicmp(BoxDyn_BoolKeys[i].name, name) == 0) {
                UINT64 bd = BoxDyn_BoolKeys[i].bit;
                entry->bool_bits[bd >> 32] |= (1ULL << (bd & 0xFFFFFFFF));
                break;
            }
        }
    }

    //
    // find or create the key node in the settings hash map
    //

    key = (BOX_DYN_KEY *)map_get(&entry->settings_map, name);

    if (! key) {

        key = Mem_Alloc(Driver_Pool, sizeof(BOX_DYN_KEY));
        if (! key)
            return STATUS_INSUFFICIENT_RESOURCES;

        key->name = Mem_Alloc(Driver_Pool, name_len);
        if (! key->name) {
            Mem_Free(key, sizeof(BOX_DYN_KEY));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        memcpy(key->name, name, name_len);

        key->values = NULL;
        key->values_tail = NULL;

        if (! map_insert(&entry->settings_map, key->name, key, 0)) {
            Mem_Free(key->name, name_len);
            Mem_Free(key, sizeof(BOX_DYN_KEY));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    //
    // append the value to the key's chain (parse order = list order)
    //

    node = Mem_Alloc(Driver_Pool, sizeof(BOX_DYN_VALUE));
    if (! node)
        return STATUS_INSUFFICIENT_RESOURCES;

    node->value = Mem_Alloc(Driver_Pool, value_len);
    if (! node->value) {
        Mem_Free(node, sizeof(BOX_DYN_VALUE));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memcpy(node->value, value, value_len);
    node->next = NULL;

    if (key->values_tail)
        key->values_tail->next = node;
    else
        key->values = node;
    key->values_tail = node;

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// BoxDyn_FreeSettings
//---------------------------------------------------------------------------


_FX void BoxDyn_FreeSettings(BOX_DYN_ENTRY *entry)
{
    map_iter_t iter = map_iter();

    while (map_next(&entry->settings_map, &iter)) {

        BOX_DYN_KEY *key = (BOX_DYN_KEY *)iter.value;
        BOX_DYN_VALUE *node = key->values;

        while (node) {
            BOX_DYN_VALUE *next_node = node->next;
            Mem_Free(node->value, (wcslen(node->value) + 1) * sizeof(WCHAR));
            Mem_Free(node, sizeof(BOX_DYN_VALUE));
            node = next_node;
        }

        Mem_Free(key->name, (wcslen(key->name) + 1) * sizeof(WCHAR));
        Mem_Free(key, sizeof(BOX_DYN_KEY));

        // the node the iterator points at is freed by map_erase below
        iter.value = NULL;
    }

    //
    // walk the buckets again and free the map nodes themselves; the
    // loop above consumed the iterator, so re-iterate raw buckets
    //

    map_clear(&entry->settings_map);
}


//---------------------------------------------------------------------------
// BoxDyn_Lookup
//---------------------------------------------------------------------------


_FX const WCHAR *BoxDyn_Lookup(
    const WCHAR *box_name, const WCHAR *setting, ULONG index)
{
    //
    // name-based handle-direct read (the Conf_GetEx branch and the
    // skeleton consumers use this; Process_GetConf uses the pointer
    // fast path BoxDyn_GetValue):
    //
    //   with a setting:  index-th value of the repeated key (ini list
    //   semantics, parse order).  without a setting:  name of the
    //   index-th distinct key (hash order; box-section enumeration of
    //   a dynamic box is a maintenance/diagnostic path only).
    //
    //   returns NULL when the box is unknown or dead.
    //

    BOX_DYN_ENTRY *entry;
    const WCHAR *value = NULL;
    KIRQL irql;

    if ((! BoxDyn_Initialized) || (! box_name))
        return NULL;

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceSharedLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(box_name);

    if (entry && (! entry->unlinked)) {

        if (setting && *setting) {

            value = BoxDyn_GetValue_locked(entry, setting, index);

        } else {

            map_iter_t iter = map_iter();
            ULONG n = 0;

            while (map_next(&entry->settings_map, &iter)) {
                if (n == index) {
                    BOX_DYN_KEY *key = (BOX_DYN_KEY *)iter.value;
                    value = key->name;
                    break;
                }
                ++n;
            }
        }
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    return value;
}


//---------------------------------------------------------------------------
// BoxDyn_GetValue_locked
//---------------------------------------------------------------------------


_FX const WCHAR *BoxDyn_GetValue_locked(
    BOX_DYN_ENTRY *entry, const WCHAR *setting, ULONG index)
{
    BOX_DYN_KEY *key = (BOX_DYN_KEY *)map_get(&entry->settings_map, setting);
    BOX_DYN_VALUE *node;
    ULONG n = 0;

    if (! key)
        return NULL;

    node = key->values;
    while (node && n < index) {
        node = node->next;
        ++n;
    }

    return (node ? node->value : NULL);
}


//---------------------------------------------------------------------------
// BoxDyn_LoadConfigText
//---------------------------------------------------------------------------
//
// Parse the multi-section KV blob into the entry's hash store
// ("Key=Value" lines, \n or \r\n separated, '#' / ';' comments,
// repeated keys append, parse order kept).
//
//      [TemplateDefaultPaths] / [TemplateNetworkPaths]
//          -> merged into the same key space as the box section --
//             exactly like classic template expansion inlines a
//             template's keys into the box section
//      [BoxConfig]
//          -> the box section's keys
//      no header before the first Key=value line
//          -> legacy flat form: everything is box-section keys
//
// The header still gates WHICH sections a blob may address (fixed
// set), so a blob cannot smuggle semantics for other purposes.
//
// Any other [header] is rejected with STATUS_INVALID_PARAMETER, as is
// a Key line with an empty value.  Trailing and leading whitespace is
// trimmed like Conf_Read_Settings does.
//


_FX NTSTATUS BoxDyn_LoadConfigText(
    BOX_DYN_ENTRY *entry, WCHAR *text, BOOLEAN *enabled_seen)
{
    static const WCHAR *BoxSectionHeader = L"BoxConfig";

    NTSTATUS status = STATUS_SUCCESS;
    WCHAR *line = text;

    *enabled_seen = FALSE;

    while (line) {

        WCHAR *eol;
        WCHAR *name;
        WCHAR *name_end;
        WCHAR *value;
        WCHAR *value_end;

        //
        // isolate one line
        //

        eol = wcschr(line, L'\n');
        if (eol) {
            if (eol > line && eol[-1] == L'\r')
                eol[-1] = L'\0';
            *eol = L'\0';
        }

        //
        // trim leading whitespace / skip comments and empty lines
        //

        while (*line == L' ' || *line == L'\t')
            ++line;

        if (*line == L'\0' || *line == L'#' || *line == L';')
            goto next_line;

        //
        // [section] header:  validated against the fixed set, but all
        // three sections share one merged key space
        //

        if (*line == L'[') {

            WCHAR *close = wcschr(line, L']');
            WCHAR *hdr;

            if (! close) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            *close = L'\0';          // ignore anything after the ']'

            hdr = line + 1;
            while (*hdr == L' ' || *hdr == L'\t')
                ++hdr;

            if (hdr == close || *hdr == L'\0') {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            if (_wcsicmp(hdr, BoxSectionHeader) != 0) {

                ULONG i;
                BOOLEAN known = FALSE;

                for (i = 0; i < 2; ++i) {
                    if (_wcsicmp(hdr, BoxDyn_TemplateSections[i]) == 0) {
                        known = TRUE;
                        break;
                    }
                }

                if (! known) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
            }

            goto next_line;
        }

        //
        // split Key=value
        //

        name = line;
        name_end = wcschr(name, L'=');
        if ((! name_end) || name_end == name) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        value = name_end + 1;

        while (name_end > name &&
                (name_end[-1] == L' ' || name_end[-1] == L'\t'))
            --name_end;
        *name_end = L'\0';

        while (*value == L' ' || *value == L'\t')
            ++value;

        value_end = value + wcslen(value);
        while (value_end > value &&
                (value_end[-1] == L' ' || value_end[-1] == L'\t'))
            --value_end;
        *value_end = L'\0';

        if (*value == L'\0') {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // append to the merged key space (list semantics for repeated
        // keys); Enabled counts everywhere (flat form has no headers)
        //

        if (_wcsicmp(name, BoxDyn_Enabled) == 0)
            *enabled_seen = TRUE;

        status = BoxDyn_AppendSetting(entry, name, value);
        if (! NT_SUCCESS(status))
            break;

    next_line:

        line = eol ? eol + 1 : NULL;
    }

    return status;
}


//---------------------------------------------------------------------------
// BoxDynamic_Api_Create
//---------------------------------------------------------------------------
//
// API_BOX_CREATE -- SbieBoxCreate(configpath)
//
// parms[1] config_text : UNICODE_STRING64, UTF-16 KV stream
// parms[2] box_handle  : ULONG64* out
// parms[3] box_name    : WCHAR[BOXNAME_COUNT] out
//


_FX NTSTATUS BoxDynamic_Api_Create(PROCESS *proc, ULONG64 *parms)
{
    API_BOX_CREATE_ARGS *args = (API_BOX_CREATE_ARGS *)parms;
    NTSTATUS status;
    UNICODE_STRING64 *user_uni;
    WCHAR *text = NULL;
    size_t text_len = 0;
    UNICODE_STRING SidString;
    ULONG SessionId;
    ULONG64 id;
    BOX_DYN_ENTRY *entry = NULL;
    KIRQL irql;

    //
    // a sandboxed process may not create sandboxes
    //

    if (proc)
        return STATUS_ACCESS_DENIED;

    //
    // privilege gate: elevated admin / SandboxieUsers (see
    // BoxDyn_CheckAccess)
    //

    status = BoxDyn_CheckAccess();
    if (! NT_SUCCESS(status))
        return status;

    //
    // copy the configuration text from user space
    //

    user_uni = args->config_text.val;
    if (! user_uni)
        return STATUS_INVALID_PARAMETER;

    status = Api_CopyStringFromUser(&text, &text_len, user_uni);
    if (! NT_SUCCESS(status))
        return status;

    if (text_len < sizeof(WCHAR) * 2 || text_len > BOX_DYN_MAX_TEXT_LEN) {
        Mem_Free(text, text_len);
        return STATUS_INVALID_PARAMETER;
    }

    //
    // capture box identity (SID + session) from the calling context
    //

    status = BoxDyn_GetSidAndSession(&SidString, &SessionId);
    if (! NT_SUCCESS(status)) {
        Mem_Free(text, text_len);
        return status;
    }

    //
    // allocate a unique section name  BoxConfig_<id>
    //

    id = (ULONG64)InterlockedIncrement64(&BoxDyn_NextId);

    entry = Mem_Alloc(Driver_Pool, sizeof(BOX_DYN_ENTRY));
    if (! entry) {
        RtlFreeUnicodeString(&SidString);
        Mem_Free(text, text_len);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    memzero(entry, sizeof(BOX_DYN_ENTRY));

    RtlStringCbPrintfW(
        entry->section, sizeof(entry->section), L"%s%I64u",
        BOX_DYN_PREFIX, id);

    if (! Box_IsValidName(entry->section)) {    // should not happen
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
        RtlFreeUnicodeString(&SidString);
        Mem_Free(text, text_len);
        return STATUS_UNSUCCESSFUL;
    }

    entry->handle = id;
    entry->creator_pid = PsGetCurrentProcessId();
    entry->session_id = SessionId;
    entry->sid_len = SidString.Length + sizeof(WCHAR);
    entry->sid = Mem_Alloc(Driver_Pool, entry->sid_len);
    if (! entry->sid) {
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
        RtlFreeUnicodeString(&SidString);
        Mem_Free(text, text_len);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memcpy(entry->sid, SidString.Buffer, entry->sid_len);
    entry->sid[entry->sid_len / sizeof(WCHAR) - 1] = L'\0';

    RtlFreeUnicodeString(&SidString);

    //
    // init the handle-direct configuration store and parse the KV
    // stream into it (nothing is written to Conf_Data)
    //

    map_init(&entry->settings_map, Driver_Pool);
    entry->settings_map.func_match_key = &BoxDyn_NameMatch;
    entry->settings_map.func_hash_key = &BoxDyn_NameHash;
    map_resize(&entry->settings_map, 32);

    {

        BOOLEAN enabled_seen = FALSE;

        status = BoxDyn_LoadConfigText(entry, text, &enabled_seen);

        //
        // classic process start checks Enabled=y via Conf_IsBoxEnabled;
        // force it unless the config supplied its own
        //

        if (NT_SUCCESS(status) && (! enabled_seen))
            status = BoxDyn_AppendSetting(
                        entry, BoxDyn_Enabled, BoxDyn_Yes);
    }

    Mem_Free(text, text_len);

    if (! NT_SUCCESS(status)) {
        BoxDyn_FreeSettings(entry);
        Mem_Free(entry->sid, entry->sid_len);
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
        return status;
    }

    //
    // publish the handle:  list + handle map + name map
    //

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    if (! BoxDyn_Initialized)
        status = STATUS_SERVER_DISABLED;
    else if (List_Count(&BoxDyn_List) >= BOX_DYN_MAX_BOXES)
        status = STATUS_ALLOTTED_SPACE_EXCEEDED;   // too many dynamic boxes
    else {
        List_Insert_After(&BoxDyn_List, NULL, entry);
        map_insert(&BoxDyn_HandleMap, (void *)entry->handle, entry, 0);
        map_insert(&BoxDyn_NameMap, entry->section, entry, 0);
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! NT_SUCCESS(status)) {
        BoxDyn_FreeSettings(entry);
        Mem_Free(entry->sid, entry->sid_len);
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
        return status;
    }

    DbgPrint("SbieDynBox: created %S (handle %I64u, creator %d)\n",
             entry->section, entry->handle, (ULONG)(ULONG_PTR)entry->creator_pid);

    //
    // write results back to the caller
    //

    __try {

        if (args->box_handle.val) {
            ProbeForWrite(args->box_handle.val, sizeof(ULONG64), sizeof(ULONG64));
            *args->box_handle.val = entry->handle;
        }

        if (args->box_name.val) {
            WCHAR *user_name = args->box_name.val;
            ProbeForWrite(user_name,
                          sizeof(WCHAR) * BOXNAME_COUNT, sizeof(WCHAR));
            wcsncpy(user_name, entry->section, BOXNAME_COUNT - 2);
            user_name[BOXNAME_COUNT - 2] = L'\0';
        }

        status = STATUS_SUCCESS;

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    if (! NT_SUCCESS(status))
        BoxDyn_TeardownEntry(entry);

    return status;
}


//---------------------------------------------------------------------------
// BoxDynamic_Api_Exec
//---------------------------------------------------------------------------
//
// API_BOX_EXEC -- SbieBoxExec(handle, exefile, ...)
//
// The caller has already created the target process suspended (this is
// the same contract SbieSvc uses with API_START_PROCESS).  We claim the
// process into the dynamic box through the standard injection path.
//
// parms[1] box_handle   : ULONG64 in
// parms[2] process_id   : HANDLE in (suspended target)
// parms[3] fake_admin   : BOOLEAN in
// parms[4] out_process_id : ULONG64* out (optional)
//


_FX NTSTATUS BoxDynamic_Api_Exec(PROCESS *proc, ULONG64 *parms)
{
    API_BOX_EXEC_ARGS *args = (API_BOX_EXEC_ARGS *)parms;
    NTSTATUS status;
    BOX_DYN_ENTRY *entry;
    BOX *box = NULL;
    PEPROCESS ProcessObject = NULL;
    HANDLE pid;
    WCHAR section[BOXNAME_COUNT];
    WCHAR *sid = NULL;
    ULONG sid_len = 0;
    ULONG session_id;
    KIRQL irql;
    BOOLEAN claimed = FALSE;

    //
    // a sandboxed process may not start processes into a dynamic box
    //

    if (proc)
        return STATUS_ACCESS_DENIED;

    status = BoxDyn_CheckAccess();
    if (! NT_SUCCESS(status))
        return status;

    pid = args->process_id.val;
    if (! pid)
        return STATUS_INVALID_CID;

    //
    // locate the box; only its creator or the service may exec
    //

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_Find_locked(args->box_handle.val);

    if ((! entry) || entry->unlinked) {

        status = STATUS_INVALID_HANDLE;

    } else if (entry->destroying) {

        status = STATUS_DELETE_PENDING;

    } else if (PsGetCurrentProcessId() != entry->creator_pid
            && PsGetCurrentProcessId() != Api_ServiceProcessId) {

        status = STATUS_ACCESS_DENIED;

    } else if (! Process_ReadyToSandbox) {

        status = STATUS_SERVER_DISABLED;

    } else {

        //
        // snapshot the box identity:  the entry can be torn down on
        // another CPU while we work (creator exits, last process dies),
        // so nothing below dereferences the entry after we release
        // the lock
        //

        wcscpy(section, entry->section);
        session_id = entry->session_id;

        sid_len = entry->sid_len;
        sid = Mem_Alloc(Driver_Pool, sid_len);
        if (sid) {
            memcpy(sid, entry->sid, sid_len);
            status = STATUS_SUCCESS;
        } else
            status = STATUS_INSUFFICIENT_RESOURCES;
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! NT_SUCCESS(status))
        return status;

    //
    // build the BOX from the temp section (Box_InitPaths reads
    // FileRootPath / KeyRootPath / IpcRootPath from it)
    //

    box = Box_CreateEx(Driver_Pool, section, sid, session_id, TRUE);
    if (! box) {
        Mem_Free(sid, sid_len);
        return STATUS_UNSUCCESSFUL;
    }

    box->fake_admin = (BOOLEAN)args->fake_admin.val;

    //
    // verify the target process and its session
    //

    status = PsLookupProcessByProcessId(pid, &ProcessObject);

    if (NT_SUCCESS(status)) {

        if (PsGetProcessSessionId(ProcessObject) != session_id)
            status = STATUS_LOGON_SESSION_COLLISION;

        //
        // claim the process into the box -- same path as
        // Process_Api_Start:  Process_Create clones the box,
        // Process_Low_Inject asks SbieSvc DriverAssist to inject
        // SbieLow, then SbieDll bootstraps against the temp section.
        // Process_NotifyProcess_Create consumes (frees) the box.
        //

        if (NT_SUCCESS(status)) {

            if (Process_NotifyProcess_Create(
                    pid, Api_ServiceProcessId, Api_ServiceProcessId,
                    NULL, 0, box)) {

                claimed = TRUE;
            }

            box = NULL;
        }

        ObDereferenceObject(ProcessObject);
    }

    if (box)
        Box_Free(box);

    if (! NT_SUCCESS(status)) {
        Mem_Free(sid, sid_len);
        return status;
    }

    if (! claimed) {
        Mem_Free(sid, sid_len);
        return STATUS_INTERNAL_ERROR;
    }

    //
    // the Process_Create hook inside Process_NotifyProcess_Create already
    // counted the process; here we only check the box was not torn down
    // concurrently -- if it was, kill the just-claimed process and report
    //

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(section);
    if (entry && entry->unlinked)
        entry = NULL;

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    Mem_Free(sid, sid_len);

    if (! entry) {
        BoxDyn_TerminatePid(pid);
        return STATUS_DELETE_PENDING;
    }

    __try {

        if (args->out_process_id.val) {
            ProbeForWrite(args->out_process_id.val, sizeof(ULONG64), sizeof(ULONG64));
            *args->out_process_id.val = (ULONG64)(ULONG_PTR)pid;
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // non-fatal:  the process is already claimed
    }

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// BoxDynamic_Api_Destroy
//---------------------------------------------------------------------------
//
// API_BOX_DESTROY -- SbieBoxDestroy(handle)
//
// parms[1] box_handle    : ULONG64 in
// parms[2] kill_processes: ULONG in, nonzero = terminate boxed processes
//


_FX NTSTATUS BoxDynamic_Api_Destroy(PROCESS *proc, ULONG64 *parms)
{
    API_BOX_DESTROY_ARGS *args = (API_BOX_DESTROY_ARGS *)parms;
    NTSTATUS status;
    BOX_DYN_ENTRY *entry;
    KIRQL irql;

    if (proc)
        return STATUS_ACCESS_DENIED;

    status = BoxDyn_CheckAccess();
    if (! NT_SUCCESS(status))
        return status;

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_Find_locked(args->box_handle.val);

    if ((! entry) || entry->unlinked) {

        status = STATUS_INVALID_HANDLE;

    } else if (PsGetCurrentProcessId() != entry->creator_pid
            && PsGetCurrentProcessId() != Api_ServiceProcessId) {

        status = STATUS_ACCESS_DENIED;

    } else {

        entry->destroying = TRUE;   // blocks exec and auto-teardown races
        status = STATUS_SUCCESS;
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! NT_SUCCESS(status))
        return status;

    if (args->kill_processes.val)
        BoxDyn_KillBox(entry);

    //
    // processes that die inside KillBox run their own Process_Delete
    // hooks; with destroying set those will not double-teardown
    //

    BoxDyn_TeardownEntry(entry);

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// BoxDynamic_OnProcessCreate
//---------------------------------------------------------------------------
//
// Hook from Process_Create:  every process that enters a box lands here,
// including children started from inside a dynamic box.  If the box name
// names a live dynamic box, its live-process count is raised (so the
// auto-teardown rule "last process gone" sees descendants too) and the
// entry is returned for proc->box_dyn.
//


_FX struct _BOX_DYN_ENTRY *BoxDynamic_OnProcessCreate(const WCHAR *box_name)
{
    BOX_DYN_ENTRY *entry = NULL;
    KIRQL irql;

    if ((! BoxDyn_Initialized) || (! box_name))
        return NULL;

    if (_wcsnicmp(box_name, BOX_DYN_PREFIX, 10) != 0)
        return NULL;     // not a dynamic box

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(box_name);
    if (entry && (! entry->unlinked) && (! entry->destroying))
        ++entry->proc_count;
    else
        entry = NULL;

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    return entry;
}


//---------------------------------------------------------------------------
// BoxDyn_GetEntry
//---------------------------------------------------------------------------


_FX struct _BOX_DYN_ENTRY *BoxDyn_GetEntry(const WCHAR *box_name)
{
    BOX_DYN_ENTRY *entry;
    KIRQL irql;

    if ((! BoxDyn_Initialized) || (! box_name))
        return NULL;

    if (_wcsnicmp(box_name, BOX_DYN_PREFIX, 10) != 0)
        return NULL;

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceSharedLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(box_name);
    if (entry && entry->unlinked)
        entry = NULL;

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    return entry;
}


//---------------------------------------------------------------------------
// BoxDyn_GetValue / BoxDyn_GetBit
//---------------------------------------------------------------------------


_FX const WCHAR *BoxDyn_GetValue(
    struct _BOX_DYN_ENTRY *entry, const WCHAR *setting, ULONG index)
{
    //
    // lock-free:  the entry's settings map is immutable after publish,
    // and a PROCESS keeps the box alive (auto teardown waits for the
    // last boxed process) -- see the header note
    //

    return BoxDyn_GetValue_locked(
                (BOX_DYN_ENTRY *)entry, setting, index);
}


_FX BOOLEAN BoxDyn_GetBit(struct _BOX_DYN_ENTRY *entry, UINT64 bit)
{
    BOX_DYN_ENTRY *e = (BOX_DYN_ENTRY *)entry;
    return (e->bool_bits[bit >> 32] & (1ULL << (bit & 0xFFFFFFFF))) != 0;
}


//---------------------------------------------------------------------------
// BoxDyn_BoolName
//---------------------------------------------------------------------------
//
// reverse mapping for the Process_GetConf_Bit static-box fallback:
// bit -> setting name (NULL for an unknown bit)
//


_FX const WCHAR *BoxDyn_BoolName(UINT64 bit)
{
    ULONG i;

    for (i = 0; i < BOX_DYN_BOOL_COUNT; ++i) {
        if (BoxDyn_BoolKeys[i].bit == bit)
            return BoxDyn_BoolKeys[i].name;
    }

    return NULL;
}


//---------------------------------------------------------------------------
// BoxDynamic_OnProcessDelete
//---------------------------------------------------------------------------
//
// Hook from Process_Delete:  the PROCESS structure for a sandboxed
// process is about to be freed.  Called with no driver locks held.
//


_FX void BoxDynamic_OnProcessDelete(PROCESS *proc)
{
    BOX_DYN_ENTRY *entry;
    KIRQL irql;
    BOOLEAN teardown = FALSE;

    if ((! BoxDyn_Initialized) || (! proc) || (! proc->box))
        return;

    if (_wcsnicmp(proc->box->name, BOX_DYN_PREFIX, 10) != 0)
        return;     // not a dynamic box

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(proc->box->name);

    if (entry && (! entry->unlinked)) {

        if (entry->proc_count > 0)
            --entry->proc_count;

        if (entry->proc_count == 0 && (! entry->destroying)) {
            entry->destroying = TRUE;   // auto teardown:  box is empty
            teardown = TRUE;
        }
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (teardown) {

        DbgPrint("SbieDynBox: last process gone, auto destroy %S\n",
                 entry->section);

        BoxDyn_TeardownEntry(entry);
    }
}


//---------------------------------------------------------------------------
// BoxDynamic_OnAnyProcessExit
//---------------------------------------------------------------------------
//
// Hook from Process_NotifyProcess_Delete:  called for EVERY process that
// exits.  If the exiting process is the creator of dynamic boxes, those
// boxes are destroyed (kill + teardown), so an owner that dies without
// calling SbieBoxDestroy does not leak its boxes.
//


_FX void BoxDynamic_OnAnyProcessExit(HANDLE ProcessId)
{
    BOX_DYN_ENTRY *entries[BOX_DYN_MAX_BOXES];
    ULONG count = 0;
    BOX_DYN_ENTRY *entry;
    KIRQL irql;
    ULONG i;

    if (! BoxDyn_Initialized)
        return;

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = List_Head(&BoxDyn_List);
    while (entry) {

        if (entry->creator_pid == ProcessId && (! entry->destroying)) {
            entry->destroying = TRUE;
            if (count < BOX_DYN_MAX_BOXES)
                entries[count++] = entry;
        }

        entry = List_Next(entry);
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    for (i = 0; i < count; ++i) {

        DbgPrint("SbieDynBox: creator %d exited, destroying %S\n",
                 (ULONG)(ULONG_PTR)ProcessId, entries[i]->section);

        BoxDyn_KillBox(entries[i]);
        BoxDyn_TeardownEntry(entries[i]);
    }
}
