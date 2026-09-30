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
// A dynamic box is an ordinary configuration section whose name is
// BoxConfig_<id>.  The whole driver (Box_CreateEx, Process_Create,
// Conf_Get, SbieDll bootstrap) works on a section name as a box name, so
// the dynamic layer only manages three things:
//
//      - the handle <-> section-name mapping (BOX_DYN_ENTRY list)
//      - the lifecycle of the temp section in Conf_Data
//      - the live-process count of each box, for auto teardown
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
//          [BoxConfig]              the box section itself (renamed to the
//                                   generated BoxConfig_<id>)
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


typedef struct _BOX_DYN_ENTRY {

    LIST_ELEM list_elem;

    ULONG64 handle;                     // == id used in the section name
    WCHAR section[BOXNAME_COUNT];       // L"BoxConfig_<id>"

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

static NTSTATUS BoxDyn_ResetTemplateSection(const WCHAR *name);

static void BoxDyn_DiscardTemplateSections(void);

static NTSTATUS BoxDyn_LoadConfigText(
    const WCHAR *section, WCHAR *text, BOOLEAN *enabled_seen);


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


static LIST BoxDyn_List;
static PERESOURCE BoxDyn_Lock = NULL;
static BOOLEAN BoxDyn_Initialized = FALSE;

static volatile LONG64 BoxDyn_NextId = 0;

static const WCHAR *BoxDyn_Enabled = L"Enabled";
static const WCHAR *BoxDyn_Yes = L"y";


//---------------------------------------------------------------------------
// BoxDynamic_Init
//---------------------------------------------------------------------------


_FX BOOLEAN BoxDynamic_Init(void)
{
    List_Init(&BoxDyn_List);

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

        // the conf sections themselves are freed with the Conf_Data pool
        // in Conf_Unload; here we only release the handle-table entries

        if (entry->sid)
            Mem_Free(entry->sid, entry->sid_len);
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));

        entry = next_entry;
    }

    List_Init(&BoxDyn_List);

    Mem_FreeLockResource(&BoxDyn_Lock);
    BoxDyn_Lock = NULL;
}


//---------------------------------------------------------------------------
// BoxDyn_Find_locked
//---------------------------------------------------------------------------


_FX BOX_DYN_ENTRY *BoxDyn_Find_locked(ULONG64 handle)
{
    BOX_DYN_ENTRY *entry = List_Head(&BoxDyn_List);
    while (entry) {
        if (entry->handle == handle)
            return entry;
        entry = List_Next(entry);
    }
    return NULL;
}


//---------------------------------------------------------------------------
// BoxDyn_FindBySection_locked
//---------------------------------------------------------------------------


_FX BOX_DYN_ENTRY *BoxDyn_FindBySection_locked(const WCHAR *section)
{
    BOX_DYN_ENTRY *entry = List_Head(&BoxDyn_List);
    while (entry) {
        if (_wcsicmp(entry->section, section) == 0)
            return entry;
        entry = List_Next(entry);
    }
    return NULL;
}


//---------------------------------------------------------------------------
// BoxDyn_TeardownEntry
//---------------------------------------------------------------------------
//
// Remove the entry from the handle table, delete the temp configuration
// section, and free the entry.  Idempotent (guarded by entry->unlinked);
// safe to call from the API path and from the process-exit hooks.
//


_FX void BoxDyn_TeardownEntry(BOX_DYN_ENTRY *entry)
{
    BOOLEAN unlink;
    NTSTATUS status;
    ULONG retries;

    KIRQL irql;
    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    if (entry->unlinked)
        unlink = FALSE;
    else {
        entry->unlinked = TRUE;
        List_Remove(&BoxDyn_List, entry);
        unlink = TRUE;
    }

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! unlink)
        return;

    //
    // delete the configuration section; a concurrent configuration
    // reader (Conf use_count != 0) makes Conf_DeleteTempSection return
    // STATUS_DEVICE_BUSY, retry briefly before giving up
    //

    for (retries = 0; retries < 5; ++retries) {

        status = Conf_DeleteTempSection(entry->section);
        if (status != STATUS_DEVICE_BUSY)
            break;

        {
            LARGE_INTEGER time;
            time.QuadPart = -(10 * 1000 * 10);     // 10ms
            KeDelayExecutionThread(KernelMode, FALSE, &time);
        }
    }

    if (! NT_SUCCESS(status))
        DbgPrint("SbieDynBox: teardown of %S left section behind (%X)\n",
                 entry->section, status);
    else
        DbgPrint("SbieDynBox: destroyed dynamic box %S\n", entry->section);

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
// else in a [header] is rejected, so a blob cannot inject arbitrary
// sections into Conf_Data (in particular not another BoxConfig_* box).
//


static const WCHAR *BoxDyn_TemplateSections[2] = {
    L"TemplateDefaultPaths", L"TemplateNetworkPaths"
};


//---------------------------------------------------------------------------
// BoxDyn_ResetTemplateSection
//---------------------------------------------------------------------------
//
// Prepare the virtual conf section for one skeleton section of the blob:
// delete a previously installed virtual copy (idempotent replace -- each
// blob is authoritative and self-contained), then create it fresh and
// mark it template-origin so section enumeration keeps hiding it.
//
// A section that came from a real ini file is NOT replaced
// (Conf_DeleteTempSection refuses non-virtual sections): the ini wins,
// exactly like the old embedded-skeleton semantics.
//


_FX NTSTATUS BoxDyn_ResetTemplateSection(const WCHAR *name)
{
    NTSTATUS status;

    status = Conf_DeleteTempSection(name);

    if (status == STATUS_OBJECT_NAME_NOT_FOUND || NT_SUCCESS(status))
        status = STATUS_SUCCESS;
    else if (status == STATUS_ACCESS_DENIED)
        return STATUS_SUCCESS;      // provided by an ini file: ini wins

    if (! NT_SUCCESS(status))
        return status;              // e.g. STATUS_DEVICE_BUSY: retry later

    status = Conf_CreateTempSection(name);
    if (! NT_SUCCESS(status))
        return status;

    return Conf_MarkSectionTemplate(name);
}


//---------------------------------------------------------------------------
// BoxDyn_DiscardTemplateSections
//---------------------------------------------------------------------------
//
// Rollback helper for a failed API_BOX_CREATE: drop the virtual skeleton
// sections this blob may have (re)installed.  Non-virtual (ini) sections
// are refused by Conf_DeleteTempSection and thus survive.
//


_FX void BoxDyn_DiscardTemplateSections(void)
{
    ULONG i;

    for (i = 0; i < 2; ++i)
        Conf_DeleteTempSection(BoxDyn_TemplateSections[i]);
}


//---------------------------------------------------------------------------
// BoxDyn_LoadConfigText
//---------------------------------------------------------------------------
//
// Parse the multi-section KV blob into Conf_Data ("Key=Value" lines,
// \n or \r\n separated, '#' / ';' comments, repeated keys append).
//
//      [TemplateDefaultPaths] / [TemplateNetworkPaths]
//          -> their own virtual template sections (replaced if a previous
//             dynamic box installed a virtual copy; an ini copy wins)
//      [BoxConfig]
//          -> the generated box section
//      no header before the first Key=Value line
//          -> legacy flat form: everything lands in the box section
//
// Any other [header] is rejected with STATUS_INVALID_PARAMETER, as is a
// Key line with an empty value.  Trailing and leading whitespace is
// trimmed like Conf_Read_Settings does.
//


_FX NTSTATUS BoxDyn_LoadConfigText(
    const WCHAR *section, WCHAR *text, BOOLEAN *enabled_seen)
{
    static const WCHAR *BoxSectionHeader = L"BoxConfig";

    NTSTATUS status = STATUS_SUCCESS;
    WCHAR *line = text;
    const WCHAR *target = section;      // box section (flat form default)

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
        // [section] header switches the target section
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

            if (_wcsicmp(hdr, BoxSectionHeader) == 0) {

                target = section;

            } else {

                ULONG i;
                BOOLEAN known = FALSE;

                for (i = 0; i < 2; ++i) {
                    if (_wcsicmp(hdr, BoxDyn_TemplateSections[i]) == 0) {
                        known = TRUE;
                        status = BoxDyn_ResetTemplateSection(hdr);
                        if (NT_SUCCESS(status))
                            target = BoxDyn_TemplateSections[i];
                        break;
                    }
                }

                if (! known)
                    status = STATUS_INVALID_PARAMETER;
                if (! NT_SUCCESS(status))
                    break;
            }

            goto next_line;
        }

        //
        // split Key=Value
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
        // append to the current target section (list semantics for
        // repeated keys); Enabled counts only in the box section
        //

        if (target == section && _wcsicmp(name, BoxDyn_Enabled) == 0)
            *enabled_seen = TRUE;

        status = Conf_AddTempSetting(target, name, value);
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
    // create the temp section and parse the KV stream into it
    //

    status = Conf_CreateTempSection(entry->section);
    if (NT_SUCCESS(status)) {

        BOOLEAN enabled_seen = FALSE;

        status = BoxDyn_LoadConfigText(entry->section, text, &enabled_seen);

        //
        // classic process start checks Enabled=y via Conf_IsBoxEnabled;
        // force it unless the config supplied its own
        //

        if (NT_SUCCESS(status) && (! enabled_seen))
            status = Conf_AddTempSetting(
                        entry->section, BoxDyn_Enabled, BoxDyn_Yes);
    }

    Mem_Free(text, text_len);

    if (! NT_SUCCESS(status)) {
        BoxDyn_DiscardTemplateSections();   // partial skeleton rollback
        Conf_DeleteTempSection(entry->section);
        Mem_Free(entry->sid, entry->sid_len);
        Mem_Free(entry, sizeof(BOX_DYN_ENTRY));
        return status;
    }

    //
    // publish the handle
    //

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    if (! BoxDyn_Initialized)
        status = STATUS_SERVER_DISABLED;
    else if (List_Count(&BoxDyn_List) >= BOX_DYN_MAX_BOXES)
        status = STATUS_ALLOTTED_SPACE_EXCEEDED;   // too many dynamic boxes
    else
        List_Insert_After(&BoxDyn_List, NULL, entry);

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);

    if (! NT_SUCCESS(status)) {
        BoxDyn_DiscardTemplateSections();
        Conf_DeleteTempSection(entry->section);
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
// names a live dynamic box, its live-process count is raised, so the
// auto-teardown rule ("last process gone") sees descendants too.
//


_FX void BoxDynamic_OnProcessCreate(const WCHAR *box_name)
{
    BOX_DYN_ENTRY *entry;
    KIRQL irql;

    if ((! BoxDyn_Initialized) || (! box_name))
        return;

    if (_wcsnicmp(box_name, BOX_DYN_PREFIX, 10) != 0)
        return;     // not a dynamic box

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(BoxDyn_Lock, TRUE);

    entry = BoxDyn_FindBySection_locked(box_name);
    if (entry && (! entry->unlinked) && (! entry->destroying))
        ++entry->proc_count;

    ExReleaseResourceLite(BoxDyn_Lock);
    KeLowerIrql(irql);
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
