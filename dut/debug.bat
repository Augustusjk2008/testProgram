@echo off
setlocal
rem Windows 7 Compatible Wrapper for debug.ps1
rem Bypasses PowerShell Execution Policy automatically

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%debug.ps1"

rem Check if PowerShell is available
where powershell >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: PowerShell is not found in PATH.
    exit /b 1
)

rem Invoke PowerShell with Bypass policy
set "PS_ARGS="
if /I "%~1"=="com3_echo" (
    rem Build, deploy and run the isolated COM3 payload echo image.
    echo COM3 echo image enabled.
    set "PS_ARGS=-Run -Com3Echo"
    shift
    goto arg_loop
)
if /I "%~1"=="hw_test_run" (
    rem Build, deploy and run the isolated product hardware-test service.
    echo Hardware-test service image enabled.
    echo WARNING: This image executes product protocol writes without safety confirmation or state restoration.
    set "PS_ARGS=-Run -HardwareTest"
    shift
    goto arg_loop
)
if /I "%~1"=="hw_run" (
    rem Build, deploy and run the complete writable MB_DDF_HW capability suite.
    echo Full hardware Demo image enabled.
    echo WARNING: Full hardware writes are enabled, including DH main engine 2 ignition.
    echo WARNING: /dev/spidev0.0 Flash test temporarily rewrites and restores one 4 KiB subsector.
    set "PS_ARGS=-Run -FullHardware"
    shift
    goto arg_loop
)
if /I "%~1"=="run" (
    set "PS_ARGS=-Run"
    shift
)

:arg_loop
if "%~1"=="" goto arg_end
set "PS_ARGS=%PS_ARGS% %1"
shift
goto arg_loop

:arg_end
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %PS_ARGS% -ForegroundGdbserver

set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
