/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
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
// GUI Services
//---------------------------------------------------------------------------

#include "dll.h"

#include "gui_p.h"
#include "common/my_version.h"


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


static int Gui_GetWindowTextW(
    HWND hWnd, WCHAR *lpWindowTitle, int nMaxCount);

static int Gui_GetWindowTextA(
    HWND hWnd, UCHAR *lpWindowTitle, int nMaxCount);

static const WCHAR *Gui_GetBoxDisplayName(void);


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


BOOLEAN Gui_DisableTitle = FALSE;

static BOOLEAN Gui_DisableCustomTitleOpt = FALSE;

const WCHAR *Gui_TitleSuffixW = TITLE_SUFFIX_W;
static ULONG Gui_TitleSuffixW_len = 0;

const UCHAR *Gui_TitleSuffixA = TITLE_SUFFIX_A;
static ULONG Gui_TitleSuffixA_len = 0;

ULONG Gui_BoxNameTitleLen = 0;
WCHAR *Gui_BoxNameTitleW = NULL;
static ANSI_STRING Gui_BoxNameTitleA;
static ULONG Gui_BoxNameTitleALen = 0;

//---------------------------------------------------------------------------
// BoxTitleFormat (custom title template)
//
// [BoxConfig] BoxTitleFormat=<fmt>  overrides the built-in
// "[#] [Box] <title> [#]" decoration with a per-box template.
//
//   %s   box display name (honours BoxAlias / BoxAliasDisplayMode,
//        same as BoxNameTitle=y)
//   %t   the original window title
//   %%   a literal percent sign
//
// The template is split at the first %t into prefix/suffix halves;
// a template without %t decorates as a pure prefix.  BoxNameTitle=-
// (disable) still takes precedence over any format.  With no
// BoxTitleFormat set, the legacy BoxNameTitle behaviour is kept
// byte-for-byte.
//---------------------------------------------------------------------------


static BOOLEAN Gui_TitleCustom = FALSE;
static WCHAR *Gui_TitleCustomPrefixW = NULL;
static ULONG  Gui_TitleCustomPrefixWLen = 0;
static WCHAR *Gui_TitleCustomSuffixW = NULL;
static ULONG  Gui_TitleCustomSuffixWLen = 0;
static UCHAR *Gui_TitleCustomPrefixA = NULL;
static ULONG  Gui_TitleCustomPrefixALen = 0;
static UCHAR *Gui_TitleCustomSuffixA = NULL;
static ULONG  Gui_TitleCustomSuffixALen = 0;


//---------------------------------------------------------------------------
// Gui_GetBoxDisplayName
//---------------------------------------------------------------------------


_FX const WCHAR *Gui_GetBoxDisplayName(void)
{
    const WCHAR* BoxName = Dll_BoxName;
    WCHAR BoxDisplayName[MAX_PATH + BOXNAME_COUNT + 4];
    ULONG AliasDisplayMode = SbieApi_QueryConfNumber(
        L"GlobalSettings", L"BoxAliasDisplayMode", 0);
    if (AliasDisplayMode > 2)
        AliasDisplayMode = 0;

    NTSTATUS status;
    WCHAR BoxAlias[MAX_PATH];
    status = SbieApi_QueryConfAsIs(NULL, L"BoxAlias", 0, BoxAlias, sizeof(BoxAlias));
    if (AliasDisplayMode != 1 && NT_SUCCESS(status) && *BoxAlias) {
        if (AliasDisplayMode == 2 && _wcsicmp(BoxAlias, Dll_BoxName) != 0) {
            Sbie_snwprintf(BoxDisplayName, ARRAYSIZE(BoxDisplayName),
                L"%s (%s)", BoxAlias, Dll_BoxName);
            BoxName = BoxDisplayName;
        }
        else
            BoxName = BoxAlias;
    }

    //
    // the alias path may point at a stack buffer; copy into a
    // Dll_Alloc'd block so the pointer stays valid after return
    //

    {
        ULONG len = wcslen(BoxName);
        WCHAR *dup = Dll_Alloc((len + 1) * sizeof(WCHAR));
        wmemcpy(dup, BoxName, len + 1);
        return dup;
    }
}


//---------------------------------------------------------------------------
// Gui_InitTitleCustom
//---------------------------------------------------------------------------


_FX void Gui_InitTitleCustom(void)
{
    WCHAR fmt[192];
    ANSI_STRING ansi;
    UNICODE_STRING uni;
    const WCHAR *boxName;
    const WCHAR *s;
    WCHAR *d;
    WCHAR *halves[2];
    ULONG halfLen[2];
    ULONG i;

    SbieDll_GetSettingsForName(
        NULL, Dll_ImageName, L"BoxTitleFormat", fmt, sizeof(fmt), NULL);
    if (! (*fmt))
        return;                       // not configured: legacy behaviour

    //
    // split the template at the first %t: half 0 decorates before the
    // original title, half 1 after it.  No %t in the template -> the
    // whole template acts as the prefix.
    //

    halves[0] = fmt;
    halves[1] = NULL;
    for (s = fmt; *s; ++s) {
        if (s[0] == L'%' && (s[1] == L't' || s[1] == L'T')) {
            halves[1] = (WCHAR *)s + 2;
            break;
        }
    }
    halfLen[0] = halves[1]
        ? (ULONG)(halves[1] - fmt - 2) : (ULONG)wcslen(fmt);
    if (halves[1])
        halfLen[1] = (ULONG)wcslen(halves[1]);
    else
        halfLen[1] = 0;

    boxName = Gui_GetBoxDisplayName();

    for (i = 0; i < 2; ++i) {

        //
        // expand %s (box display name) and %% (literal percent) in the
        // half; a single % before anything else is kept verbatim.
        // alloc bound: every template char may expand to a full box
        // name (halfLen * (boxName + 1)) -- generous and safe.
        //

        WCHAR *exp = Dll_Alloc(
            ((halfLen[i] + 1) * (wcslen(boxName) + 1) + 1) * sizeof(WCHAR));
        d = exp;
        if (halves[i]) {
            for (s = halves[i]; s < halves[i] + halfLen[i]; ++s) {
                if (s[0] == L'%') {
                    if (s + 1 < halves[i] + halfLen[i] &&
                            (s[1] == L's' || s[1] == L'S')) {
                        ULONG bl = wcslen(boxName);
                        wmemcpy(d, boxName, bl);
                        d += bl;
                        ++s;
                        continue;
                    }
                    if (s + 1 < halves[i] + halfLen[i] && s[1] == L'%') {
                        *d = L'%';
                        ++d;
                        ++s;
                        continue;
                    }
                }
                *d = *s;
                ++d;
            }
        }
        *d = L'\0';

        RtlInitUnicodeString(&uni, exp);
        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&ansi, &uni, TRUE))) {
            if (i == 0) {
                Gui_TitleCustomPrefixW = exp;
                Gui_TitleCustomPrefixWLen = (ULONG)wcslen(exp);
                Gui_TitleCustomPrefixA = (UCHAR *)ansi.Buffer;
                Gui_TitleCustomPrefixALen = ansi.Length;
            } else {
                Gui_TitleCustomSuffixW = exp;
                Gui_TitleCustomSuffixWLen = (ULONG)wcslen(exp);
                Gui_TitleCustomSuffixA = (UCHAR *)ansi.Buffer;
                Gui_TitleCustomSuffixALen = ansi.Length;
            }
        }
    }

    Gui_TitleCustom = TRUE;
}


//---------------------------------------------------------------------------
// Gui_InitTitle
//---------------------------------------------------------------------------


_FX BOOLEAN Gui_InitTitle(HMODULE module)
{
    WCHAR buf[10];

    //
    // initialize title variables
    //

    SbieDll_GetSettingsForName(NULL, Dll_ImageName, L"BoxNameTitle", buf, sizeof(buf), NULL);
    if (*buf == L'-') // don't alter boxed window titles at all
        Gui_DisableTitle = TRUE;
    else {
        Gui_InitTitleCustom();
        if (! Gui_TitleCustom && (*buf == L'y' || *buf == L'Y')) {
            // indicator + box name (legacy decoration)

            const WCHAR* BoxName = Gui_GetBoxDisplayName();

            UNICODE_STRING uni;

            Gui_BoxNameTitleLen = wcslen(BoxName) + 3;
            Gui_BoxNameTitleW =
                Dll_Alloc((Gui_BoxNameTitleLen + 3) * sizeof(WCHAR));
            Gui_BoxNameTitleW[0] = Gui_TitleSuffixW[1];         // L'['
            wcscpy(&Gui_BoxNameTitleW[1], BoxName);
            wcscat(Gui_BoxNameTitleW, &Gui_TitleSuffixW[3]);    // L"]"
            wcscat(Gui_BoxNameTitleW, L" ");

            RtlInitUnicodeString(&uni, Gui_BoxNameTitleW);
            RtlUnicodeStringToAnsiString(&Gui_BoxNameTitleA, &uni, TRUE);
            Gui_BoxNameTitleALen = Gui_BoxNameTitleA.Length;
        }
        //  else if(*buf == L'n' || *buf == L'N') means show indicator but not box name
    }

    Gui_TitleSuffixW_len = wcslen(Gui_TitleSuffixW);
    Gui_TitleSuffixA_len = strlen(Gui_TitleSuffixA);

    //
    // check if user wants to disable the custom titlebar optimization
    // (i.e. allow [#] markers on VCL/Qt/Electron custom titlebar windows)
    //

    SbieDll_GetSettingsForName(NULL, Dll_ImageName, L"DisableCustomTitleOpt", buf, sizeof(buf), NULL);
    if (*buf == L'y' || *buf == L'Y')
        Gui_DisableCustomTitleOpt = TRUE;

    //
    // hook functions
    //

    if (! Gui_DisableTitle) {

        SBIEDLL_HOOK_GUI(GetWindowTextW);
        SBIEDLL_HOOK_GUI(GetWindowTextA);
    }

    return TRUE;
}


//---------------------------------------------------------------------------
// Gui_ShouldCreateTitle
//---------------------------------------------------------------------------


_FX BOOLEAN Gui_ShouldCreateTitle(HWND hWnd)
{
    if (Gui_DisableTitle)
        return FALSE;

    if (hWnd == (HWND)(ULONG_PTR)tzuk)      // console window
        return TRUE;

    if (__sys_GetParent(hWnd)) {

        //
        // don't do title for child windows (i.e. windows that
        // have a parent), except if this is a popup dialog window
        //

        ULONG_PTR dlgproc;
        if (__sys_IsWindowUnicode(hWnd))
            dlgproc = __sys_GetWindowLongPtrW(hWnd, DWLP_DLGPROC);
        else
            dlgproc = __sys_GetWindowLongPtrA(hWnd, DWLP_DLGPROC);
        if (dlgproc) {

            ULONG style = (ULONG)__sys_GetWindowLongW(hWnd, GWL_STYLE);
            if (style & WS_POPUP)
                return TRUE;
        }

    } else {

        //
        // do title if the window has a caption, unless it is a
        // window of type Edit control
        //

        ULONG style = (ULONG)__sys_GetWindowLongW(hWnd, GWL_STYLE);
        if ((style & WS_CAPTION) == WS_CAPTION) {

            //
            // Check for custom titlebar - if the client area extends close to
            // the top of the window, the application is rendering its own
            // titlebar (e.g., VCL TCustomTitleBarPanel, Qt, Electron, etc.)
            // and modifying the title causes excessive repainting.
            // Set DisableCustomTitleOpt=y to allow [#] markers on
            // custom titlebar windows.
            //

            if (! Gui_DisableCustomTitleOpt) {

                RECT windowRect;
                POINT clientOrigin = {0, 0};
                if (__sys_GetWindowRect(hWnd, &windowRect) &&
                    __sys_ClientToScreen(hWnd, &clientOrigin)) {

                    int titleBarHeight = clientOrigin.y - windowRect.top;

                    // Standard titlebar is typically 20-40 pixels. If the gap
                    // is less than 10 pixels, it's a custom titlebar - skip
                    // modification.
                    if (titleBarHeight < 10)
                        return FALSE;
                }
            }

            // $Workaround$ - 3rd party fix
            WCHAR clsnm[256];
            UINT nChars = __sys_RealGetWindowClassW(hWnd, clsnm, sizeof(clsnm) - 1);

            // MS stupidly added a WS_CAPTION attribute to some hidden window that comes up with the Office splash screens -- but they don't actually
            // have any captions. When we replace the caption, Office doesn't like it and goes into an infinite loop calling SetWindowPos.
            // The only way I can see to detect these splash screens reliably is to check for each individual dialog class name.
            if (wcsstr(clsnm, L":XLMAIN"))          // Excel
                return FALSE;
            if (wcsstr(clsnm, L":OpusApp"))         // Word
                return FALSE;
            if (wcsstr(clsnm, L":PPTFrameClass"))   // PowerPoint
                return FALSE;
            if (wcsstr(clsnm, L":MSWinPub"))        // Publisher
                return FALSE;
            if (wcsstr(clsnm, L":rctrl_renwnd32"))  // Outlook
                return FALSE;
            if (wcsstr(clsnm, L":Framework::CFrame"))   // Onenote
                return FALSE;

            if (_wcsicmp(clsnm, L"Edit") != 0)
                return TRUE;
            
        }
    }

    return FALSE;
}


//---------------------------------------------------------------------------
// Gui_CreateTitleW
//---------------------------------------------------------------------------


_FX WCHAR *Gui_CreateTitleW(const WCHAR *oldTitle)
{
    WCHAR *newTitle, *ptr;
    ULONG len_new, len_old;

    if ((! oldTitle) || Gui_DisableTitle)
        return (WCHAR *)oldTitle;

    __try {

        len_old = wcslen(oldTitle);

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return (WCHAR *)oldTitle;
    }

    //
    // custom BoxTitleFormat template: title = prefix + old + suffix
    //

    if (Gui_TitleCustom) {

        if (Gui_TitleCustomPrefixWLen && len_old >= Gui_TitleCustomPrefixWLen
                && wmemcmp(oldTitle, Gui_TitleCustomPrefixW,
                           Gui_TitleCustomPrefixWLen) == 0
            && Gui_TitleCustomSuffixWLen
                && len_old >= Gui_TitleCustomSuffixWLen
                && wmemcmp(oldTitle + len_old - Gui_TitleCustomSuffixWLen,
                           Gui_TitleCustomSuffixW,
                           Gui_TitleCustomSuffixWLen) == 0)
            return (WCHAR *)oldTitle;          // already decorated

        len_new = (Gui_TitleCustomPrefixWLen + len_old
                   + Gui_TitleCustomSuffixWLen + 1) * sizeof(WCHAR);
        newTitle = Dll_Alloc(len_new);

        ptr = newTitle;
        if (Gui_TitleCustomPrefixWLen) {
            wmemcpy(ptr, Gui_TitleCustomPrefixW, Gui_TitleCustomPrefixWLen);
            ptr += Gui_TitleCustomPrefixWLen;
        }
        wmemcpy(ptr, oldTitle, len_old);
        ptr += len_old;
        if (Gui_TitleCustomSuffixWLen) {
            wmemcpy(ptr, Gui_TitleCustomSuffixW, Gui_TitleCustomSuffixWLen);
            ptr += Gui_TitleCustomSuffixWLen;
        }
        *ptr = L'\0';

        return newTitle;
    }

    if (len_old > Gui_TitleSuffixW_len) {
        ptr = (WCHAR *)oldTitle + len_old - Gui_TitleSuffixW_len;
        if (wmemcmp(ptr, Gui_TitleSuffixW, Gui_TitleSuffixW_len) == 0)
            return (WCHAR *)oldTitle;
    }

    len_new = (len_old + Gui_TitleSuffixW_len * 2 + 1) * sizeof(WCHAR);
    if (Gui_BoxNameTitleLen)
        len_new += Gui_BoxNameTitleLen * sizeof(WCHAR);
    newTitle = Dll_Alloc(len_new);

    wmemcpy(newTitle, Gui_TitleSuffixW + 1, Gui_TitleSuffixW_len - 1);
    ptr = newTitle + Gui_TitleSuffixW_len - 1;
    *ptr = L' ';
    ++ptr;

    if (Gui_BoxNameTitleLen) {
        wmemcpy(ptr, Gui_BoxNameTitleW, Gui_BoxNameTitleLen);
        ptr += Gui_BoxNameTitleLen;
    }

    wmemcpy(ptr, oldTitle, len_old);
    ptr += len_old;
    wmemcpy(ptr, Gui_TitleSuffixW, Gui_TitleSuffixW_len);
    ptr += Gui_TitleSuffixW_len;
    *ptr = L'\0';

    return newTitle;
}


//---------------------------------------------------------------------------
// Gui_CreateTitleA
//---------------------------------------------------------------------------


_FX UCHAR *Gui_CreateTitleA(const UCHAR *oldTitle)
{
    UCHAR *newTitle, *ptr;
    ULONG len_new, len_old;

    if ((! oldTitle) || Gui_DisableTitle)
        return (char *)oldTitle;

    __try {

        len_old = strlen(oldTitle);

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        return (char *)oldTitle;
    }

    //
    // custom BoxTitleFormat template: title = prefix + old + suffix
    //

    if (Gui_TitleCustom) {

        if (Gui_TitleCustomPrefixALen && len_old >= Gui_TitleCustomPrefixALen
                && memcmp(oldTitle, Gui_TitleCustomPrefixA,
                          Gui_TitleCustomPrefixALen) == 0
            && Gui_TitleCustomSuffixALen
                && len_old >= Gui_TitleCustomSuffixALen
                && memcmp(oldTitle + len_old - Gui_TitleCustomSuffixALen,
                          Gui_TitleCustomSuffixA,
                          Gui_TitleCustomSuffixALen) == 0)
            return (UCHAR *)oldTitle;          // already decorated

        len_new = Gui_TitleCustomPrefixALen + len_old
                  + Gui_TitleCustomSuffixALen + 1;
        newTitle = Dll_Alloc(len_new);

        ptr = newTitle;
        if (Gui_TitleCustomPrefixALen) {
            memcpy(ptr, Gui_TitleCustomPrefixA, Gui_TitleCustomPrefixALen);
            ptr += Gui_TitleCustomPrefixALen;
        }
        memcpy(ptr, oldTitle, len_old);
        ptr += len_old;
        if (Gui_TitleCustomSuffixALen) {
            memcpy(ptr, Gui_TitleCustomSuffixA, Gui_TitleCustomSuffixALen);
            ptr += Gui_TitleCustomSuffixALen;
        }
        *ptr = '\0';

        return newTitle;
    }

    if (len_old > Gui_TitleSuffixA_len) {
        ptr = (UCHAR *)oldTitle + len_old - Gui_TitleSuffixA_len;
        if (memcmp(ptr, Gui_TitleSuffixA, Gui_TitleSuffixA_len) == 0)
            return (UCHAR *)oldTitle;
    }

    len_new = (len_old + Gui_TitleSuffixA_len * 2 + 1) * sizeof(UCHAR);
    if (Gui_BoxNameTitleALen)
        len_new += Gui_BoxNameTitleALen * sizeof(UCHAR);
    newTitle = Dll_Alloc(len_new);

    memcpy(newTitle, Gui_TitleSuffixA + 1, Gui_TitleSuffixA_len - 1);
    ptr = newTitle + Gui_TitleSuffixA_len - 1;
    *ptr = ' ';
    ++ptr;

    if (Gui_BoxNameTitleALen) {
        memcpy(ptr, Gui_BoxNameTitleA.Buffer, Gui_BoxNameTitleALen);
        ptr += Gui_BoxNameTitleALen;
    }

    memcpy(ptr, oldTitle, len_old);
    ptr += len_old;
    memcpy(ptr, Gui_TitleSuffixA, Gui_TitleSuffixA_len);
    ptr += Gui_TitleSuffixA_len;
    *ptr = '\0';

    return newTitle;
}


//---------------------------------------------------------------------------
// Gui_FixTitleW
//---------------------------------------------------------------------------


_FX int Gui_FixTitleW(HWND hWnd, WCHAR *lpWindowTitle, int len)
{
    if (Gui_TitleCustom && Gui_ShouldCreateTitle(hWnd)) {

        if (Gui_TitleCustomPrefixWLen && len >= (int)Gui_TitleCustomPrefixWLen
                && wmemcmp(lpWindowTitle, Gui_TitleCustomPrefixW,
                           Gui_TitleCustomPrefixWLen) == 0) {
            len -= Gui_TitleCustomPrefixWLen;
            wmemmove(lpWindowTitle,
                     lpWindowTitle + Gui_TitleCustomPrefixWLen, len);
            lpWindowTitle[len] = L'\0';
        }
        if (Gui_TitleCustomSuffixWLen && len >= (int)Gui_TitleCustomSuffixWLen
                && wmemcmp(lpWindowTitle + len - Gui_TitleCustomSuffixWLen,
                           Gui_TitleCustomSuffixW,
                           Gui_TitleCustomSuffixWLen) == 0) {
            len -= Gui_TitleCustomSuffixWLen;
            lpWindowTitle[len] = L'\0';
        }
        return len;
    }

    if (len >= (int)Gui_TitleSuffixW_len * 2 &&
                                            Gui_ShouldCreateTitle(hWnd)) {

        if (wmemcmp(lpWindowTitle, &Gui_TitleSuffixW[1], 3) == 0) {
            len -= 4;
            wmemmove(lpWindowTitle, lpWindowTitle + 4, len);
            lpWindowTitle[len] = L'\0';
        }
        if (wmemcmp(lpWindowTitle + len - 4, Gui_TitleSuffixW, 4) == 0) {
            len -= 4;
            lpWindowTitle[len] = L'\0';
        }
        if (Gui_BoxNameTitleLen) {
            const int lenTitle    = Gui_BoxNameTitleLen;
            const WCHAR *ptrTitle = Gui_BoxNameTitleW;
            if (len >= lenTitle
                    && wmemcmp(lpWindowTitle, ptrTitle, lenTitle) == 0) {
                len -= lenTitle;
                wmemmove(lpWindowTitle, lpWindowTitle + lenTitle, len);
                lpWindowTitle[len] = L'\0';
            }
        }
    }

    return len;
}


//---------------------------------------------------------------------------
// Gui_FixTitleA
//---------------------------------------------------------------------------


_FX int Gui_FixTitleA(HWND hWnd, UCHAR *lpWindowTitle, int len)
{
    if (Gui_TitleCustom && Gui_ShouldCreateTitle(hWnd)) {

        if (Gui_TitleCustomPrefixALen && len >= (int)Gui_TitleCustomPrefixALen
                && memcmp(lpWindowTitle, Gui_TitleCustomPrefixA,
                          Gui_TitleCustomPrefixALen) == 0) {
            len -= Gui_TitleCustomPrefixALen;
            memmove(lpWindowTitle,
                    lpWindowTitle + Gui_TitleCustomPrefixALen, len);
            lpWindowTitle[len] = '\0';
        }
        if (Gui_TitleCustomSuffixALen && len >= (int)Gui_TitleCustomSuffixALen
                && memcmp(lpWindowTitle + len - Gui_TitleCustomSuffixALen,
                          Gui_TitleCustomSuffixA,
                          Gui_TitleCustomSuffixALen) == 0) {
            len -= Gui_TitleCustomSuffixALen;
            lpWindowTitle[len] = '\0';
        }
        return len;
    }

    if (len >= (int)Gui_TitleSuffixA_len * 2 &&
                                            Gui_ShouldCreateTitle(hWnd)) {

        if (memcmp(lpWindowTitle, &Gui_TitleSuffixA[1], 3) == 0) {
            len -= 4;
            memmove(lpWindowTitle, lpWindowTitle + 4, len);
            lpWindowTitle[len] = '\0';
        }
        if (memcmp(lpWindowTitle + len - 4, Gui_TitleSuffixA, 4) == 0) {
            len -= 4;
            lpWindowTitle[len] = '\0';
        }
        if (Gui_BoxNameTitleALen) {
            const int lenTitle    = Gui_BoxNameTitleALen;
            const UCHAR *ptrTitle = Gui_BoxNameTitleA.Buffer;
            if (len >= lenTitle
                    && memcmp(lpWindowTitle, ptrTitle, lenTitle) == 0) {
                len -= lenTitle;
                memmove(lpWindowTitle, lpWindowTitle + lenTitle, len);
                lpWindowTitle[len] = '\0';
            }
        }
    }

    return len;
}


//---------------------------------------------------------------------------
// Gui_GetWindowTextW
//---------------------------------------------------------------------------


_FX int Gui_GetWindowTextW(
    HWND hWnd, WCHAR *lpWindowTitle, int nMaxCount)
{
    int rc = __sys_GetWindowTextW(hWnd, lpWindowTitle, nMaxCount);
    return Gui_FixTitleW(hWnd, lpWindowTitle, rc);
}


//---------------------------------------------------------------------------
// Gui_GetWindowTextA
//---------------------------------------------------------------------------


_FX int Gui_GetWindowTextA(
    HWND hWnd, UCHAR *lpWindowTitle, int nMaxCount)
{
    int rc = __sys_GetWindowTextA(hWnd, lpWindowTitle, nMaxCount);
    return Gui_FixTitleA(hWnd, lpWindowTitle, rc);
}
