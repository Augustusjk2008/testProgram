@echo off
rem Windows 7 Compatible Wrapper for build.ps1
rem Bypasses PowerShell Execution Policy automatically

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%build.ps1"

rem Check if PowerShell is available
where powershell >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: PowerShell is not found in PATH.
    exit /b 1
)

rem Invoke PowerShell with Bypass policy
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*

if %ERRORLEVEL% NEQ 0 (
    exit /b %ERRORLEVEL%
)
