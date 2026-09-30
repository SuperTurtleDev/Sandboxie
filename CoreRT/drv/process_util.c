/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 * Copyright 2020 David Xanatos, xanasoft.com
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
// Process Management:  Various Utilities
//---------------------------------------------------------------------------


#include "process.h"
#include "conf.h"
#include "file.h"
#include "token.h"
#include "api.h"
#include "obj.h"
#include "box_dynamic.h"
#include "common/pattern.h"
#include "common/my_version.h"


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


static BOOLEAN Process_MatchImageGroup(
    BOX *box, const WCHAR *group, ULONG group_len, const WCHAR *test_str,
    ULONG depth);

static BOOLEAN Process_AddPath_2(
    PROCESS *proc, PATH_SET *list, const WCHAR *value, const WCHAR *setting_name,
    BOOLEAN AddFirst, BOOLEAN AddStar,
    BOOLEAN RemoveBackslashes, BOOLEAN CheckReparse, BOOLEAN* Reparsed, ULONG Level);


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------

static const WCHAR *Process_Normal = L"Normal";
static const WCHAR *Process_Open   = L"Open";
static const WCHAR *Process_Read   = L"Read";
static const WCHAR *Process_Write  = L"Write";
static const WCHAR *Process_Closed = L"Closed";


//---------------------------------------------------------------------------
// Process_IsSameBox
//---------------------------------------------------------------------------


_FX BOOLEAN Process_IsSameBox(
    PROCESS *proc, PROCESS *proc2, ULONG_PTR proc2_pid)
{
    KIRQL irql;
    BOOLEAN ok;
    BOOLEAN locked;

    if (proc2)
        locked = FALSE;
    else {
        proc2 = Process_Find((HANDLE)(ULONG_PTR)proc2_pid, &irql);
        locked = TRUE;
    }

    if (proc2 == proc) {

        //
        // write access is permitted into the same process
        //

        ok = TRUE;

    } else if (proc2 && (! proc2->terminated) && (! proc2->untouchable)) {

        //
        // when host image protection is enabled, sandboxed processes 
        // are protected form being written to by other sandboxed processes even from the same box
        // 
        // Note: this restriction will prevent images located in the sandbox from
        // starting not images located on the host
        //

        if (!proc2->image_from_box && proc2->protect_host_images && proc->image_from_box)
            ok = FALSE;
        else

        //
        // write access is only permitted within the same sandbox
        // and same session
        //

        ok = (proc->box->session_id == proc2->box->session_id) &&
             (proc->box->name_len == proc2->box->name_len)
          && (_wcsicmp(proc->box->name, proc2->box->name) == 0);

    } else {

        //
        // deny write access
        //

        ok = FALSE;
    }

    if (locked) {

        ExReleaseResourceLite(Process_ListLock);
        KeLowerIrql(irql);
    }

    return ok;
}


//---------------------------------------------------------------------------
// Process_IsStarter
//---------------------------------------------------------------------------

#ifdef DRV_BREAKOUT
_FX BOOLEAN Process_IsStarter(
    PROCESS* proc1, PROCESS* proc2)
{
    if (proc1->create_time > proc2->create_time)
        return FALSE; // reused pid? the new process can not be older than the on that started it

    if (proc1->box->session_id != proc2->box->session_id)
        return FALSE; // SID must be same

    return proc1->pid == proc2->starter_id;
}
#endif

//---------------------------------------------------------------------------
// PATH_SET helpers (approved optimization #1: first-segment bucketing)
//---------------------------------------------------------------------------


_FX void Process_PathSetInit(PATH_SET *set)
{
    List_Init(&set->wild_patterns);
    List_Init(&set->buckets);
}


_FX void Process_PathSetFirstSeg(
    const WCHAR *path, ULONG path_len,
    WCHAR *seg, ULONG *seg_len)
{
    //
    // first segment = everything before the first backslash, with
    // leading backslashes skipped.  "\Device\HarddiskVolume3\X" gives
    // "device";  "a:\x" gives "a:";  a path with no backslash gives
    // the whole string.  Capped at PATH_SET_SEG_MAX so build and
    // query sides always agree on long segments.
    //

    ULONG i = 0, n = 0;

    while (i < path_len && path[i] == L'\\')
        ++i;

    for (; i < path_len && path[i] != L'\\' && n < PATH_SET_SEG_MAX; ++i)
        seg[n++] = path[i];

    seg[n] = L'\0';
    *seg_len = n;

    for (i = 0; i < n; ++i) {
        WCHAR c = seg[i];
        if (c >= L'A' && c <= L'Z')
            seg[i] = c - L'A' + L'a';
    }
}


_FX BOOLEAN Process_PathSetAdd(
    POOL *pool, PATH_SET *set, PATTERN *pat, BOOLEAN AddFirst)
{
    const WCHAR *src = Pattern_Source(pat);
    WCHAR seg[PATH_SET_SEG_MAX + 1];
    ULONG seg_len;
    ULONG i;
    BOOLEAN wild = FALSE;
    PATH_BUCKET *bucket;

    Process_PathSetFirstSeg(src, wcslen(src), seg, &seg_len);

    for (i = 0; i < seg_len; ++i) {
        if (seg[i] == L'*' || seg[i] == L'?') {
            wild = TRUE;
            break;
        }
    }

    if (wild) {
        if (AddFirst)
            List_Insert_Before(&set->wild_patterns, NULL, pat);
        else
            List_Insert_After(&set->wild_patterns, NULL, pat);
        return TRUE;
    }

    bucket = Process_PathSetFindBucket(set, seg, seg_len);
    if (! bucket) {

        bucket = Mem_Alloc(pool, sizeof(PATH_BUCKET));
        if (! bucket)
            return FALSE;

        memzero(bucket, sizeof(PATH_BUCKET));
        wcscpy(bucket->first, seg);
        List_Init(&bucket->patterns);

        List_Insert_After(&set->buckets, NULL, bucket);
    }

    if (AddFirst)
        List_Insert_Before(&bucket->patterns, NULL, pat);
    else
        List_Insert_After(&bucket->patterns, NULL, pat);

    return TRUE;
}


_FX PATH_BUCKET *Process_PathSetFindBucket(
    PATH_SET *set, const WCHAR *seg, ULONG seg_len)
{
    PATH_BUCKET *bucket = List_Head(&set->buckets);
    while (bucket) {
        if (_wcsicmp(bucket->first, seg) == 0)
            return bucket;
        bucket = List_Next(bucket);
    }
    return NULL;
}


_FX PATTERN *Process_PathSetPop(PATH_SET *set)
{
    //
    // unlink and return the first pattern of the set (wild bucket
    // first, then each bucket in order).  Used by the ClosedXxx
    // discard pass, which moves survivors into the live set.
    //

    PATH_BUCKET *bucket;
    PATTERN *pat;

    pat = List_Head(&set->wild_patterns);
    if (pat) {
        List_Remove(&set->wild_patterns, pat);
        return pat;
    }

    bucket = List_Head(&set->buckets);
    while (bucket) {

        pat = List_Head(&bucket->patterns);
        if (pat) {
            List_Remove(&bucket->patterns, pat);
            return pat;
        }

        bucket = List_Next(bucket);
    }

    return NULL;
}


_FX void Process_PathSetPurge(PATH_SET *set)
{
    //
    // free every pattern in the set and the buckets themselves.
    // the patterns (and buckets) live in the process pool, which is
    // freed wholesale at process teardown -- this purge is for the
    // path-list refresh API, so the pool is still alive; pattern
    // nodes carry their own allocation and are freed via Pattern_Free
    // while the bucket shells are only freed when empty (bucket
    // nodes are small, keeping them would also be fine).
    //

    PATH_BUCKET *bucket;
    PATTERN *pat;

    while (1) {
        pat = List_Head(&set->wild_patterns);
        if (! pat)
            break;
        List_Remove(&set->wild_patterns, pat);
        Pattern_Free(pat);
    }

    while (1) {

        bucket = List_Head(&set->buckets);
        if (! bucket)
            break;

        while (1) {
            pat = List_Head(&bucket->patterns);
            if (! pat)
                break;
            List_Remove(&bucket->patterns, pat);
            Pattern_Free(pat);
        }

        List_Remove(&set->buckets, bucket);
        Mem_Free(bucket, sizeof(PATH_BUCKET));
    }

    Process_PathSetInit(set);
}


//---------------------------------------------------------------------------
// Process_MatchImage
//---------------------------------------------------------------------------


_FX BOOLEAN Process_MatchImage(
    BOX *box, const WCHAR *pat_str, ULONG pat_len, const WCHAR *test_str,
    ULONG depth)
{
    PATTERN *pat;
    WCHAR *tmp, *expnd;
    ULONG tmp_len;
    BOOLEAN ok;

    //
    // if pat_len was specified, we should create the match pattern
    // using only the first pat_len characters of pat_str
    //

    if (pat_len) {

        tmp_len = (pat_len + 1) * sizeof(WCHAR);
        tmp = Mem_Alloc(box->expand_args->pool, tmp_len);
        if (! tmp)
            return FALSE;

        wcsncpy(tmp, pat_str, pat_len);
        tmp[pat_len] = L'\0';

        expnd = Conf_Expand(box->expand_args, tmp, NULL);

        Mem_Free(tmp, tmp_len);

    } else {

        expnd = Conf_Expand(box->expand_args, pat_str, NULL);
    }

    if (! expnd)
        return FALSE;

    pat = Pattern_Create(box->expand_args->pool, expnd, TRUE, 0);

    Mem_FreeString(expnd);

    if (! pat)
        return FALSE;

    //
    //
    //

    if (*pat_str == L'<') {

        Conf_AdjustUseCount(TRUE);

        ok = Process_MatchImageGroup(
                box, Pattern_Source(pat), 0, test_str, depth + 1);

        Conf_AdjustUseCount(FALSE);

        Pattern_Free(pat);

        return ok;
    }

    //
    // create a lower-case copy of test_str
    //

    ok = FALSE;

    tmp_len = (wcslen(test_str) + 1) * sizeof(WCHAR);
    tmp = Mem_Alloc(box->expand_args->pool, tmp_len);
    if (tmp) {

        memcpy(tmp, test_str, tmp_len);
        _wcslwr(tmp);

        ok = Pattern_Match(pat, tmp, wcslen(tmp));

        Mem_Free(tmp, tmp_len);
    }

    Pattern_Free(pat);

    return ok;
}


//---------------------------------------------------------------------------
// Process_MatchImageGroup
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Process_GetFlatGroup  (approved optimization #2)
//---------------------------------------------------------------------------
//
// Return the flattened member list of a ProcessGroup:  a pointer to a
// comma-separated, recursively-expanded member string, cached on the
// BOX.  The first lookup walks the ProcessGroup settings once (and
// recurses into nested groups, depth-capped like the classic code);
// every later lookup is one list walk over the tiny cache.
//
// The returned string is allocated from the box expand_args pool and
// stays valid for the lifetime of the BOX.  NULL means "not found".
//


typedef struct _FLAT_GROUP {

    LIST_ELEM list_elem;

    WCHAR *name;                        // group name, without <>
    WCHAR *members;                     // flattened, comma separated

} FLAT_GROUP;


_FX const WCHAR *Process_GetFlatGroup(
    BOX *box, const WCHAR *group, ULONG group_len, ULONG depth)
{
    static const WCHAR *ProcessGroupSetting = L"ProcessGroup";

    FLAT_GROUP *node;
    const WCHAR *flat = NULL;
    WCHAR *build = NULL;
    ULONG build_len = 0;
    ULONG index;
    POOL *pool;

    //
    // cache lookup
    //

    node = List_Head(&box->flat_groups);
    while (node) {
        if (_wcsicmp(node->name, group) == 0)
            return node->members;
        node = List_Next(node);
    }

    //
    // walk every ProcessGroup setting; concatenate the member lists
    // of the (possibly several) definitions of this group
    //

    Conf_AdjustUseCount(TRUE);

    for (index = 0; ; ++index) {

        ULONG value_len;
        const WCHAR *value = Conf_Get(box->name, ProcessGroupSetting, index);
        if (! value)
            break;

        value_len = wcslen(value);
        if (value_len <= group_len + 1)
            continue;
        if (_wcsnicmp(value, group, group_len) != 0)
            continue;

        value += group_len;
        if (*value != L',')
            continue;
        ++value;

        //
        // append this definition's members, expanding nested groups
        //

        while (*value) {

            WCHAR *ptr = wcschr(value, L',');
            ULONG member_len = ptr
                ? (ULONG)(ULONG_PTR)(ptr - value) : wcslen(value);

            if (member_len) {

                if (value[0] == L'<' && depth < 6) {

                    //
                    // nested group:  flatten it into this list.  the
                    // child result is NOT cached under its own name
                    // (the cache would need transitive invalidation);
                    // each distinct root group expansion walks the
                    // settings once, which is still the promised
                    // one-time flattening per group per box
                    //

                    WCHAR child[BOXNAME_COUNT + 4];
                    const WCHAR *child_flat;
                    ULONG child_len = member_len - 2;

                    if (child_len && child_len < BOXNAME_COUNT) {

                        wmemcpy(child, value + 1, child_len);
                        child[child_len] = L'\0';

                        child_flat = Process_GetFlatGroup(
                                        box, child, child_len, depth + 1);
                        if (child_flat && *child_flat) {

                            ULONG add_len = wcslen(child_flat);
                            WCHAR *nb = Mem_Alloc(
                                box->expand_args->pool,
                                (build_len + add_len + 4) * sizeof(WCHAR));
                            if (nb) {
                                if (build_len)
                                    wmemcpy(nb, build, build_len);
                                if (build_len && nb[build_len - 1] != L',')
                                    nb[build_len++] = L',';
                                wmemcpy(nb + build_len, child_flat, add_len);
                                build_len += add_len;
                                nb[build_len] = L'\0';
                                if (build)
                                    Mem_Free(build,
                                        (wcslen(build) + 1) * sizeof(WCHAR));
                                build = nb;
                            }
                        }
                    }

                } else {

                    ULONG add_len = member_len;
                    WCHAR *nb = Mem_Alloc(
                        box->expand_args->pool,
                        (build_len + add_len + 4) * sizeof(WCHAR));
                    if (nb) {
                        if (build_len)
                            wmemcpy(nb, build, build_len);
                        if (build_len && nb[build_len - 1] != L',')
                            nb[build_len++] = L',';
                        wmemcpy(nb + build_len, value, add_len);
                        build_len += add_len;
                        nb[build_len] = L'\0';
                        if (build)
                            Mem_Free(build,
                                (wcslen(build) + 1) * sizeof(WCHAR));
                        build = nb;
                    }
                }
            }

            value += member_len;
            while (*value == L',')
                ++value;
        }
    }

    Conf_AdjustUseCount(FALSE);

    //
    // cache the flattened list (an empty result caches as an empty
    // string so a missing group is not re-walked either)
    //

    pool = box->expand_args->pool;

    node = Mem_Alloc(pool, sizeof(FLAT_GROUP));
    if (! node) {
        if (build)
            Mem_Free(build, (wcslen(build) + 1) * sizeof(WCHAR));
        return flat;
    }

    node->name = Mem_Alloc(pool, (group_len + 1) * sizeof(WCHAR));
    node->members = Mem_Alloc(pool, (build_len + 1) * sizeof(WCHAR));

    if (node->name && node->members) {

        wmemcpy(node->name, group, group_len);
        node->name[group_len] = L'\0';

        if (build_len)
            wmemcpy(node->members, build, build_len + 1);
        else
            node->members[0] = L'\0';

        List_Insert_After(&box->flat_groups, NULL, node);

        flat = node->members;

    } else {

        if (node->name)
            Mem_Free(node->name, (group_len + 1) * sizeof(WCHAR));
        if (node->members)
            Mem_Free(node->members, (build_len + 1) * sizeof(WCHAR));
        Mem_Free(node, sizeof(FLAT_GROUP));
    }

    if (build)
        Mem_Free(build, (wcslen(build) + 1) * sizeof(WCHAR));

    return flat;
}


//---------------------------------------------------------------------------
// Process_MatchImageGroup
//---------------------------------------------------------------------------


_FX BOOLEAN Process_MatchImageGroup(
    BOX *box, const WCHAR *group, ULONG group_len, const WCHAR *test_str,
    ULONG depth)
{
    BOOLEAN match = FALSE;
    const WCHAR *flat;
    const WCHAR *value;

    if (! group_len)
        group_len = wcslen(group);

    Conf_AdjustUseCount(TRUE);

    //
    // (optimization #2) one flattened, cached member list instead of
    // the per-call recursive settings walk
    //

    flat = Process_GetFlatGroup(box, group, group_len, depth);
    if (flat && *flat) {

        value = flat;
        while (*value && (! match)) {

            ULONG value_len;
            WCHAR *ptr = wcschr(value, L',');
            if (ptr)
                value_len = (ULONG)(ULONG_PTR)(ptr - value);
            else
                value_len = wcslen(value);

            if (value_len)
                match = Process_MatchImage(
                            box, value, value_len, test_str, depth + 1);

            value += value_len;
            while (*value == L',')
                ++value;
        }
    }

    Conf_AdjustUseCount(FALSE);

    return match;
}


//---------------------------------------------------------------------------
// Process_MatchImageAndGetValue
//---------------------------------------------------------------------------


_FX const WCHAR* Process_MatchImageAndGetValue(BOX *box, const WCHAR* value, const WCHAR* ImageName, ULONG* pLevel)
{
    WCHAR* tmp;
    ULONG len;

    //
    // if the setting indicates an image name followed by a comma,
    // then match the image name against the executing process.
    //

    tmp = wcschr(value, L',');
    if (tmp) {

        BOOLEAN inv, match;

        //
        // exclamation marks negates the matching
        //

        if (*value == L'!') {
            inv = TRUE;
            ++value;
        } else
            inv = FALSE;

        len = (ULONG)(tmp - value);
        if (len) {
            match = Process_MatchImage(box, value, len, ImageName, 1);
            if (inv)
                match = !match;
            if (!match)
                return NULL;
            else if (pLevel) {
                if (len == 1 && *value == L'*')
                    *pLevel = 2; // 2 - match all 
                else
                    *pLevel = inv ? 1 : 0; // 1 - match by negation, 0 - exact match
            }
        }

        value = tmp + 1;
    }
    else {

        if (pLevel) *pLevel = 3; // 3 - global default
    }

    if (! *value)
        return NULL;

    return value;
}


//---------------------------------------------------------------------------
// Process_GetConfEx
//---------------------------------------------------------------------------


_FX const WCHAR* Process_GetConfEx(BOX *box, const WCHAR *image_name, const WCHAR* setting)
{
    ULONG index = 0;
    const WCHAR *value;
    const WCHAR *found_value = NULL;
    ULONG found_level = -1;

    for (index = 0; ; ++index) {

        value = Conf_Get(box->name, setting, index);
        if (! value)
            break;

        ULONG level = -1;
        value = Process_MatchImageAndGetValue(box, value, image_name, &level);
        if (!value || level > found_level)
            continue;
        found_value = value;
        found_level = level;
    }

    return found_value;
}


//---------------------------------------------------------------------------
// Process_GetConf
//---------------------------------------------------------------------------


_FX const WCHAR* Process_GetConf(PROCESS* proc, const WCHAR* setting)
{
    //
    // dynamic-box-arch (plan B):  handle-direct read from the entry's
    // hash store -- no Conf_Lock, no Conf_Data walk.  The image-name
    // matching semantics of Process_GetConfEx are kept (values may be
    // "program,value" qualified).
    //

    if (proc->box_dyn) {

        ULONG index = 0;
        const WCHAR *value;
        const WCHAR *found_value = NULL;
        ULONG found_level = -1;

        for (index = 0; ; ++index) {

            value = BoxDyn_GetValue(proc->box_dyn, setting, index);
            if (! value)
                break;

            ULONG level = -1;
            value = Process_MatchImageAndGetValue(
                        proc->box, value, proc->image_name, &level);
            if (!value || level > found_level)
                continue;
            found_value = value;
            found_level = level;
        }

        return found_value;
    }

    return Process_GetConfEx(proc->box, proc->image_name, setting);
}


//---------------------------------------------------------------------------
// Process_GetConfX  (dynamic-box-arch, plan B)
//---------------------------------------------------------------------------
//
// Raw indexed read (list semantics, no image-name matching) -- the
// drop-in for the Conf_Get(box->name, setting, index) call pattern:
// dynamic box -> entry hash store; static box -> Conf_Get.
//


_FX const WCHAR* Process_GetConfX(
    PROCESS *proc, const WCHAR *setting, ULONG index)
{
    if (proc->box_dyn)
        return BoxDyn_GetValue(proc->box_dyn, setting, index);

    return Conf_Get(proc->box->name, setting, index);
}


//---------------------------------------------------------------------------
// Process_GetConfEx_bool
//---------------------------------------------------------------------------


_FX BOOLEAN Process_GetConfEx_bool(BOX *box, const WCHAR *image_name, const WCHAR* setting, BOOLEAN def)
{
    const WCHAR *value;
    BOOLEAN retval;

    Conf_AdjustUseCount(TRUE);

    value = Process_GetConfEx(box, image_name, setting);

    retval = def;
    if (value) {
        if (*value == 'y' || *value == 'Y')
            retval = TRUE;
        else if (*value == 'n' || *value == 'N')
            retval = FALSE;
    }

    Conf_AdjustUseCount(FALSE);

    return retval;
}


//---------------------------------------------------------------------------
// Process_GetConf_bool
//---------------------------------------------------------------------------


_FX BOOLEAN Process_GetConf_bool(PROCESS* proc, const WCHAR* setting, BOOLEAN def)
{
    if (proc->box_dyn) {

        const WCHAR *value = Process_GetConf(proc, setting);

        if (value) {
            if (*value == L'y' || *value == L'Y')
                return TRUE;
            if (*value == L'n' || *value == L'N')
                return FALSE;
        }

        return def;
    }

    return Process_GetConfEx_bool(proc->box, proc->image_name, setting, def);
}


//---------------------------------------------------------------------------
// Process_GetConf_Bit  (dynamic-box-arch, plan B)
//---------------------------------------------------------------------------
//
// Boolean read by bitset:  one AND for a dynamic box, no hash walk, no
// string compare.  Static boxes fall back to the name-based read via
// the bit->name table in box_dynamic.c.
//
// NOTE:  the bitset cannot distinguish "explicitly n" from "unset" --
// both return def.  All in-tree callers pass def=FALSE.
//


_FX BOOLEAN Process_GetConf_Bit(PROCESS *proc, UINT64 bit, BOOLEAN def)
{
    if (proc && proc->box_dyn)
        return (BoxDyn_GetBit(proc->box_dyn, bit) || def);

    {
        const WCHAR *name = BoxDyn_BoolName(bit);
        if (! name)
            return def;
        return Process_GetConf_bool(proc, name, def);
    }
}


//---------------------------------------------------------------------------
// Process_GetPaths
//---------------------------------------------------------------------------


_FX BOOLEAN Process_GetPaths(
    PROCESS *proc, PATH_SET *list, const WCHAR *section_name, const WCHAR *setting_name, BOOLEAN AddStar)
{
    ULONG index;
    const WCHAR *value;
    BOOLEAN ok = TRUE;

    BOOLEAN closed = (_wcsnicmp(setting_name, Process_Closed, 6) == 0);
    BOOLEAN closed_ipc = FALSE;
    if (closed)
        closed_ipc = (_wcsnicmp(setting_name + 6, L"Ipc", 3) == 0);

    //
    // dynamic-box-arch (plan B):  a dynamic box carries its skeleton
    // ([TemplateDefaultPaths] / [TemplateNetworkPaths]) and its own
    // path lists merged in the entry's hash store -- read them
    // handle-direct, no Conf_Data involved.  Static boxes keep the
    // Conf_Get walk (ini sections).
    //

    if (proc->box_dyn) {

        for (index = 0; ; ++index) {

            value = BoxDyn_GetValue(proc->box_dyn, setting_name, index);
            if (! value)
                break;

            if (closed && (*value == L'!')) {
                if (closed_ipc && proc->image_sbie)
                    continue;
                if (proc->image_from_box && proc->always_close_for_boxed) {
                    value = wcschr(value, L',');
                    if (! value)
                        continue;
                    ++value;
                }
            }

            if (! Process_AddPath(
                            proc, list, setting_name, FALSE, value, AddStar)) {
                ok = FALSE;
                break;
            }
        }

        return ok;
    }

    Conf_AdjustUseCount(TRUE);

    for (index = 0; ; ++index) {

        //
        // get next configuration setting for this path list
        //

        value = Conf_Get(section_name, setting_name, index);
        if (! value)
            break;

        if (closed && (*value == L'!')) {

            // don't close paths for sbie components
            if (closed_ipc && proc->image_sbie)
                continue;

            // for all other advance to the path and apply the block for all sandboxed images
            if (proc->image_from_box && proc->always_close_for_boxed) {

                value = wcschr(value, L',');
                if (! value)
                    continue;
                ++value;
            }
        }

        if (! Process_AddPath(
                        proc, list, setting_name, FALSE, value, AddStar)) {
            ok = FALSE;
            break;
        }
    }

    Conf_AdjustUseCount(FALSE);

    return ok;
}


//---------------------------------------------------------------------------
// Process_GetPaths2
//---------------------------------------------------------------------------


#ifndef USE_MATCH_PATH_EX
_FX BOOLEAN Process_GetPaths2(
    PROCESS *proc, LIST *list, LIST *list2,
    const WCHAR *setting_name, BOOLEAN AddStar)
{
    LIST dummy_list;
    PATTERN *pat;
    const WCHAR *value;
    ULONG len;
    BOOLEAN is_open, is_closed;

    //
    // this function gets a list of settings, typically WriteXxxPath,
    // and compares it to an already-populated list of settings, typically
    // ClosedXxxPath, in order to discard duplicate settings.  the intent
    // is to keep the general rule that ClosedXxxPath settings override
    // any other settings, including WriteXxxPath settings
    //

    Process_PathSetInit(&dummy_list);
    if (! Process_GetPaths(proc, &dummy_list, setting_name, AddStar))
        return FALSE;

    while (1) {

        pat = List_Head(&dummy_list);
        if (! pat)
            break;

        //
        // get a setting from the list of potential settings,
        // and discard suffix wildcards
        //

        value = Pattern_Source(pat);
        for (len = wcslen(value); len && value[len - 1] == L'*'; --len)
            ;

        if (! len)
            is_closed = TRUE;
        else {

            Process_MatchPath(proc->pool, value, len,
                              NULL, list2, &is_open, &is_closed);
        }

        List_Remove(&dummy_list, pat);

        if (is_closed)
            Pattern_Free(pat);
        else
            List_Insert_After(list, NULL, pat);
    }

    return TRUE;
}
#endif


//---------------------------------------------------------------------------
// Process_GetTemplatePaths
//---------------------------------------------------------------------------


#ifdef USE_TEMPLATE_PATHS
BOOLEAN Process_GetTemplatePaths(PROCESS *proc, PATH_SET *list, const WCHAR *setting_name)
{
    BOOLEAN ok;

    //
    // (dynamic-box-arch) the two skeleton sections come from the per-box
    // configuration:  API_BOX_CREATE writes [TemplateDefaultPaths] (and
    // [TemplateNetworkPaths]) into Conf_Data from the caller's blob, so a
    // runtime deployment needs no Templates.ini and the driver embeds no
    // default tables.  [TemplateNetworkPaths] is consumed by
    // File_InitPaths' InternetAccessDevices gate (network open/block), not
    // here.  The old mode-driven sections -- SMod / PMod / AppC -- are gone:
    // their rules live in the box section as plain keys.
    //

    ok = Process_GetPaths(proc, list, L"TemplateDefaultPaths", setting_name, FALSE);

    return ok;
}
#endif


//---------------------------------------------------------------------------
// Process_AddPath
//---------------------------------------------------------------------------


_FX BOOLEAN Process_AddPath(
    PROCESS *proc, PATH_SET *list, const WCHAR *setting_name,
    BOOLEAN AddFirst, const WCHAR *value, BOOLEAN AddStar)
{
    WCHAR *tmp;
    ULONG len;
    BOOLEAN RemoveBackslashes = FALSE;
    BOOLEAN CheckReparse = FALSE;
    BOOLEAN AddDrives = FALSE;
    BOOLEAN Reparsed;
    BOOLEAN ok;
    ULONG Level;

    //
    // if this is a file/pipe/key setting, remove duplicate backslashes
    // if this is a file setting, also check the path for reparse points
    //

    if (setting_name) {

        const WCHAR *setting_name_ptr = setting_name;
        if (_wcsnicmp(setting_name, Process_Normal, 6) == 0 ||
            _wcsnicmp(setting_name, Process_Closed, 6) == 0)
            setting_name_ptr = setting_name + 6;
        else if (_wcsnicmp(setting_name, Process_Write, 5) == 0)
            setting_name_ptr = setting_name + 5;
        else if (_wcsnicmp(setting_name, Process_Read, 4) == 0 ||
                 _wcsnicmp(setting_name, Process_Open, 4) == 0)
            setting_name_ptr = setting_name + 4;
        else
            setting_name_ptr = NULL;

        if (setting_name_ptr) {

            if (_wcsnicmp(setting_name_ptr, L"Key", 3) == 0
                  || _wcsnicmp(setting_name_ptr, L"Conf", 4) == 0) {
                if (AddStar) {
                    RemoveBackslashes = TRUE;
                }
            } else if (_wcsnicmp(setting_name_ptr, L"File", 4) == 0
                  || _wcsnicmp(setting_name_ptr, L"Pipe", 4) == 0) {
                if (AddStar) {
                    RemoveBackslashes = TRUE;
                    CheckReparse = TRUE;
                }
                AddDrives = TRUE;
            }
        }
    }

    value = Process_MatchImageAndGetValue(proc->box, value, proc->image_name, &Level);

    if (!value)
        return TRUE;

    //
    // image name matches, or was not specified.  next, expand
    // the configuration path setting.
    //
    // note that if we're removing backslashes (i.e. for file/pipe/key)
    // and the setting value begins with a pipe character, then we
    // do not append a suffix wildcard character
    //

    if (RemoveBackslashes && *value == L'|') {
        ++value;
        AddStar = FALSE;
    }

    //
    // add the value as requested
    //

    ok = Process_AddPath_2(proc, list, value, setting_name,
               AddFirst, AddStar, RemoveBackslashes, CheckReparse, &Reparsed, Level);
    if (ok && CheckReparse && Reparsed) {
        //
        // If the path was reparsed, add also the original path to the list
        //
        ok = Process_AddPath_2(proc, list, value, setting_name,
               AddFirst, AddStar, RemoveBackslashes, FALSE, NULL, Level);
    }

    //
    // if this is a file setting (AddDrives == TRUE) and starts with
    // *: or ?: then manually replace with each of the 26 possible drives
    //

    if (ok && AddDrives && (value[0] == L'?' || value[0] == L'*')
                           && (value[1] == L':')) {

        tmp = Mem_AllocString(proc->pool, value);
        if (! tmp)
            return FALSE;
        for (len = L'A'; (len <= L'Z') && ok; ++len) {
            *tmp = (WCHAR)len;
            ok = Process_AddPath_2(proc, list, tmp, setting_name,
                   AddFirst, AddStar, RemoveBackslashes, CheckReparse, &Reparsed, Level);
            if (ok && CheckReparse && Reparsed) {
                ok = Process_AddPath_2(proc, list, tmp, setting_name,
                       AddFirst, AddStar, RemoveBackslashes, FALSE, NULL, Level);
            }
        }
        Mem_FreeString(tmp);
    }

    return ok;
}


//---------------------------------------------------------------------------
// Process_AddPath_2
//---------------------------------------------------------------------------


_FX BOOLEAN Process_AddPath_2(
    PROCESS *proc, PATH_SET *list, const WCHAR *value, const WCHAR *setting_name,
    BOOLEAN AddFirst, BOOLEAN AddStar,
    BOOLEAN RemoveBackslashes, BOOLEAN CheckReparse, BOOLEAN* Reparsed, ULONG Level)
{
    PATTERN *pat;
    WCHAR *expand, *tmp;
    ULONG len;

    //
    // expand any variables in the value
    //

    expand = Conf_Expand(proc->box->expand_args, value, setting_name);
    if (! expand)
        return FALSE;

    //
    // duplicate the expanded string as a temp string, in case we
    // need to add a star at the end
    //

    len = (wcslen(expand) + 1) * sizeof(WCHAR);
    if (AddStar) {
        if (wcschr(expand, L'*') == NULL)
            len += sizeof(WCHAR);
        else
            AddStar = FALSE;
    }

    tmp = Mem_Alloc(proc->pool, len);
    if (! tmp) {
        Mem_FreeString(expand);
        return FALSE;
    }

    //
    // copy the expanded path string into the temporary string
    // optionally, removing backslashes
    // optionally, adding a star at the end
    //

    if (RemoveBackslashes) {

        WCHAR *src_ptr = expand;
        WCHAR *dst_ptr = tmp;
        while (*src_ptr) {
            if (src_ptr[0] == L'\\' && src_ptr[1] == L'\\') {
                ++src_ptr;
                continue;
            }
            *dst_ptr = *src_ptr;
            ++src_ptr;
            ++dst_ptr;
        }
        *dst_ptr = L'\0';

    } else
        wcscpy(tmp, expand);

    if (AddStar)
        wcscat(tmp, L"*");

    //
    // check for reparse points
    //

    if (CheckReparse) {

        WCHAR *tmp2 = File_TranslateReparsePoints(tmp, proc->pool);
        if (tmp2) {
            *Reparsed = _wcsicmp(tmp, tmp2) != 0; // check if its actually different
            Mem_FreeString(tmp);
            tmp = tmp2;
        } else {
            *Reparsed = FALSE; // nothing found
        }
    }

    //
    // add the pattern
    //

    pat = Pattern_Create(proc->pool, tmp, TRUE, Level);
    if (pat) {

        //
        // bucket the pattern by its first path segment (approved
        // optimization #1); the flat single list is gone
        //

        if (! Process_PathSetAdd(proc->pool, list, pat, AddFirst)) {
            Pattern_Free(pat);
            pat = NULL;
        }
    }

    Mem_FreeString(tmp);
    Mem_FreeString(expand);

    if (! pat)
        return FALSE;

    return TRUE;
}


//---------------------------------------------------------------------------
// Process_MatchPath
//---------------------------------------------------------------------------


_FX const WCHAR *Process_MatchPath(
    POOL *pool, const WCHAR *path, ULONG path_len,
    PATH_SET *open_list, PATH_SET *closed_list,
    BOOLEAN *is_open, BOOLEAN *is_closed)
{
    PATTERN *pat;
    WCHAR *path_lwr;
    ULONG path_lwr_len;
    const WCHAR *patsrc = NULL;
    WCHAR seg[PATH_SET_SEG_MAX + 1];
    ULONG seg_len;

    *is_open = FALSE;
    *is_closed = FALSE;

    //
    // scan paths list.  if the path to match does not already end with
    // a backslash character, we will check it twice, second time with
    // a suffixing backslash.  this will make sure we match C:\X even
    // even when {Open,Closed}XxxPath=C:\X\ (with a backslash suffix)
    //
    // (optimization #1) each PATH_SET is walked as wildcard bucket +
    // the one bucket selected by the query's first path segment
    //

    path_lwr_len = (path_len + 4) * sizeof(WCHAR);
    path_lwr = Mem_Alloc(pool, path_lwr_len);
    if (! path_lwr)
        return NULL;

    wmemcpy(path_lwr, path, path_len);
    path_lwr[path_len]     = L'\0';
    path_len = wcslen(path_lwr);
    if (! path_len) {
        Mem_Free(path_lwr, path_lwr_len);
        return NULL;
    }
    path_lwr[path_len]     = L'\0';
    path_lwr[path_len + 1] = L'\0';
    _wcslwr(path_lwr);

    Process_PathSetFirstSeg(path_lwr, path_len, seg, &seg_len);

    if (closed_list) {

        PATH_BUCKET *bucket =
            Process_PathSetFindBucket(closed_list, seg, seg_len);

        pat = List_Head(&closed_list->wild_patterns);
        while (pat) {

            if (Pattern_Match(pat, path_lwr, path_len)) {
                *is_closed = TRUE;
                patsrc = Pattern_Source(pat);
                break;
            }

            if (path_lwr[path_len - 1] != L'\\') {
                path_lwr[path_len] = L'\\';
                if (Pattern_Match(pat, path_lwr, path_len + 1)) {
                    path_lwr[path_len] = L'\0';
                    *is_closed = TRUE;
                    patsrc = Pattern_Source(pat);
                    break;
                }
                path_lwr[path_len] = L'\0';
            }

            pat = List_Next(pat);
        }

        if ((! *is_closed) && bucket) {

            pat = List_Head(&bucket->patterns);
            while (pat) {

                if (Pattern_Match(pat, path_lwr, path_len)) {
                    *is_closed = TRUE;
                    patsrc = Pattern_Source(pat);
                    break;
                }

                if (path_lwr[path_len - 1] != L'\\') {
                    path_lwr[path_len] = L'\\';
                    if (Pattern_Match(pat, path_lwr, path_len + 1)) {
                        path_lwr[path_len] = L'\0';
                        *is_closed = TRUE;
                        patsrc = Pattern_Source(pat);
                        break;
                    }
                    path_lwr[path_len] = L'\0';
                }

                pat = List_Next(pat);
            }
        }
    }

    if (open_list && (! *is_closed)) {

        PATH_BUCKET *bucket =
            Process_PathSetFindBucket(open_list, seg, seg_len);

        pat = List_Head(&open_list->wild_patterns);
        while (pat) {

            if (Pattern_Match(pat, path_lwr, path_len)) {
                *is_open = TRUE;
                patsrc = Pattern_Source(pat);
                break;
            }

            if (path_lwr[path_len - 1] != L'\\') {
                path_lwr[path_len] = L'\\';
                if (Pattern_Match(pat, path_lwr, path_len + 1)) {
                    path_lwr[path_len] = L'\0';
                    *is_open = TRUE;
                    patsrc = Pattern_Source(pat);
                    break;
                }
                path_lwr[path_len] = L'\0';
            }

            pat = List_Next(pat);
        }

        if ((! *is_open) && bucket) {

            pat = List_Head(&bucket->patterns);
            while (pat) {

                if (Pattern_Match(pat, path_lwr, path_len)) {
                    *is_open = TRUE;
                    patsrc = Pattern_Source(pat);
                    break;
                }

                if (path_lwr[path_len - 1] != L'\\') {
                    path_lwr[path_len] = L'\\';
                    if (Pattern_Match(pat, path_lwr, path_len + 1)) {
                        path_lwr[path_len] = L'\0';
                        *is_open = TRUE;
                        patsrc = Pattern_Source(pat);
                        break;
                    }
                    path_lwr[path_len] = L'\0';
                }

                pat = List_Next(pat);
            }
        }
    }

    Mem_Free(path_lwr, path_lwr_len);
    return patsrc;
}


//---------------------------------------------------------------------------
// Process_MatchPathEx
//---------------------------------------------------------------------------

#ifdef USE_MATCH_PATH_EX
_FX ULONG Process_MatchPathEx(
    PROCESS *proc, const WCHAR *path, ULONG path_len, WCHAR path_code,
    PATH_SET *normal_list,
    PATH_SET *open_list, PATH_SET *closed_list,
    PATH_SET *read_list, PATH_SET *write_list,
    const WCHAR** patsrc)
{
    WCHAR *path_lwr;
    ULONG path_lwr_len;
    int match_len;
    ULONG level;
    ULONG flags;
    USHORT wildc;
    ULONG mp_flags;
    WCHAR seg[PATH_SET_SEG_MAX + 1];
    ULONG seg_len;
    PATH_BUCKET *bucket;

    path_lwr_len = (path_len + 4) * sizeof(WCHAR);
    path_lwr = Mem_Alloc(proc->pool, path_lwr_len);
    if (! path_lwr)
        return 0;

    wmemcpy(path_lwr, path, path_len);
    path_lwr[path_len]     = L'\0';
    path_len = wcslen(path_lwr);
    if (! path_len) {
        Mem_Free(path_lwr, path_lwr_len);
        return 0;
    }
    path_lwr[path_len]     = L'\0';
    path_lwr[path_len + 1] = L'\0';
    _wcslwr(path_lwr);

    //
    // (optimization #1) pre-select the one bucket every list consults:
    // each Pattern_MatchPathListEx below runs twice per list -- once
    // over the wildcard patterns and once over the first-segment
    // bucket.  The best-match accumulators (level / match_len /
    // flags / wildc) compose across the two calls, so the semantics
    // of the flat walk are preserved exactly.
    //

    Process_PathSetFirstSeg(path_lwr, path_len, seg, &seg_len);

    //
    // Rule priorities are implemented based on their specificity and match level with the process.
    // The specificity describes how well a pattern matches a given path,
    // i.e. how many characters of the path it matches, disregarding the last wild card.
    // The process match level describes in which way a rule applies to a given process:
    //  0 - exact match, eg. ...Path=program.exe,...
    //  1 - match by negation, eg. ...Path=!program.exe,...
    //  2 - match all, eg. ...Path=*,...
    //  3 - global default, eg. ...Path=...
    // Rules with the most exact matches overrule the more generic once.
    // The match level overrules the specificity.
    //
    // A rule with less wildcards will overrule one with more
    //
    // If a rule ends with an * it is not exact and will be overruled by an exact rule
    //
    // Adding UseRuleSpecificity=n disables this behaviour and reverts to the old classical one
    //

    //
    // set default behaviour
    //

    level = 3; // 3 - global default - lower is better, 3 is max value
    flags = 0;
    wildc = -1; // lower is better
    match_len = 0;
    if (path_code == L'n' && proc->file_block_network_files) {

        //
        // handle network share access preset
        //

        mp_flags = TRUE_PATH_CLOSED_FLAG | COPY_PATH_CLOSED_FLAG;
    }
    else {

        //
        // the sandbox default: read access to all locations unless restricted,
        // and all writes are redirected to the sandbox.  Privacy-style
        // shadow access is expressed per box with WriteFilePath/WriteKeyPath
        // keys (the write lists are matched before the normal list, so a
        // write rule shadows the default).
        //

        mp_flags = TRUE_PATH_READ_FLAG | COPY_PATH_OPEN_FLAG; // normal mode
    }

    //
    // closed path list, in non specific mode has the higher priority
    // these paths are inaccessible for true and copy locations
    //

    bucket = closed_list
        ? Process_PathSetFindBucket(closed_list, seg, seg_len) : NULL;

    if ((closed_list &&
            Pattern_MatchPathListEx(path_lwr, path_len, &closed_list->wild_patterns, &level, &match_len, &flags, &wildc, patsrc))
        || (bucket &&
            Pattern_MatchPathListEx(path_lwr, path_len, &bucket->patterns, &level, &match_len, &flags, &wildc, patsrc))) {
        mp_flags = TRUE_PATH_CLOSED_FLAG | COPY_PATH_CLOSED_FLAG;
        if (!proc->use_rule_specificity) goto finish;
    }

    //
    // write path list, behaved on the driver side like closed path list
    // these paths allow read access to true location and read/write access to copy location
    //

    bucket = write_list
        ? Process_PathSetFindBucket(write_list, seg, seg_len) : NULL;

    if ((write_list &&
            Pattern_MatchPathListEx(path_lwr, path_len, &write_list->wild_patterns, &level, &match_len, &flags, &wildc, patsrc))
        || (bucket &&
            Pattern_MatchPathListEx(path_lwr, path_len, &bucket->patterns, &level, &match_len, &flags, &wildc, patsrc))) {
        mp_flags = TRUE_PATH_CLOSED_FLAG | COPY_PATH_OPEN_FLAG;
        if (!proc->use_rule_specificity) goto finish;
    }

    //
    // read path list behaves in the kernel like the default normal behaviour
    // these paths allow read only access to true path and copy locations
    //

    bucket = read_list
        ? Process_PathSetFindBucket(read_list, seg, seg_len) : NULL;

    if ((read_list &&
            Pattern_MatchPathListEx(path_lwr, path_len, &read_list->wild_patterns, &level, &match_len, &flags, &wildc, patsrc))
        || (bucket &&
            Pattern_MatchPathListEx(path_lwr, path_len, &bucket->patterns, &level, &match_len, &flags, &wildc, patsrc))) {
        mp_flags = TRUE_PATH_READ_FLAG | COPY_PATH_READ_FLAG;
        if (!proc->use_rule_specificity) goto finish;
    }

    //
    // normal path list restores normal behaviour when used in specific mode
    // these paths allow reading the true location and write to the copy location
    //

    bucket = normal_list
        ? Process_PathSetFindBucket(normal_list, seg, seg_len) : NULL;

    if ((normal_list &&
            Pattern_MatchPathListEx(path_lwr, path_len, &normal_list->wild_patterns, &level, &match_len, &flags, &wildc, patsrc))
        || (bucket &&
            Pattern_MatchPathListEx(path_lwr, path_len, &bucket->patterns, &level, &match_len, &flags, &wildc, patsrc))) {
        mp_flags = TRUE_PATH_READ_FLAG | COPY_PATH_OPEN_FLAG;
        // don't goto finish as open can overwrite this
    }

    //
    // open path has lowest priority in non specific mode
    // these paths allow read/write access to the true location
    //

    bucket = open_list
        ? Process_PathSetFindBucket(open_list, seg, seg_len) : NULL;

    if ((open_list &&
            Pattern_MatchPathListEx(path_lwr, path_len, &open_list->wild_patterns, &level, &match_len, &flags, &wildc, patsrc))
        || (bucket &&
            Pattern_MatchPathListEx(path_lwr, path_len, &bucket->patterns, &level, &match_len, &flags, &wildc, patsrc))) {
        mp_flags = TRUE_PATH_OPEN_FLAG;
    }


finish:
    Mem_Free(path_lwr, path_lwr_len);

    return mp_flags;
}
#endif

//---------------------------------------------------------------------------
// Process_GetProcessName
//---------------------------------------------------------------------------


_FX void Process_GetProcessName(
    POOL *pool, ULONG_PTR idProcess,
    void **out_buf, ULONG *out_len, WCHAR **out_ptr)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    CLIENT_ID cid;
    HANDLE handle;
    ULONG len;

    *out_buf = NULL;
    *out_len = 0;
    *out_ptr = NULL;

    if (! idProcess)
        return;

    InitializeObjectAttributes(&objattrs,
        NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    cid.UniqueProcess = (HANDLE)idProcess;
    cid.UniqueThread = 0;

    status = ZwOpenProcess(
        &handle, PROCESS_QUERY_INFORMATION, &objattrs, &cid);

    if (! NT_SUCCESS(status))
        return;

    status = ZwQueryInformationProcess(
                        handle, ProcessImageFileName, NULL, 0, &len);

    if (status == STATUS_INFO_LENGTH_MISMATCH) {

        ULONG uni_len = len + 8 + 8;
        UNICODE_STRING *uni = Mem_Alloc(pool, uni_len);
        if (uni) {

            uni->Buffer = NULL;

            status = ZwQueryInformationProcess(
                        handle, ProcessImageFileName, uni, len + 8, &len);

            if (NT_SUCCESS(status) && uni->Buffer) {

                WCHAR *ptr;
                uni->Buffer[uni->Length / sizeof(WCHAR)] = L'\0';
                if (! uni->Buffer[0]) {
                    uni->Buffer[0] = L'?';
                    uni->Buffer[1] = L'\0';
                }
                ptr = wcsrchr(uni->Buffer, L'\\');
                if (ptr) {
                    ++ptr;
                    if (! *ptr)
                        ptr = uni->Buffer;
                } else
                    ptr = uni->Buffer;
                *out_buf = uni;
                *out_len = uni_len;
                *out_ptr = ptr;

            } else
                Mem_Free(uni, uni_len);
        }
    }

    ZwClose(handle);
}


//---------------------------------------------------------------------------
// Process_CheckProcessName
//---------------------------------------------------------------------------


_FX BOOLEAN Process_CheckProcessName(
    PROCESS *proc, PATH_SET *open_paths, ULONG_PTR idProcess,
    const WCHAR **pSetting)
{
    BOOLEAN result;
    PATTERN *pat;
    PATH_BUCKET *bucket;
    void *nbuf;
    ULONG nlen;
    WCHAR *nptr;

    result = FALSE;

    if (pSetting)
        *pSetting = NULL;

    if (! idProcess)
        return result;

    nbuf = NULL;
    nlen = 0;
    nptr = NULL;

    //
    // Scan settings list for "$:ProcessName"
    // (optimization #1: walk the bucketed set -- wild list first,
    //  then every bucket; the "$:" rules have no path shape)
    //

    pat = List_Head(&open_paths->wild_patterns);
    bucket = List_Head(&open_paths->buckets);

    while (1) {

        if (! pat) {
            if (! bucket)
                break;
            pat = List_Head(&bucket->patterns);
            bucket = List_Next(bucket);
            if (! pat)
                continue;
        }

        const WCHAR *src = Pattern_Source(pat);
        pat = List_Next(pat);
        if (wcslen(src) >= 3 && src[0] == L'$' && src[1] == L':') {

            if (! nptr) {
                Process_GetProcessName(
                    proc->pool, idProcess, &nbuf, &nlen, &nptr);
                if (! nptr)
                    break;
            }
            if (_wcsicmp(nptr, src + 2) == 0 || (src[2] == L'*' && src[3] == L'\0')) { // "$:*" is permitted
                result = TRUE;
                if (pSetting)
                    *pSetting = src;
                break;
            }
        }
    }

    if (nbuf)
        Mem_Free(nbuf, nlen);

    return result;
}


//---------------------------------------------------------------------------
// Process_GetSidStringAndSessionId
//---------------------------------------------------------------------------


_FX NTSTATUS Process_GetSidStringAndSessionId(
    HANDLE ProcessHandle, HANDLE ProcessId,
    UNICODE_STRING *SidString, ULONG *SessionId)
{
    NTSTATUS status;
    PEPROCESS ProcessObject = NULL;
    PACCESS_TOKEN TokenObject;

    if (ProcessHandle == NtCurrentProcess()) {

        ProcessObject = PsGetCurrentProcess();
        ObReferenceObject(ProcessObject);
        status = STATUS_SUCCESS;

    } else if (ProcessHandle) {

        const KPROCESSOR_MODE AccessMode =
            ((ProcessHandle == NtCurrentProcess()) ? KernelMode : UserMode);

        status = ObReferenceObjectByHandle(ProcessHandle, 0, *PsProcessType,
                                           AccessMode, &ProcessObject, NULL);

    } else if (ProcessId) {

        status = PsLookupProcessByProcessId(ProcessId, &ProcessObject);

    } else {

        status = STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(status)) {

        *SessionId = PsGetProcessSessionId(ProcessObject);

        TokenObject = PsReferencePrimaryToken(ProcessObject);
        status = Token_QuerySidString(TokenObject, SidString);
        PsDereferencePrimaryToken(TokenObject);

        ObDereferenceObject(ProcessObject);
    }

    if (! NT_SUCCESS(status)) {

        SidString->Buffer = NULL;
        *SessionId = -1;
    }

    return status;
}


//---------------------------------------------------------------------------
// Process_LogMessage
//---------------------------------------------------------------------------


_FX void Process_LogMessage(PROCESS *proc, ULONG msgid)
{
    BOX *box = proc->box;
    ULONG len = proc->image_name_len + box->name_len + 8 * sizeof(WCHAR);
    WCHAR *text = Mem_Alloc(proc->pool, len);
    RtlStringCbPrintfW(text, len, L"%s [%s]", proc->image_name, box->name);
    if (proc->image_from_box)
        wcscat(text, L" *");
    Log_MsgP1(msgid, text, proc->pid);
    Mem_Free(text, len);
}


//---------------------------------------------------------------------------
// Process_TrackProcessLimit
//---------------------------------------------------------------------------


//_FX void Process_TrackProcessLimit(PROCESS *proc)
//{
//    ULONG v;
//    ULONG ProcessLimit1;
//    ULONG ProcessLimit2;
//
//    //
//    // get the process limits in this sandbox
//    //
//
//    ProcessLimit1 = 100;
//    ProcessLimit2 = 200;
//
//    v = Conf_Get_Number(proc->box->name, L"ProcessLimit1", 0, 0);
//    if (v >= 1 && v <= 999999)
//        ProcessLimit1 = v;
//
//    v = Conf_Get_Number(proc->box->name, L"ProcessLimit2", 0, 0);
//    if (v >= 1 && v <= 999999)
//        ProcessLimit2 = v;
//
//    if (ProcessLimit2 <= ProcessLimit1)
//        ProcessLimit2 = ProcessLimit1 + 1;
//
//    //
//    // count number of processes in this sandbox
//    //
//
//    Process_Enumerate(proc->box->name, FALSE, proc->box->session_id,
//                      NULL, &v);
//
//    if (v > ProcessLimit2) {
//
//        Process_SetTerminated(proc, 4);
//
//    } else if (v > ProcessLimit1) {
//
//        LARGE_INTEGER time;
//
//        time.QuadPart = -SECONDS(10);
//        KeDelayExecutionThread(KernelMode, FALSE, &time);
//    }
//}


//---------------------------------------------------------------------------
// Process_TerminateProcess
//---------------------------------------------------------------------------


_FX BOOLEAN Process_TerminateProcess(PROCESS* proc)
{
    if (Conf_Get_Boolean(NULL, L"TerminateUsingService", 0, TRUE)) {

        if (Process_CancelProcess(proc))
            return TRUE;
        // else fall back to the kernel method
    }

    return Process_ScheduleKill(proc, 0);
}


//---------------------------------------------------------------------------
// Process_CancelProcess
//---------------------------------------------------------------------------


_FX BOOLEAN Process_CancelProcess(PROCESS *proc)
{
    SVC_PROCESS_MSG msg;

    ULONG len = wcslen(proc->image_name);
    const ULONG max_len = sizeof(msg.process_name) / sizeof(WCHAR) - 1;
    if (len > max_len)
        len = max_len;
    wmemcpy(msg.process_name, proc->image_name, len);
    msg.process_name[len] = L'\0';

    msg.process_id = (ULONG)(ULONG_PTR)proc->pid;
    msg.session_id = proc->box->session_id;
    msg.create_time = proc->create_time;
    msg.is_wow64 = FALSE;
    msg.add_to_job = FALSE;
    msg.reason = proc->reason;

    return Api_SendServiceMessage(SVC_CANCEL_PROCESS, sizeof(msg), &msg);
}


//---------------------------------------------------------------------------
// Process_IsSbieImage
//---------------------------------------------------------------------------


_FX VOID Process_IsSbieImage(const WCHAR* image_path, BOOLEAN *image_sbie, BOOLEAN *is_start_exe)
{
    if(image_sbie) *image_sbie = FALSE;
    if(is_start_exe) *is_start_exe = FALSE;

    WCHAR *image_name = wcsrchr(image_path, L'\\');
    if (image_name) {

        ULONG len = (ULONG)(image_name - image_path);
        if ((len == Driver_HomePathNt_Len) &&
                (wcsncmp(image_path, Driver_HomePathNt, len) == 0)) {

            if(image_sbie) *image_sbie = TRUE;

            if (_wcsicmp(image_name + 1, START_EXE) == 0) {

                if(is_start_exe) *is_start_exe = TRUE;
            }
        }
    }
}


//---------------------------------------------------------------------------
// Process_IsPcaJob
//---------------------------------------------------------------------------


_FX BOOLEAN Process_IsInPcaJob(HANDLE ProcessId)
{
    PEPROCESS ProcessObject;
    ULONG_PTR JobObject;
    OBJECT_NAME_INFORMATION *Name;
    ULONG NameLength;
    NTSTATUS status;
    BOOLEAN IsInPcaJob = FALSE;

    status = PsLookupProcessByProcessId(ProcessId, &ProcessObject);
    if (NT_SUCCESS(status)) {

        JobObject = PsGetProcessJob(ProcessObject);
        if (JobObject) {

            status = Obj_GetName(Driver_Pool, (void *)JobObject,
                                 &Name, &NameLength);
            if (NT_SUCCESS(status) && (Name != &Obj_Unnamed)) {

                if (Name->Name.Length == 60 * sizeof(WCHAR)
                        && 0 == _wcsnicmp(Name->Name.Buffer,
                                    L"\\BaseNamedObjects\\PCA_", 22)) {

                    IsInPcaJob = TRUE;
                }

                Mem_Free(Name, NameLength);

            } else if (NT_SUCCESS(status) && (Name == &Obj_Unnamed) &&
                            Driver_OsVersion >= DRIVER_WINDOWS_8) {
                //
                // on Windows 8 the PCA job is unnamed
                //

                IsInPcaJob = TRUE;
            }
        }

        ObDereferenceObject(ProcessObject);
    }

    return IsInPcaJob;
}


//---------------------------------------------------------------------------
// Process_IsInAppPkg
//---------------------------------------------------------------------------

#define TOKEN_SECURITY_ATTRIBUTE_TYPE_INVALID 0x00
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_INT64 0x01
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_UINT64 0x02
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING 0x03 // Case insensitive attribute value string by default. Unless the flag TOKEN_SECURITY_ATTRIBUTE_VALUE_CASE_SENSITIVE is set.
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN 0x04 // Fully-qualified binary name.
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_SID 0x05
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_BOOLEAN 0x06
#define TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING 0x10

_FX BOOLEAN Process_IsInAppPkg(HANDLE ProcessId)
{
    NTSTATUS status;
    HANDLE processHandle = NULL;
    HANDLE tokenHandle = NULL;
    OBJECT_ATTRIBUTES objAttr;
    CLIENT_ID clientId;
    PTOKEN_SECURITY_ATTRIBUTES_INFORMATION info = NULL;
    ULONG returnLength = 0;
    BOOLEAN IsInAppPkg = FALSE;

    // Initialize structures
    InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    clientId.UniqueProcess = ProcessId;
    clientId.UniqueThread = NULL;
    status = ZwOpenProcess(&processHandle, PROCESS_QUERY_INFORMATION, &objAttr, &clientId);
    if (!NT_SUCCESS(status))
        goto Cleanup;

    status = ZwOpenProcessTokenEx(processHandle, TOKEN_QUERY, OBJ_KERNEL_HANDLE, &tokenHandle);
    if (!NT_SUCCESS(status))
        goto Cleanup;

    status = ZwQueryInformationToken(tokenHandle, TokenSecurityAttributes, NULL, 0, &returnLength);
    if (status != STATUS_BUFFER_TOO_SMALL && returnLength == 0)
        goto Cleanup;

    info = (PTOKEN_SECURITY_ATTRIBUTES_INFORMATION)ExAllocatePoolWithTag(PagedPool, returnLength, tzuk);
    if (!info)
        goto Cleanup;

    status = ZwQueryInformationToken( tokenHandle, TokenSecurityAttributes, info, returnLength, &returnLength);

    if (!NT_SUCCESS(status))
    {
        goto Cleanup;
    }

    // Find WIN://SYSAPPID attribute
    for (ULONG i = 0; i < info->AttributeCount; i++)
    {
        PTOKEN_SECURITY_ATTRIBUTE_V1 attr = &info->Attribute.pAttributeV1[i];

        if (attr->ValueType == TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING)
        {
            UNICODE_STRING attrName;
            RtlInitUnicodeString(&attrName, L"WIN://SYSAPPID");

            UNICODE_STRING currentName;
            RtlInitUnicodeString(&currentName, attr->Name.Buffer);

            if (RtlEqualUnicodeString(&attrName, &currentName, TRUE) &&
                attr->ValueCount > 0)
            {
                IsInAppPkg = TRUE;
				DbgPrint("SBIE: Process is in App Package: %wZ\n", attr->Values.pString);
                break;
            }
        }
    }

Cleanup:
    if (info)
        ExFreePoolWithTag(info, tzuk);

    if (tokenHandle)
        ZwClose(tokenHandle);

    if (processHandle)
        ZwClose(processHandle);

    return IsInAppPkg;
}


//---------------------------------------------------------------------------
// Process_ScheduleKillProc
//---------------------------------------------------------------------------

extern BOOLEAN Driver_FullUnload;

_FX VOID Process_ScheduleKillProc(IN PVOID StartContext)
{
    PVOID* params = (PVOID*)StartContext;
    HANDLE process_id = (HANDLE)(params[0]);
    LONG delay_ms = (LONG)(params[1]);
    Mem_Free(params, sizeof(PVOID)*2);

    NTSTATUS status;
    HANDLE handle = NULL;
    PEPROCESS ProcessObject;

    __try {
    retry:
        if (Driver_FullUnload)
            __leave;
        status = PsLookupProcessByProcessId(process_id, &ProcessObject);
        if (NT_SUCCESS(status)) {

            status = ObOpenObjectByPointer(ProcessObject, OBJ_KERNEL_HANDLE, NULL, PROCESS_ALL_ACCESS, NULL, KernelMode, &handle);
            ObDereferenceObject(ProcessObject);

            if (NT_SUCCESS(status)) {

                if (delay_ms > 0) {
                    ZwClose(handle);

                    LARGE_INTEGER time;
                    time.QuadPart = -(SECONDS(1) / 20); // wait half a second = 50ms
                    KeDelayExecutionThread(KernelMode, FALSE, &time);

                    delay_ms -= 50;
                    goto retry;
                }
                else {

                    ZwTerminateProcess(handle, STATUS_PROCESS_IS_TERMINATING);
                    ZwClose(handle);
                }
            }
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    PsTerminateSystemThread(status);
}


//---------------------------------------------------------------------------
// Process_ScheduleKill
//---------------------------------------------------------------------------


_FX BOOLEAN Process_ScheduleKill(PROCESS *proc, LONG delay_ms)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    HANDLE handle;

    PVOID *params = Mem_Alloc(Driver_Pool, sizeof(PVOID)*2);
    params[0] = proc->pid;
    params[1] = (PVOID)delay_ms;

    InitializeObjectAttributes(&objattrs, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &objattrs, NULL, NULL, Process_ScheduleKillProc, params);
    if (NT_SUCCESS(status)) {

        ZwClose(handle);

        if (delay_ms != 0)
            return TRUE;

        ULONG len = proc->image_name_len + 32 * sizeof(WCHAR);
        WCHAR *text = Mem_Alloc(Driver_Pool, len);
        if (text) {

            if (proc->reason == 0)
                RtlStringCbPrintfW(text, len, L"%s", proc->image_name);
            else if (proc->reason != -1) // in this case we have SBIE1308 and don't want any other messages
                RtlStringCbPrintfW(text, len, L"%s [%d]", proc->image_name, proc->reason);
            else
                *text = 0;
            proc->reason = -1; // avoid repeated messages if this gets re triggered

            if (*text)
                Log_MsgP1(MSG_2314, text, proc->pid);
            Mem_Free(text, len);
        }

        return TRUE;
    }
    return FALSE;
}
