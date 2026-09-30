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
// forced processes); counts the process for auto-teardown.

void BoxDynamic_OnProcessCreate(const WCHAR *box_name);


// hook from Process_Delete:  called for a sandboxed process that is about
// to be freed; decrements the live-process count of its dynamic box and
// tears the box down when the count reaches zero.

void BoxDynamic_OnProcessDelete(PROCESS *proc);


// hook from Process_NotifyProcess_Delete:  called for EVERY process that
// exits; destroys dynamic boxes whose creator process has died.

void BoxDynamic_OnAnyProcessExit(HANDLE ProcessId);


// API handlers (registered by BoxDynamic_Init)

NTSTATUS BoxDynamic_Api_Create(PROCESS *proc, ULONG64 *parms);

NTSTATUS BoxDynamic_Api_Exec(PROCESS *proc, ULONG64 *parms);

NTSTATUS BoxDynamic_Api_Destroy(PROCESS *proc, ULONG64 *parms);


//---------------------------------------------------------------------------

#endif // _MY_BOX_DYNAMIC_H
