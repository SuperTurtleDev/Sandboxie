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
//---------------------------------------------------------------------------

#ifndef _MY_BOX_DYNAMIC_H
#define _MY_BOX_DYNAMIC_H

#include "driver.h"
#include "process.h"

//---------------------------------------------------------------------------
// Defines
//---------------------------------------------------------------------------

// dynamic box sections are named  BoxConfig_<id>  where <id> is a decimal
// 64-bit monotonic counter.  The name obeys Box_IsValidName ([0-9A-Za-z_],
// max 38 chars) so the rest of the driver -- which treats a section name
// as a box name -- works on it unchanged.

#define BOX_DYN_PREFIX              L"BoxConfig_"

// handle returned by API_BOX_CREATE is the raw 64-bit id

#define BOX_DYN_MAX_BOXES           64      // concurrent dynamic boxes
#define BOX_DYN_MAX_TEXT_LEN        (64 * 1024)  // config blob, in bytes
#define BOX_DYN_MAX_PIDS_PER_SWEEP  64      // box kill sweep batch

//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------

BOOLEAN BoxDynamic_Init(void);

void BoxDynamic_Unload(void);

// hook from Process_Create:  called for every process that enters a box
// (API_BOX_EXEC roots, children started from inside a dynamic box, and
// forced processes); counts the process for auto-teardown and returns
// the dynamic-box entry when the box is dynamic (NULL otherwise).  The
// returned pointer is stored in proc->box_dyn and stays alive for the
// lifetime of the PROCESS (auto teardown only fires when no boxed
// process remains).

struct _BOX_DYN_ENTRY;

struct _BOX_DYN_ENTRY *BoxDynamic_OnProcessCreate(const WCHAR *box_name);

void BoxDynamic_OnProcessDelete(PROCESS *proc);

void BoxDynamic_OnAnyProcessExit(HANDLE ProcessId);

// name-based handle-direct read (the Conf_GetEx branch and the
// skeleton consumers):  box_name identifies the dynamic box; with a
// setting returns its index-th value (ini list semantics), without a
// setting returns the index-th key name.  NULL when unknown/dead.

const WCHAR *BoxDyn_Lookup(
    const WCHAR *box_name, const WCHAR *setting, ULONG index);

// lock-free fast paths for a process that holds a live entry
// (the entry is immutable after publish; teardown only happens when
// no boxed process remains):

struct _BOX_DYN_ENTRY *BoxDyn_GetEntry(const WCHAR *box_name);

const WCHAR *BoxDyn_GetValue(
    struct _BOX_DYN_ENTRY *entry, const WCHAR *setting, ULONG index);

BOOLEAN BoxDyn_GetBit(struct _BOX_DYN_ENTRY *entry, UINT64 bit);

const WCHAR *BoxDyn_BoolName(UINT64 bit);

// API handlers (registered by BoxDynamic_Init)

NTSTATUS BoxDynamic_Api_Create(PROCESS *proc, ULONG64 *parms);

NTSTATUS BoxDynamic_Api_Exec(PROCESS *proc, ULONG64 *parms);

NTSTATUS BoxDynamic_Api_Destroy(PROCESS *proc, ULONG64 *parms);

//---------------------------------------------------------------------------

#endif // _MY_BOX_DYNAMIC_H
