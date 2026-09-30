@echo off
REM ---------------------------------------------------------------------------
REM sbie-gui - Sandboxie-OSS WinUI 3 management GUI (thin shell over sbie-cli.exe)
REM Copyright (C) 2026 Sandboxie-OSS contributors
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Build sbie-gui and publish it to ..\Installer\SbieOSS_x64\sbie-gui\
REM   1) dotnet build  -c Release          (compile, 0 errors required)
REM   2) dotnet publish -r win-x64 --self-contained false
REM      (.NET 8 framework-dependent + Windows App SDK self-contained)
REM
REM ASCII only: cmd.exe parses batch files in the OEM code page.
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set DOTNET=C:\Program Files\dotnet\dotnet.exe
if not exist "%DOTNET%" set DOTNET=dotnet

set PROJ=%~dp0sbie-gui\sbie-gui.csproj
set OUTDIR=%~dp0..\Installer\SbieOSS_x64\sbie-gui
set NUGET_SRC=https://api.nuget.org/v3/index.json

echo === [1/3] restore ===
"%DOTNET%" restore "%PROJ%"
if errorlevel 1 (
    echo restore failed - retrying with explicit nuget.org source...
    "%DOTNET%" restore "%PROJ%" --source %NUGET_SRC%
    if errorlevel 1 goto :fail
)

echo === [2/3] build (Release) ===
"%DOTNET%" build "%PROJ%" -c Release --no-restore
if errorlevel 1 goto :fail

echo === [3/3] publish -^> %OUTDIR% ===
if not exist "%~dp0..\Installer\SbieOSS_x64\" mkdir "%~dp0..\Installer\SbieOSS_x64" || goto :fail
if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
"%DOTNET%" publish "%PROJ%" -c Release -r win-x64 --self-contained false -o "%OUTDIR%" --no-restore
if errorlevel 1 goto :fail

if not exist "%OUTDIR%\sbie-gui.exe" (
    echo sbie-gui.exe missing from the publish output:
    dir "%OUTDIR%" 2>nul
    goto :fail
)

echo.
echo OK: %OUTDIR%\sbie-gui.exe
endlocal & exit /b 0

:fail
echo BUILD FAILED
endlocal & exit /b 1
