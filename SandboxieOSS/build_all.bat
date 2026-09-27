@echo off
REM NOTE: keep delayed expansion OFF - the repository build scripts contain
REM literal '!' characters (7-Zip -i!wildcard switches) that delayed
REM expansion would strip.
setlocal EnableExtensions
REM ============================================================
REM  Sandboxie-OSS unified build - x64 (core + sbie-cli + sbie-gui)
REM  Output: ..\Installer\SbieOSS_x64\   (self-contained OSS layout)
REM
REM  Step classification vs the Sandboxie-Plus root build.bat
REM  (full table in docs/05-build.md section 4):
REM    CORE (built here) : SandboxDll.sln Win32  -> 32\ WOW64 pair
REM                        Sandbox.sln x64       -> SbieSvc/SbieDll/
REM                                                 SbieMsg/com stubs/
REM                                                 KmdUtil
REM                        SandboxDrv.sln x64    -> driver\SbieDrv.sys (WDK)
REM                        SandboxieTools /t:ImBox -> ImBox.exe
REM    OSS  (built here) : build_oss.bat (sbie-cli), build_gui.bat (sbie-gui)
REM    PLUS (NOT built)  : Qt download, qmake_plus, SandMan, SbieShell,
REM                        OpenSSL/7z runtime fetch, copy_build merge,
REM                        get_assets - no Plus step runs in this chain.
REM
REM  Signing is DECOUPLED from the build: the WDK driver build output is
REM  placed as-is into driver\ (sys + inf; cat only if the build emits
REM  one, which a stock WDK build does not). No signtool/inf2cat call,
REM  no signature gate. Kernel-signing is a separate follow-up process
REM  that pairs sys+cat from the KernelSigner manual archive - see
REM  docs/05-build.md section 5.
REM
REM  Usage: build_all.bat [--no-pause]
REM  (ASCII only: cmd.exe parses batch files in the OEM code page)
REM ============================================================

cd /d "%~dp0"
set "OSSDIR=%CD%"
set "REPO=%CD%\.."
set "T_START=%TIME%"

if /i "%~1"=="--no-pause" (set "NO_PAUSE=1") else (set "NO_PAUSE=0")

echo ============================================================
echo   Sandboxie-OSS unified build (x64, no Plus components)
echo   Repo : %REPO%
echo   Start: %DATE% %TIME%
echo ============================================================
echo.

REM When launched from Git Bash / MSYS shells, GNU find.exe and
REM link.exe shadow the Windows and MSVC ones and break the
REM repository scripts. Put System32 first.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

REM ------------------------------------------------------------
REM [0] Build environment (same bootstrap as root build.bat step 0,
REM     minus the Plus-only 7-Zip / SDK-19041 checks)
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
echo [0/8] Visual Studio: %VS_PATH%
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
echo [0/8] WDK kernel-mode include: %WDK_KM%

REM VC runtime redist source for the layout trio (set by vcvars64).
REM NOTE: %VCToolsRedistDir% contains parentheses ("Program Files (x86)")
REM - inside a parenthesized block it MUST stay quoted in echo, or the
REM expanded ')' prematurely closes the block (parse error, silent exit 0).
if not exist "%VCToolsRedistDir%\x64\Microsoft.VC143.CRT" (
    echo [ERROR] VC runtime redist not found: "%VCToolsRedistDir%"
    goto :fail
)

REM ------------------------------------------------------------
REM [1] Sandboxie core - Win32 (SbieDll/SbieSvc for 32-bit apps:
REM     the 32\ WOW64 pair of the layout)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [1/8] Building Sandboxie core Win32 (SbieRelease, WOW64 pair)
msbuild /t:build Sandboxie\SandboxDll.sln /p:Configuration="SbieRelease" /p:Platform=Win32 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [2] Sandboxie core - x64 (SbieSvc, SbieDll, SbieMsg, com
REM     stubs, KmdUtil; the driver project is listed in the sln
REM     but has no Build.0 entry, so it is skipped here)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [2/8] Building Sandboxie core x64 (SbieRelease)
msbuild /t:build Sandboxie\Sandbox.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [3] Sandboxie kernel driver (WDK)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [3/8] Building Sandboxie driver x64 (SbieDrv.sys, WDK)
msbuild /t:build Sandboxie\SandboxDrv.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [4] Sandboxie tools - ImBox target only (needed at runtime by
REM     SbieSvc MountManager; UpdUtil/MiniDump are Plus-side and
REM     are not built)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [4/8] Building ImBox x64 (SandboxieTools, target selection)
msbuild /t:ImBox SandboxieTools\SandboxieTools.sln /p:Configuration="Release" /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [5] Assemble the core runtime face into Installer\SbieOSS_x64
REM     (file list = make_dist.bat 21-file manifest face:
REM     docs/05-build.md section 6)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [5/8] Assembling Installer\SbieOSS_x64
set "OUT=%REPO%\Installer\SbieOSS_x64"
set "BIN64=%REPO%\Sandboxie\Bin\x64\SbieRelease"
set "BIN32=%REPO%\Sandboxie\Bin\Win32\SbieRelease"
set "TOOLS=%REPO%\SandboxieTools\x64\Release"

if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%OUT%" ( echo [ERROR] cannot clean stale "%OUT%" & goto :fail )
mkdir "%OUT%\32" "%OUT%\driver" || goto :fail

REM x64 core runtime
for %%f in (SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe) do copy /y "%BIN64%\%%f" "%OUT%\%%f" >nul || goto :fail
copy /y "%TOOLS%\ImBox.exe" "%OUT%\ImBox.exe" >nul || goto :fail
copy /y "%REPO%\Sandboxie\install\Templates.ini" "%OUT%\Templates.ini" >nul || goto :fail

REM VC runtime trio - layout runs on machines without the VC++ redist
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%VCToolsRedistDir%\x64\Microsoft.VC143.CRT\%%f" "%OUT%\%%f" >nul || goto :fail

REM WOW64 pair
for %%f in (SbieDll.dll SbieSvc.exe) do copy /y "%BIN32%\%%f" "%OUT%\32\%%f" >nul || goto :fail

REM Driver - WDK build output as-is, signing decoupled:
REM   sys from the build output; inf from the driver project source
REM   (byte-identical to what the Plus layout shipped); cat only if
REM   the build produced one (stock WDK build does not).
copy /y "%BIN64%\SbieDrv.sys" "%OUT%\driver\SbieDrv.sys" >nul || goto :fail
set "INF_SRC=%REPO%\Sandboxie\core\drv\SbieDrv.inf"
if exist "%BIN64%\SbieDrv.inf" set "INF_SRC=%BIN64%\SbieDrv.inf"
copy /y "%INF_SRC%" "%OUT%\driver\SbieDrv.inf" >nul || goto :fail
if exist "%BIN64%\SbieDrv.cat" copy /y "%BIN64%\SbieDrv.cat" "%OUT%\driver\SbieDrv.cat" >nul

REM ------------------------------------------------------------
REM [6] Sandboxie-OSS CLI (standalone-capable script; adds
REM     sbie-cli.exe into the layout)
REM ------------------------------------------------------------
echo.
echo [6/8] Building sbie-cli (SandboxieOSS)
call "%OSSDIR%\build_oss.bat"
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [7] Sandboxie-OSS GUI (adds sbie-gui\ into the layout)
REM ------------------------------------------------------------
echo.
echo [7/8] Building sbie-gui (SandboxieOSS)
call "%OSSDIR%\build_gui.bat"
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM [8] Verify the layout face + print it (files + sizes)
REM ------------------------------------------------------------
echo.
echo [8/8] Verifying layout
set "FACE=sbie-cli.exe SbieSvc.exe SbieDll.dll SbieMsg.dll KmdUtil.exe ImBox.exe SandboxieRpcSs.exe SandboxieDcomLaunch.exe Templates.ini msvcp140.dll vcruntime140.dll vcruntime140_1.dll"
set "MISSING="
for %%f in (%FACE%) do if not exist "%OUT%\%%f" call :add_missing "%%f"
for %%f in (SbieDll.dll SbieSvc.exe) do if not exist "%OUT%\32\%%f" call :add_missing "32\%%f"
for %%f in (SbieDrv.sys SbieDrv.inf) do if not exist "%OUT%\driver\%%f" call :add_missing "driver\%%f"
if not exist "%OUT%\sbie-gui\sbie-gui.exe" call :add_missing "sbie-gui\sbie-gui.exe"
if defined MISSING (
    echo [ERROR] incomplete layout:%MISSING%
    goto :fail
)

echo.
echo ===== Installer\SbieOSS_x64 layout =====
powershell -NoProfile -Command "$root='%OUT%'; $f=Get-ChildItem -LiteralPath $root -Recurse -File; $f | ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($root.Length+1) }; ''; 'TOTAL: {0} files, {1:N0} bytes' -f $f.Count, ($f | Measure-Object Length -Sum).Sum"
echo =========================================

echo.
echo ============================================================
echo   BUILD SUCCEEDED
echo   Output : %REPO%\Installer\SbieOSS_x64\
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
echo   BUILD FAILED
echo ============================================================
if "%NO_PAUSE%"=="0" pause
exit /b 1
