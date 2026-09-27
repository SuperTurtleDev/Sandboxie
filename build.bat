@echo off
REM NOTE: keep delayed expansion OFF - the repository build scripts contain
REM literal '!' characters (7-Zip -i!wildcard switches) that delayed
REM expansion would strip, breaking them exactly as described in the CI docs.
setlocal EnableExtensions

REM ============================================================
REM  Sandboxie-Plus one-click build (x64 / Qt6)
REM  Exact mirror of the CI job Build_x64_Qt6 in
REM  .github/workflows/main.yml (windows-2022 runner).
REM
REM  Toolchain (deploy with setup_env.bat):
REM    VS 2022 BuildTools + v143 + v142 toolset + ATL/MFC
REM    Windows 10 SDK 10.0.19041.0
REM    Windows Driver Kit 10.0.26100
REM    7-Zip at C:\Program Files\7-Zip\7z.exe
REM    Qt 6.8.3 / jom / OpenSSL / 7z runtime: downloaded
REM    automatically by the repository's own scripts.
REM
REM  Output: Installer\SbiePlus_x64\   (complete runtime layout)
REM          Installer\Assets\         (installer assets)
REM ============================================================

cd /d "%~dp0"
set "REPO=%CD%"
set "T_START=%TIME%"

if /i "%~1"=="--no-pause" (set "NO_PAUSE=1") else (set "NO_PAUSE=0")

echo ============================================================
echo   Sandboxie-Plus build (x64, Qt6)
echo   Repo : %REPO%
echo   Start: %DATE% %TIME%
echo ============================================================
echo.

REM When launched from Git Bash / MSYS shells, GNU find.exe and
REM link.exe shadow the Windows and MSVC ones and break the
REM repository scripts. Put System32 first.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

REM ------------------------------------------------------------
REM 0. Build environment
REM    CI runners use VS 2022 Enterprise; the repository scripts
REM    (qmake_plus.cmd / copy_build.cmd) hard-code its vcvars path.
REM    Locally we pre-load the same environment from whatever VS
REM    2022 instance is installed, which makes those hard-coded
REM    calls harmless while cl/link/msbuild stay available.
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
    echo         Run setup_env.bat first to deploy the toolchain.
    goto :fail
)
echo [0/10] Visual Studio: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where msbuild >nul 2>&1 || (echo [ERROR] msbuild not on PATH after vcvars64 & goto :fail)
where cl >nul 2>&1 || (echo [ERROR] cl.exe not on PATH after vcvars64 & goto :fail)

if not exist "C:\Program Files\7-Zip\7z.exe" (
    echo [ERROR] C:\Program Files\7-Zip\7z.exe not found. Run: winget install 7zip.7zip
    goto :fail
)
if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.19041.0" (
    echo [ERROR] Windows 10 SDK 10.0.19041.0 not found - required by SbieShell.
    echo         Run setup_env.bat first.
    goto :fail
)
if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.26100.0\km" (
    echo [ERROR] WDK 10.0.26100 not found - required by SbieDrv.sys.
    echo         Run setup_env.bat first.
    goto :fail
)

REM ------------------------------------------------------------
REM 1. Sandboxie core - Win32 DLLs & service (SbieDll for 32-bit apps)
REM ------------------------------------------------------------
echo.
echo [1/10] Building Sandboxie core x86 DLLs + SbieSvc (SbieRelease, Win32)
msbuild /t:build Sandboxie\SandboxDll.sln /p:Configuration="SbieRelease" /p:Platform=Win32 -maxcpucount:8
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 2. Sandboxie core - x64 applications, service and DLLs
REM ------------------------------------------------------------
echo.
echo [2/10] Building Sandboxie core x64 (SbieRelease)
msbuild /t:build Sandboxie\Sandbox.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 3. Sandboxie kernel driver SbieDrv.sys
REM ------------------------------------------------------------
echo.
echo [3/10] Building Sandboxie driver x64 (SbieDrv.sys, WDK)
msbuild /t:build Sandboxie\SandboxDrv.sln /p:Configuration="SbieRelease" /p:Platform=x64 -maxcpucount:8
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 4. Qt 6.8.3 + jom (downloaded once into ..\Qt next to the repo)
REM ------------------------------------------------------------
echo.
echo [4/10] Ensuring Qt + jom toolchain
call SandboxiePlus\install_qt.cmd x64
if errorlevel 1 goto :fail
call SandboxiePlus\install_jom.cmd
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 5. Sandboxie-Plus Qt UI (SandMan + libraries)
REM ------------------------------------------------------------
echo.
echo [5/10] Building Sandboxie-Plus UI x64 (Qt6)
call SandboxiePlus\qmake_plus.cmd x64 build_qt6
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 6. Explorer shell extension (v142 / SDK 19041, as pinned by
REM    the projects and provided by the CI runner image)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [6/10] Building SbieShell x64
msbuild /t:restore,build -p:RestorePackagesConfig=true SandboxiePlus\SbieShell\SbieShell.sln /p:Configuration="Release" /p:Platform=x64
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 7. Sandboxie tools (ImBox, UpdUtil, MiniDump)
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [7/10] Building Sandboxie-Tools x64
msbuild /t:build SandboxieTools\SandboxieTools.sln /p:Configuration="Release" /p:Platform=x64 -maxcpucount:8
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 8. Runtime dependencies (Qt translations, OpenSSL, 7z DLLs)
REM    Re-extracting over existing files makes 7-Zip prompt for
REM    confirmation; clear the folders first so reruns behave like
REM    a fresh CI checkout.
REM ------------------------------------------------------------
cd /d "%REPO%"
if exist "Installer\OpenSSL" rmdir /s /q "Installer\OpenSSL"
if exist "Installer\7-Zip" rmdir /s /q "Installer\7-Zip"
echo.
echo [8/10] Fetching runtime dependencies
call Installer\fix_qt5_languages.cmd x64 build_qt6
if errorlevel 1 goto :fail
call Installer\get_openssl.cmd
if errorlevel 1 goto :fail
call Installer\get_7zip.cmd
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 9. Merge everything into Installer\SbiePlus_x64
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [9/10] Merging build output
call Installer\copy_build.cmd x64 build_qt6
if errorlevel 1 goto :fail

REM ------------------------------------------------------------
REM 10. Collect installer assets
REM ------------------------------------------------------------
cd /d "%REPO%"
echo.
echo [10/10] Collecting installer assets
call Installer\get_assets.cmd
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo   BUILD SUCCEEDED
echo   Output : %REPO%\Installer\SbiePlus_x64\
echo   Started: %T_START%   Finished: %TIME%
echo ============================================================
if not exist "Installer\SbiePlus_x64\SandMan.exe" (
    echo [WARN] SandMan.exe missing from the output folder!
    goto :fail
)
if "%NO_PAUSE%"=="0" pause
exit /b 0

:fail
echo.
echo ============================================================
echo   BUILD FAILED
echo ============================================================
if "%NO_PAUSE%"=="0" pause
exit /b 1
