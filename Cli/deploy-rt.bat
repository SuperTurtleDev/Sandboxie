@echo off
setlocal EnableExtensions
REM ============================================================
REM  Sandboxie-OSS sbie-rt deployment staging - x64
REM  (dynamic-box-arch runtime, docs/12-dynamic-arch.md)
REM
REM  Stages the complete ValidationOS / bare-host runtime needed by
REM  the sbie-rt dynamic-box lifecycle into ONE flat directory:
REM
REM     sbie-rt create examples\standard.kv   -> BoxConfig_<id>
REM     sbie-rt exec <handle> <exe>           -> SbieSvc injects
REM     SbieLow -> SbieDll -> boxed process;  sbie-rt destroy
REM
REM  Prereq: CoreRT\build.bat (runtime layout) + Cli\build.bat
REM          (sbie-rt.exe).  Output: ..\dist\sbie-rt\  (override
REM          with SBIEOSS_RTROOT).
REM
REM  Component notes (verified against source, see make_dist.bat
REM  header for the audit trail):
REM    - SbieDrv.sys goes to the deploy ROOT next to SbieDll.dll:
REM      the driver home path (Api_GetHomePath -> SbieDll location)
REM      is the SbieDrv.sys ImagePath DIRECTORY, so a driver\
REM      subdirectory would break SbieDll resolution.
REM    - SbieLow.dll does NOT ship as a file: SbieLow 32+64 are
REM      embedded in SbieSvc.exe as RT_RCDATA resources
REM      LOWLEVEL64/LOWLEVEL32 (CoreRT\dll\lowlevel_inject.c).
REM    - SbieSvc/KmdUtil/service stubs import SbieDll.dll
REM      statically; SandboxieRpcSs/DcomLaunch/BITS/WUAU/Crypto are
REM      spawned from this directory by SbieDll Ipc_StartServer /
REM      hardcoded SCM redirects.
REM    - 32\SbieDll.dll + 32\SbieSvc.exe: WOW64 pair (<home>\32\ is
REM      where SbieLow looks for 32-bit SbieDll / ComProxy slave).
REM    - VC runtime trio: sbie-rt.exe imports
REM      MSVCP140/VCRUNTIME140/140_1.
REM    - NO Templates.ini: the driver embeds no defaults and reads
REM      no ini on the runtime path; every box carries its own
REM      [TemplateDefaultPaths]/[TemplateNetworkPaths] skeleton in
REM      the API_BOX_CREATE blob (CoreRT\sdk\examples\*.kv).
REM    - SandboxieUsers local group: the future non-admin grant for
REM      the dynamic-box API (driver currently gates on elevated
REM      admin, UAC-aware).  Created opportunistically below.
REM
REM  Post-staging service registration (SCM hosts only - NOT run by
REM  this script; SbieSvc itself NtLoadDriver's SbieDrv on demand):
REM    KmdUtil install SbieDrv "<DIR>\SbieDrv.sys" type=kernel start=demand altitude=86900 "msgfile=<DIR>\SbieMsg.dll"
REM    KmdUtil install SbieSvc "<DIR>\SbieSvc.exe" type=own start=auto "display=Sandboxie-OSS Service" group=UIGroup "msgfile=<DIR>\SbieMsg.dll"
REM    KmdUtil start SbieSvc
REM
REM  ASCII only: cmd.exe parses batch files in the OEM code page.
REM ============================================================
cd /d "%~dp0"
set "CLIDIR=%CD%"
set "REPO=%CD%\.."
set "LAYOUT=%REPO%\Installer\SbieOSS_x64"
if defined SBIEOSS_RTROOT (
    set "STAGE=%SBIEOSS_RTROOT%"
) else (
    set "STAGE=%REPO%\dist\sbie-rt"
)

REM --- 0. prerequisites ---
if not exist "%LAYOUT%\SbieSvc.exe" (
    echo [ERROR] %LAYOUT%\SbieSvc.exe missing - run CoreRT\build.bat first.
    goto :fail
)
if not exist "%LAYOUT%\driver\SbieDrv.sys" (
    echo [ERROR] %LAYOUT%\driver\SbieDrv.sys missing - run CoreRT\build.bat first.
    goto :fail
)
if not exist "%CLIDIR%\x64\Release\sbie-rt.exe" (
    echo [ERROR] %CLIDIR%\x64\Release\sbie-rt.exe missing - run Cli\build.bat first.
    goto :fail
)

REM root components copied flat (SbieDrv.sys from layout\driver\ ON
REM PURPOSE - see header note on the driver home path).
set "ROOT_FILES=SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe ImBox.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe SandboxieBITS.exe SandboxieWUAU.exe SandboxieCrypto.exe msvcp140.dll vcruntime140.dll vcruntime140_1.dll"
set "WOW_FILES=SbieDll.dll SbieSvc.exe"
set "KV_FILES=standard.kv hardened.kv privacy.kv appc.kv dynamic-box.kv"

set "MISSING="
for %%f in (%ROOT_FILES%) do if not exist "%LAYOUT%\%%f" call :add_missing "%%f"
for %%f in (%WOW_FILES%)  do if not exist "%LAYOUT%\32\%%f" call :add_missing "32\%%f"
for %%f in (%KV_FILES%)   do if not exist "%REPO%\CoreRT\sdk\examples\%%f" call :add_missing "examples\%%f"
if defined MISSING (
    echo [ERROR] missing prerequisites:%MISSING%
    goto :fail
)

REM --- 1. stage the tree ---
echo [1/3] Staging %STAGE%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%STAGE%" ( echo [ERROR] cannot clean stale %STAGE% & goto :fail )
mkdir "%STAGE%\32" "%STAGE%\examples" || goto :fail

copy /y "%LAYOUT%\driver\SbieDrv.sys" "%STAGE%\SbieDrv.sys" >nul || goto :fail
copy /y "%CLIDIR%\x64\Release\sbie-rt.exe" "%STAGE%\sbie-rt.exe" >nul || goto :fail
for %%f in (%ROOT_FILES%) do copy /y "%LAYOUT%\%%f" "%STAGE%\%%f" >nul || goto :fail
for %%f in (%WOW_FILES%)  do copy /y "%LAYOUT%\32\%%f" "%STAGE%\32\%%f" >nul || goto :fail
for %%f in (%KV_FILES%)   do copy /y "%REPO%\CoreRT\sdk\examples\%%f" "%STAGE%\examples\%%f" >nul || goto :fail

REM --- 2. SandboxieUsers local group (future non-admin grant) ---
REM Informational only: the driver currently requires an elevated
REM admin for CREATE/EXEC/DESTROY.  Needs an elevated shell.
net session >nul 2>&1
if not errorlevel 1 (
    net localgroup SandboxieUsers /add >nul 2>&1
    if not errorlevel 1 (
        echo [2/3] local group SandboxieUsers created
        echo        grant a user:  net localgroup SandboxieUsers USERNAME /add
    ) else (
        echo [2/3] local group SandboxieUsers already exists or was refused
    )
) else (
    echo [2/3] not elevated - skipped SandboxieUsers group creation
)

REM --- 3. manifest ---
echo [3/3] Writing MANIFEST.txt
powershell -NoProfile -Command "$root='%STAGE%'; $f=Get-ChildItem -LiteralPath $root -Recurse -File; $lines=@(); $lines+='sbie-rt deployment layout (dynamic-box-arch)'; $lines+='stage root: '+$root; $lines+=''; $f | ForEach-Object { $lines+=('{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($root.Length+1)) }; $lines+=''; $lines+=('TOTAL: {0} files, {1:N0} bytes' -f $f.Count, ($f | Measure-Object Length -Sum).Sum); $lines | Set-Content -LiteralPath (Join-Path $root 'MANIFEST.txt') -Encoding Ascii; $lines | ForEach-Object { $_ }"

echo.
echo ===== deploy staged: %STAGE% =====
echo Driver home = stage root: SbieDrv.sys sits next to SbieDll.dll.
echo SandboxieLow: embedded in SbieSvc.exe (LOWLEVEL64/32 resources).
echo No Templates.ini needed: boxes carry their own skeleton (examples\*.kv).
echo.
echo SCM-host registration (run elevated from the stage root, once):
echo   KmdUtil install SbieDrv "%STAGE%\SbieDrv.sys" type=kernel start=demand altitude=86900 "msgfile=%STAGE%\SbieMsg.dll"
echo   KmdUtil install SbieSvc "%STAGE%\SbieSvc.exe" type=own start=auto "display=Sandboxie-OSS Service" group=UIGroup "msgfile=%STAGE%\SbieMsg.dll"
echo   KmdUtil start SbieSvc
echo.
echo Then:  sbie-rt drv  /  sbie-rt create examples\standard.kv
echo DEPLOY SUCCEEDED
exit /b 0

:add_missing
set "MISSING=%MISSING% %~1"
exit /b 0

:fail
echo DEPLOY FAILED
exit /b 1
