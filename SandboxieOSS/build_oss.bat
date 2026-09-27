@echo off
setlocal EnableExtensions
REM ============================================================
REM  Sandboxie-OSS (sbie-cli) standalone build - x64
REM  Spec: docs/05-build.md section 3 (ASCII only: cmd.exe
REM  parses batch files in the OEM code page)
REM  Output: ..\Installer\SbiePlus_x64\sbie-cli.exe
REM ============================================================
cd /d "%~dp0"
set "REPO=%CD%\.."

REM --- 0. vcvars64 bootstrap (same as root build.bat step 0) ---
REM System32 first on PATH: prevent GNU find/link from Git Bash
REM shadowing MSVC tools (docs/05-build.md 6.3 - do not remove)
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [ERROR] vswhere.exe not found & goto :fail )
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH ( echo [ERROR] No VS2022 C++ toolset & goto :fail )
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where msbuild >nul 2>&1 || ( echo [ERROR] msbuild not on PATH & goto :fail )
where cl >nul 2>&1 || ( echo [ERROR] cl.exe not on PATH & goto :fail )

REM --- 1. build (user-mode only, no WDK/Qt needed) ---
echo [1/2] Building SandboxieOSS x64 (Release)
msbuild /t:build SandboxieOSS.sln /p:Configuration=Release /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM --- 2. merge output into install layout ---
echo [2/2] Copying output
if not exist "%REPO%\Installer\SbiePlus_x64\" (
    echo [ERROR] %REPO%\Installer\SbiePlus_x64 missing - run root build.bat first.
    goto :fail
)
copy /y x64\Release\sbie-cli.exe "%REPO%\Installer\SbiePlus_x64\sbie-cli.exe"
if errorlevel 1 goto :fail

echo BUILD SUCCEEDED: %REPO%\Installer\SbiePlus_x64\sbie-cli.exe
REM Optional next step (not run automatically - see docs/05-build.md section 8):
REM   cmd /c make_dist.bat --verify   - assemble + zip dist\Sandboxie-OSS-x64
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
