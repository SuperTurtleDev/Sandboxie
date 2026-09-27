@echo off
setlocal EnableExtensions
REM ============================================================
REM  Sandboxie-OSS dist packaging - x64
REM
REM  Prereq: build_all.bat produced the unified layout
REM             ..\Installer\SbieOSS_x64\
REM          (core runtime + sbie-cli.exe + sbie-gui\)
REM  Output: dist\Sandboxie-OSS-x64\        staged tree
REM          dist\Sandboxie-OSS-x64.zip     the archive
REM  Usage:  make_dist.bat [--verify]
REM          --verify  also extract the zip to dist\_verify and
REM                    run the extracted sbie-cli.exe standalone;
REM                    driver cat/signature, if present, are
REM                    REPORTED but never gate the build (signing
REM                    is decoupled - docs/05-build.md section 5).
REM
REM  Core runtime selection criteria (verified 2026-09-27,
REM  dumpbin /dependents + source audit, details in
REM  docs/05-build.md section 8):
REM    - sbie-cli.exe imports ONLY system DLLs + VC runtime
REM      (MSVCP140/VCRUNTIME140/VCRUNTIME140_1) - no static dep
REM      on any Sandboxie binary; SbieDll.dll is LoadLibrary-ed
REM      at runtime, same directory is search candidate #2.
REM    - SbieSvc/KmdUtil/Start/SbieIni/SandboxieRpcSs/
REM      SandboxieDcomLaunch/SboxHostDll import SbieDll.dll
REM      statically - shipping SbieDll.dll alongside covers all.
REM    - KmdUtil.exe is invoked by "sbie-cli maint" for driver
REM      start/stop; ImBox.exe is spawned by SbieSvc MountManager
REM      for disk-image/ram boxes; SandboxieRpcSs.exe +
REM      SandboxieDcomLaunch.exe are auto-started sandboxed COM
REM      stubs (core\dll\ipc_start.c) unless NoSandboxieRpcSs=y.
REM      SandboxieBITS/WUAU/Crypto.exe are HARDCODED SCM redirects
REM      (core\dll\scm.c Scm_IsBoxedService + scm_create.c
REM      Scm_StartBoxedService2): a boxed StartService of
REM      bits/wuauserv/cryptsvc launches the matching stub; NOT
REM      template-gated, and missing stubs mean the in-box feature
REM      silently fails. ~0.4MB combined - all five ship.
REM    - Templates.ini: the ORIGINAL full file ships (byte-identical from the
REM      layout). A minimal OSS stub was prototyped and REJECTED on evidence:
REM      with a stubbed home Templates.ini, sandboxed process startup breaks
REM      deterministically on the frozen 5.73.5 runtime (A/B/A verified twice:
REM      full=ok, stub=child exit 127 + SYSTEM-context log flood; consumer
REM      lives in frozen core\dll/core\svc code). V2 never consumes this file
REM      for its own boxes (user-space expansion, zero Template= residue) -
REM      it ships purely to keep the frozen runtime healthy. Revisit a slim
REM      file post-freeze. docs/05-build.md section 8.2 records the evidence.
REM    - templates\ = the V2 template tree (400 files, migrated from
REM      install\Templates.ini by "sbie-cli --migrate-templates").
REM      It is the initial SBIE_TEMPLATE_DIR content; sbie-cli also
REM      falls back to <exe dir>\templates automatically, so the
REM      dist is zero-config self-contained.
REM    - 32\SbieDll.dll + 32\SbieSvc.exe = WOW64 pair for
REM      sandboxing 32-bit apps.
REM    - VC runtime trio included so the dist runs on machines
REM      without the VC++ 2015-2022 redist installed.
REM    - driver\ is copied AS-IS from the unified layout: the
REM      WDK build output (sys + inf; cat only if the build
REM      emitted one - a stock WDK build does not). No signing
REM      assertion; kernel-signing pairs sys+cat from the
REM      KernelSigner manual archive as a separate process.
REM    - EXCLUDED (Plus-specific or not needed by sbie-cli):
REM      SandMan.exe, Qt6*.dll, QSbieAPI.dll, MiscHelpers.dll,
REM      UGlobalHotkey.dll, qtsingleapp.dll, platforms\, styles\,
REM      tls\, 7z.dll, UpdUtil.exe, SbieShellExt.dll, MiniDump,
REM      SbieCtrl.exe, SboxHostDll.dll, SbieIni.exe (sbie-cli
REM      has its own cfg group), Start.exe (exec launches through
REM      SbieSvc RunSandboxed, never Start.exe), pdb files.
REM      sbie-gui\ (the OSS GUI ships from the layout, not the
REM      minimal dist - see docs/09-gui.md).
REM    - No "msgs" text directory exists in the layout: message
REM      text ships compiled inside SbieMsg.dll (resource-only
REM      DLL built from Sandboxie\msgs\msgs.mc).
REM  Spec: docs/05-build.md section 8 (ASCII only: cmd.exe
REM  parses batch files in the OEM code page)
REM ============================================================
cd /d "%~dp0"
set "OSSDIR=%CD%"
set "REPO=%CD%\.."
set "LAYOUT=%REPO%\Installer\SbieOSS_x64"
REM Dist output root. Override SBIEOSS_DISTROOT to emit elsewhere (CI,
REM or when the default dist\ is transiently locked by an external
REM process - e.g. a shell sitting inside it).
if defined SBIEOSS_DISTROOT (
    set "DISTROOT=%SBIEOSS_DISTROOT%"
) else (
    set "DISTROOT=%OSSDIR%\dist"
)
set "DIST_NAME=Sandboxie-OSS-x64"
set "STAGE=%DISTROOT%\%DIST_NAME%"
set "ZIP=%DISTROOT%\%DIST_NAME%.zip"
set "VERIFY="
if /i "%~1"=="--verify" set "VERIFY=1"

REM --- 0. prerequisites: layout + every input file verified ---
if not exist "%LAYOUT%\" (
    echo [ERROR] %LAYOUT% missing - run build_all.bat first.
    goto :fail
)
if not exist "%LAYOUT%\sbie-cli.exe" (
    echo [ERROR] %LAYOUT%\sbie-cli.exe missing - run build_all.bat first.
    goto :fail
)

REM root files copied flat into the dist root.
REM Templates.ini = the ORIGINAL full file (see header note: the minimal stub
REM was rejected on evidence - it breaks spawns on the frozen runtime).
set "ROOT_FILES=sbie-cli.exe SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe ImBox.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe SandboxieBITS.exe SandboxieWUAU.exe SandboxieCrypto.exe Templates.ini msvcp140.dll vcruntime140.dll vcruntime140_1.dll"
REM V2 template tree (initial SBIE_TEMPLATE_DIR content + exe-dir fallback)
set "TPL_SRC=%OSSDIR%\templates"
REM driver: sys+inf required, copied as-is from layout\driver\
REM (any further files in layout\driver\ - e.g. a build-emitted
REM cat - are copied along as-is; none are hard-required)
set "DRV_FILES=SbieDrv.sys SbieDrv.inf"
REM WOW64 pair from layout\32\
set "WOW_FILES=SbieDll.dll SbieSvc.exe"

set "MISSING="
for %%f in (%ROOT_FILES%) do if not exist "%LAYOUT%\%%f" call :add_missing "%%f"
for %%f in (%DRV_FILES%)  do if not exist "%LAYOUT%\driver\%%f" call :add_missing "driver\%%f"
for %%f in (%WOW_FILES%)  do if not exist "%LAYOUT%\32\%%f" call :add_missing "32\%%f"
if not exist "%TPL_SRC%\BoxTypes\Standard.ini" call :add_missing "templates\BoxTypes\Standard.ini"
if not exist "%REPO%\LICENSE.Classic"        call :add_missing "..\LICENSE.Classic"
if not exist "%OSSDIR%\thirdparty\README.md" call :add_missing "thirdparty\README.md"
if not exist "%OSSDIR%\docs\README.md"       call :add_missing "docs\README.md"
if defined MISSING (
    echo [ERROR] missing prerequisites:%MISSING%
    goto :fail
)

REM --- 1. stage the dist tree ---
echo [1/4] Staging %STAGE%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%STAGE%" ( echo [ERROR] cannot clean stale %STAGE% & goto :fail )
mkdir "%STAGE%\driver" "%STAGE%\32" "%STAGE%\docs" || goto :fail

for %%f in (%ROOT_FILES%) do copy /y "%LAYOUT%\%%f"        "%STAGE%\%%f"        >nul || goto :fail
REM driver\ directory copied AS-IS (sys+inf required, extra files along)
for /f "delims=" %%f in ('dir /b "%LAYOUT%\driver"') do copy /y "%LAYOUT%\driver\%%f" "%STAGE%\driver\%%f" >nul || goto :fail
for %%f in (%WOW_FILES%) do copy /y "%LAYOUT%\32\%%f"      "%STAGE%\32\%%f"     >nul || goto :fail
copy /y "%REPO%\LICENSE.Classic"        "%STAGE%\LICENSE-OSS"     >nul || goto :fail
copy /y "%OSSDIR%\thirdparty\README.md" "%STAGE%\THIRD-PARTY.md"  >nul || goto :fail
copy /y "%OSSDIR%\docs\README.md"       "%STAGE%\docs\README.md"  >nul || goto :fail

REM V2 template tree -> dist\templates\ (robocopy /E: recursive incl. empty;
REM exit codes 0-7 are success for robocopy)
robocopy "%TPL_SRC%" "%STAGE%\templates" /E /NJH /NJS /NDL /NFL >nul
if errorlevel 8 ( echo [ERROR] robocopy templates failed & goto :fail )

REM --- 2. VERSION.txt: sbie-cli version + driver version + date ---
echo [2/4] Generating VERSION.txt
REM Running the staged copy doubles as a smoke test: it resolves
REM SbieDll.dll from its own directory (candidate #2 of the
REM drv::LoadSbieDll search chain after --sbie-dll-path).
set "CLI_VER=unknown"
"%STAGE%\sbie-cli.exe" --version > "%DISTROOT%\version.tmp" 2>nul
if errorlevel 1 (
    echo [WARN] staged sbie-cli.exe version check failed - CLI_VER stays unknown
) else (
    for /f "tokens=2" %%v in ('findstr /b /c:"sbie-cli " "%DISTROOT%\version.tmp"') do set "CLI_VER=%%v"
)
del /f /q "%DISTROOT%\version.tmp" >nul 2>&1
set "DRV_VER=unknown"
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Item -LiteralPath '%LAYOUT%\driver\SbieDrv.sys').VersionInfo.FileVersion"`) do set "DRV_VER=%%v"
set "BUILD_DATE=unknown"
for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"`) do set "BUILD_DATE=%%d"

>  "%STAGE%\VERSION.txt" echo Sandboxie-OSS dist %DIST_NAME%
>> "%STAGE%\VERSION.txt" echo sbie-cli version : %CLI_VER%
>> "%STAGE%\VERSION.txt" echo SbieDrv version  : %DRV_VER%  - WDK build output, as-is (signing decoupled)
>> "%STAGE%\VERSION.txt" echo build date       : %BUILD_DATE%
>> "%STAGE%\VERSION.txt" echo layout source    : Installer\SbieOSS_x64 after build_all.bat

REM --- 3. zip + manifest ---
echo [3/4] Creating %ZIP%
if exist "%ZIP%" del /f /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -LiteralPath '%STAGE%' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 goto :fail

echo.
echo ===== dist manifest: %DIST_NAME% =====
powershell -NoProfile -Command "$root='%STAGE%'; $f=Get-ChildItem -LiteralPath $root -Recurse -File; $f | ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($root.Length+1) }; ''; 'TOTAL: {0} files, {1:N0} bytes' -f $f.Count, ($f | Measure-Object Length -Sum).Sum; 'ZIP  : {0} bytes' -f (Get-Item -LiteralPath '%ZIP%').Length"
echo =========================================

REM --- 4. optional verification: extract zip, run extracted exe ---
if not defined VERIFY (
    echo.
    echo DIST SUCCEEDED: %ZIP%
    echo hint: run "make_dist.bat --verify" to extract-check the zip and
    echo        smoke-test the extracted sbie-cli.exe standalone.
    exit /b 0
)

echo.
echo [4/4] Verifying zip integrity + standalone run
set "VDIR=%DISTROOT%\_verify"
if exist "%VDIR%" rmdir /s /q "%VDIR%"
mkdir "%VDIR%" || goto :fail
powershell -NoProfile -Command "Expand-Archive -LiteralPath '%ZIP%' -DestinationPath '%VDIR%' -Force"
if errorlevel 1 goto :fail
if not exist "%VDIR%\%DIST_NAME%\sbie-cli.exe" (
    echo [ERROR] zip layout wrong: sbie-cli.exe not at %DIST_NAME%\ root after extract
    goto :fail
)
"%VDIR%\%DIST_NAME%\sbie-cli.exe" --version
if errorlevel 1 (
    echo [ERROR] extracted sbie-cli.exe failed to run
    goto :fail
)

REM V2 dist invariants: full Templates.ini present (UTF-16 file - findstr
REM cannot match it; verify by size: full = ~152KB, any stub would be <2KB)
REM + template tree + five service stubs
set "VD=%VDIR%\%DIST_NAME%"
set "TPLSZ=0"
for %%A in ("%VD%\Templates.ini") do set "TPLSZ=%%~zA"
if %TPLSZ% LSS 100000 (
    echo [ERROR] Templates.ini too small - %TPLSZ% bytes, expected the full 152KB original
    goto :fail
)
if not exist "%VD%\templates\BoxTypes\Standard.ini" (
    echo [ERROR] V2 template tree missing: templates\BoxTypes\Standard.ini not found
    goto :fail
)
for %%f in (SandboxieRpcSs.exe SandboxieDcomLaunch.exe SandboxieBITS.exe SandboxieWUAU.exe SandboxieCrypto.exe) do (
    if not exist "%VD%\%%f" (
        echo [ERROR] sandboxed service stub missing: %%f
        goto :fail
    )
)
echo V2 invariants OK: full Templates.ini + templates tree + 5 service stubs present.

REM driver cat / signature status: REPORT ONLY, never a gate.
REM A stock WDK build emits no cat and a test-signed sys - that is
REM expected here. Kernel-signing (KernelSigner manual archive,
REM sys+cat pairing) is a separate follow-up process.
powershell -NoProfile -Command "$d='%VDIR%\%DIST_NAME%\driver'; if (Test-Path (Join-Path $d 'SbieDrv.cat')) { Get-ChildItem -LiteralPath $d -File | ForEach-Object { $s = Get-AuthenticodeSignature -LiteralPath $_.FullName; 'driver report : {0} -> {1} (informational, not a gate)' -f $_.Name, $s.Status } } else { 'driver report : no cat present - WDK build output as-is, signing decoupled' }"

echo VERIFY OK: zip extracts; extracted sbie-cli.exe runs and resolves
echo               SbieDll.dll from its own directory.
echo DIST SUCCEEDED: %ZIP%
exit /b 0

:add_missing
set "MISSING=%MISSING% %~1"
exit /b 0

:fail
echo DIST FAILED
exit /b 1
