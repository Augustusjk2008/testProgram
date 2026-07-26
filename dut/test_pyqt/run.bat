@echo off
setlocal
rem Windows 7 compatible wrapper for the pinned PyQt5 environment.
rem Usage: run.bat [package]

set "SCRIPT_DIR=%~dp0"
where powershell.exe >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: PowerShell is not found in PATH.
    exit /b 1
)

if /I "%~1"=="package" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%run.ps1" -Package
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%run.ps1" %*
)
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
