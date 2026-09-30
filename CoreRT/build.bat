@echo off
REM NOTE: keep delayed expansion OFF - the repository build scripts contain
REM literal '!' characters (7-Zip -i!wildcard switches) that delayed
REM expansion would strip.
setlocal EnableExtensions
REM ============================================================
REM  CoreRT build - x64 (kernel runtime + service + DLL + msgs
REM  + service stubs + KmdUtil/ImBox tools)
REM  Output: ..\Installer\SbieOSS_x64\   (core runtime face)
REM
REM  Builds:
REM    SandboxDll.sln Win32  -> 32\ WOW64 pair
REM    Sandbox.sln x64       -> SbieSvc/SbieDll/SbieMsg/com stubs/KmdUtil
REM    SandboxDrv.sln x64    -> driver\SbieDrv.sys (WDK)
REM    tools\SandboxieTools.sln /t:ImBox -> ImBox.exe
REM  then assembles the core runtime face into the shared layout.
REM  Follow-ups (NOT run here): Cli\build.bat (sbie-rt.exe),
REM  GUI\build_gui.bat (sbie-gui\), root make_dist.bat (zip).
REM
REM  Signing is DECOUPLED from the build: the WDK driver build output is
REM  placed as-is into driver\ (sys + inf; cat only if the build emits
REM  one, which a stock WDK build does not). No signtool/inf2cat call,
REM  no signature gate. Kernel-signing is a separate follow-up process
REM  that pairs sys+cat from the KernelSigner manual archive - see
REM  docs/05-build.md section 5.
REM
REM  Usage: CoreRT\build.bat [--no-pause]
REM  (ASCII only: cmd.exe parses batch files in the OEM code page)
REM ============================================================

cd /d "%~dp0"
set "CORE=%CD%"
set "REPO=%CD%\.."
set "T_START=%TIME%"

if /i "%~1"=="--no-pause" (set "NO_PAUSE=1") else (set "NO_PAUSE=0")

echo ============================================================
echo   CoreRT build (x64)
echo   Repo : %REPO%
echo   Start: %DATE% %TIME%
echo ============================================================
echo.

REM When launched from Git Bash / MSYS shells, GNU find.exe and
REM link.exe shadow the Windows and MSVC ones and break the
REM repository scripts. Put System32 first.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

REM ------------------------------------------------------------
REM [0] Build environment
REM ------------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found - Visual Studio Installer missing.
    goto :fail
)
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo [ERROR] No VS 2022 instance with the C++ toolset found.
    goto :fail
)
echo [0/5] Visual Studio: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where msbuild >nul 2>&1 || ( echo [ERROR] msbuild not on PATH after vcvars64 & goto :fail )
where cl >nul 2>&1 || ( echo [ERROR] cl.exe not on PATH after vcvars64 & goto :fail )

REM WDK: SandboxDrv.sln needs the kernel-mode toolset (any installed
REM 10 SDK km tree; the driver project pins LatestTargetPlatformVersion).
set "WDK_KM="
for /d %%d in ("%ProgramFiles(x86)%\Windows Kits\10\Include\*") do if exist "%%d\km" set "WDK_KM=%%d"
if not defined WDK_KM (
    echo [ERROR] No WDK kernel-mode include under "%ProgramFiles(x86)%\Windows Kits\10\Include" - required by SbieDrv.sys.
    goto :fail
)
echo [0/5] WDK kernel-mode include: %WDK_KM%

REM VC runtime redist source for the layout trio (set by vcvars64).
if not exist "%VCToolsRedistDir%x64\Microsoft.VC143.CRT" (
    echo [ERROR] VC runtime redist not found: "%VCToolsRedistDir%"
    goto :fail
)

REM ------------------------------------------------------------
REM [1] Core - Win32 (SbieDll/SbieSvc for 32-bit apps: the 32\ pair)
REM ------------------------------------------------------------
cd /d "%CORE%"
echo.
echo [1/5] Building CoreRT Win32 (SbieRelease, WOW64 pair)
msbuild /t:build SandboxDll.sln /p:Configuration="SbieRelease" /p:Platform=Win32 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [2] Core - x64 (SbieSvc, SbieDll, SbieMsg, com stubs, KmdUtil)
REM ------------------------------------------------------------
cd /d "%CORE%"
echo.
echo [2/5] Building CoreRT x64 (SbieRelease)
msbuild /t:build Sandbox.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [3] Kernel driver (WDK)
REM ------------------------------------------------------------
cd /d "%CORE%"
echo.
echo [3/5] Building CoreRT driver x64 (SbieDrv.sys, WDK)
msbuild /t:build SandboxDrv.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [4] Tools - ImBox target only (needed at runtime by SbieSvc
REM     MountManager; UpdUtil/MiniDump were Plus-side, deleted)
REM ------------------------------------------------------------
cd /d "%CORE%"
echo.
echo [4/5] Building ImBox x64 (tools, target selection)
msbuild /t:ImBox tools\SandboxieTools.sln /p:Configuration="Release" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [5] Assemble the core runtime face into ..\Installer\SbieOSS_x64
REM     (file list = make_dist.bat manifest face: docs/05-build.md
REM     section 6)
REM ------------------------------------------------------------
cd /d "%CORE%"
echo.
echo [5/5] Assembling Installer\SbieOSS_x64
set "OUT=%REPO%\Installer\SbieOSS_x64"
set "BIN64=%CORE%\Bin\x64\SbieRelease"
set "BIN32=%CORE%\Bin\Win32\SbieRelease"
set "TOOLS=%CORE%\tools\x64\Release"

REM Empty the layout IN PLACE (children first): the root itself may be
REM pinned as an external process's CWD (documented hazard class - see
REM make_dist.bat SBIEOSS_DISTROOT note about dist\ locked by a shell
REM sitting inside it). Deleting all children achieves the identical
REM fresh state without requiring the root handle to be free.  Empty
REM directories that only stay behind because a shell sits inside them
REM are tolerated: the robocopy /MIR below re-syncs their content.
if exist "%OUT%" (
    for /d %%d in ("%OUT%\*") do rmdir /s /q "%%d" 2>nul
    del /f /q "%OUT%\*" >nul 2>&1
)
REM fail loudly only if locked FILES survived the cleanup
dir /b /a-d "%OUT%" 2>nul | findstr . >nul && ( echo [ERROR] cannot clean stale "%OUT%" & goto :fail )
if not exist "%OUT%\32"     mkdir "%OUT%\32"     || goto :fail
if not exist "%OUT%\driver" mkdir "%OUT%\driver" || goto :fail

REM x64 core runtime
REM All five in-sandbox service stubs ship: RpcSs + DcomLaunch are started
REM on demand via the epmapper/actkernel ports (dll ipc_start.c) and
REM are required by default boxes; BITS + WUAU + Crypto are HARDCODED SCM
REM redirects (dll scm.c Scm_IsBoxedService + scm_create.c StartBoxedService2:
REM a boxed StartService of bits/wuauserv/cryptsvc runs the matching stub
REM exe; NOT template-gated - if absent the in-box feature silently fails).
REM Combined ~0.4MB, ship all (docs/05 section 8.2 revision).
for %%f in (SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe SandboxieBITS.exe SandboxieWUAU.exe SandboxieCrypto.exe) do copy /y "%BIN64%\%%f" "%OUT%\%%f" >nul || goto :fail
copy /y "%TOOLS%\ImBox.exe" "%OUT%\ImBox.exe" >nul || goto :fail
REM Trimmed OSS Templates.ini from the source tree (deployment
REM principle: templates\ = V2 tree, Templates.ini = the unmigrated
REM residue + frozen-runtime startup skeleton). The kernel reads
REM (Home)\Templates.ini at every config reload (drv\conf.c
REM Conf_Read); the skeleton sections are load-bearing -
REM [TemplateDefaultPaths] carries OpenIpcPath=\KnownDlls\* without
REM which boxed processes die (SBIE2112, child exit 127). Full
REM evidence + A/B matrix in docs/05-build.md section 8.2.
copy /y "%REPO%\Templates.ini" "%OUT%\Templates.ini" >nul || goto :fail

REM V2 template tree mirrored into the layout from the source tree
REM (robocopy /MIR = exact sync incl. stale-file purge, so the
REM layout can never drift from the source; exit 0-7 = success).
REM This is the SBIE_TEMPLATE_DIR initial content; sbie-rt also
REM falls back to <exe dir>\templates automatically.
robocopy "%REPO%\templates" "%OUT%\templates" /MIR /NJH /NJS /NDL /NFL >nul
if errorlevel 8 ( echo [ERROR] robocopy templates into layout failed & goto :fail )

REM VC runtime trio - layout runs on machines without the VC++ redist
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\%%f" "%OUT%\%%f" >nul || goto :fail

REM WOW64 pair
for %%f in (SbieDll.dll SbieSvc.exe) do copy /y "%BIN32%\%%f" "%OUT%\32\%%f" >nul || goto :fail

REM Driver - WDK build output as-is, signing decoupled:
REM   sys from the build output; inf from the driver project source
REM   (byte-identical to what the Plus layout shipped); cat only if
REM   the build produced one (stock WDK build does not).
copy /y "%BIN64%\SbieDrv.sys" "%OUT%\driver\SbieDrv.sys" >nul || goto :fail
set "INF_SRC=%CORE%\drv\SbieDrv.inf"
if exist "%BIN64%\SbieDrv.inf" set "INF_SRC=%BIN64%\SbieDrv.inf"
copy /y "%INF_SRC%" "%OUT%\driver\SbieDrv.inf" >nul || goto :fail
if exist "%BIN64%\SbieDrv.cat" copy /y "%BIN64%\SbieDrv.cat" "%OUT%\driver\SbieDrv.cat" >nul

REM Verify the core face (sbie-rt.exe / sbie-gui\ are added by the
REM Cli / GUI module builds that follow).
echo.
echo ===== Installer\SbieOSS_x64 core face =====
set "FACE=SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe ImBox.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe SandboxieBITS.exe SandboxieWUAU.exe SandboxieCrypto.exe Templates.ini msvcp140.dll vcruntime140.dll vcruntime140_1.dll"
set "MISSING="
for %%f in (%FACE%) do if not exist "%OUT%\%%f" call :add_missing "%%f"
for %%f in (SbieDll.dll SbieSvc.exe) do if not exist "%OUT%\32\%%f" call :add_missing "32\%%f"
for %%f in (SbieDrv.sys SbieDrv.inf) do if not exist "%OUT%\driver\%%f" call :add_missing "driver\%%f"
if not exist "%OUT%\templates\BoxTypes\Standard.ini" call :add_missing "templates\BoxTypes\Standard.ini"
if defined MISSING (
    echo [ERROR] incomplete layout:%MISSING%
    goto :fail
)

echo.
echo ============================================================
echo   CoreRT BUILD SUCCEEDED (core face assembled)
echo   Output : %OUT%\
echo   Next   : Cli\build.bat  -^> sbie-rt.exe into the layout
echo            GUI\build_gui.bat -^> sbie-gui\ into the layout
echo            make_dist.bat --verify -^> dist zip
echo   Started: %T_START%   Finished: %TIME%
echo   Signing decoupled: driver\ holds the WDK build output as-is.
echo ============================================================
if "%NO_PAUSE%"=="0" pause
exit /b 0

:add_missing
set "MISSING=%MISSING% %~1"
exit /b 0

:fail
echo.
echo ============================================================
echo   CoreRT BUILD FAILED
echo ============================================================
if "%NO_PAUSE%"=="0" pause
exit /b 1
