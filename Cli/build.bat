@echo off
setlocal EnableExtensions
REM ============================================================
REM  Cli build - sbie-rt x64 (unified V2+V3 command line)
REM  Links the CoreRT SDK (CoreRT\sdk\SbieCore\SbieCore.lib via
REM  ProjectReference).
REM  Output: ..\Installer\SbieOSS_x64\sbie-rt.exe
REM  (normally produced after CoreRT\build.bat has assembled the
REM  core face; can also run standalone - the SDK project builds
REM  on demand through the ProjectReference)
REM ============================================================
cd /d "%~dp0"
set "REPO=%CD%\.."

REM --- 0. vcvars64 bootstrap (same as CoreRT\build.bat step 0) ---
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
echo [1/2] Building sbie-rt x64 (Release)
msbuild /t:build sbie-rt.vcxproj /p:Configuration=Release /p:Platform=x64 -maxcpucount:8 -nologo -v:m
if errorlevel 1 goto :fail

REM --- 2. merge output into install layout ---
echo [2/2] Copying output
if not exist "%REPO%\Installer\SbieOSS_x64\" mkdir "%REPO%\Installer\SbieOSS_x64" || goto :fail
copy /y x64\Release\sbie-rt.exe "%REPO%\Installer\SbieOSS_x64\sbie-rt.exe"
if errorlevel 1 goto :fail

echo BUILD SUCCEEDED: %REPO%\Installer\SbieOSS_x64\sbie-rt.exe
REM Optional next step (not run automatically - see docs/05-build.md section 8):
REM   cmd /c ..\make_dist.bat --verify  - assemble + zip dist\Sandboxie-OSS-x64
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
