#Requires -Version 5.1
<#
.SYNOPSIS
    全量实机测试入口：静态检查、构建、部署并运行全部目标板测试二进制。

.EXAMPLE
    .\tests\test-all.ps1 -BuildOnly
    .\tests\test-all.ps1
    .\tests\test-all.ps1 -RemoteHost 192.168.1.50 -SaveResults
#>
param(
    [string]$RemoteHost = "192.168.1.29",
    [string]$RemoteDir = "/home/sast8/user_tests",
    [string]$ResultsDir = "",
    [switch]$BuildOnly,
    [switch]$SaveResults,
    [switch]$WithCoverage,
    [switch]$SkipStaticChecks
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

function Write-Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Green }
function Write-Step($msg) { Write-Host "`n[STEP] $msg" -ForegroundColor Cyan }
function Write-ErrorLine($msg) { Write-Host "[ERROR] $msg" -ForegroundColor Red }

function Invoke-NoMatchCheck([string]$Pattern, [string[]]$Paths) {
    & rg -n $Pattern @Paths
    if ($LASTEXITCODE -eq 0) {
        throw "Static check failed: pattern '$Pattern' found in $($Paths -join ', ')"
    }
    if ($LASTEXITCODE -gt 1) {
        throw "rg failed with exit code $LASTEXITCODE"
    }
}

function Assert-MissingPaths([string[]]$Paths) {
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            throw "Static check failed: unexpected path exists: $path"
        }
    }
}

function Invoke-TestBinary([string]$BinaryName, [string]$Filter) {
    $deployScript = Join-Path $PSScriptRoot "test-deploy.ps1"
    $deployArgs = @(
        "-RemoteHost", $RemoteHost,
        "-RemoteDir", $RemoteDir,
        "-TestFilter", $Filter,
        "-TestBinaryName", $BinaryName
    )

    if ($ResultsDir -ne "") { $deployArgs += @("-ResultsDir", $ResultsDir) }
    if ($BuildOnly) { $deployArgs += "-BuildOnly" }
    if ($SaveResults) { $deployArgs += "-SaveResults" }
    if ($WithCoverage) { $deployArgs += "-WithCoverage" }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $deployScript @deployArgs 2>&1 |
        ForEach-Object { Write-Host $_ }
    $exitCode = $LASTEXITCODE
    return $exitCode
}

Push-Location $ProjectRoot
try {
    Write-Step "Full target-board test entry"
    Write-Info "Remote: $RemoteHost"
    Write-Info "Target: $RemoteDir"
    if ($BuildOnly) {
        Write-Info "BuildOnly: enabled"
    }

    if (-not $SkipStaticChecks) {
        Write-Step "Running DDS-only static checks"
        Assert-MissingPaths @(
            "src\Demo",
            "src\No8Demo",
            "src\Tools\slog_reader.cpp",
            "src\MB_DDF\Endpoint",
            "src\MB_DDF\Monitor",
            "src\MB_DDF\No8_Compatibility",
            "src\MB_DDF\PhysicalLayer",
            "src\MB_DDF\Timer",
            "src\MB_DDF\Tools"
        )
        Invoke-NoMatchCheck "futex|SYS_futex|linux/futex|FUTEX_" @("src\MB_DDF\DDS")
        Invoke-NoMatchCheck "EndpointPtr|DDSEndpoint|DDSHandle|PubAndSub|Endpoint::Port|wait_event|event_callback_|endpoint_" @("src\MB_DDF\DDS")
        Write-Info "Static checks passed"
    }

    $failures = @()

    Write-Step "Running unit/integration test binary"
    $unitExit = Invoke-TestBinary -BinaryName "MB_DDF_v2_Tests" -Filter "*"
    if ($unitExit -ne 0) {
        $failures += "MB_DDF_v2_Tests=$unitExit"
    }

    Write-Step "Running hardware test binary"
    $hardwareExit = Invoke-TestBinary -BinaryName "MB_DDF_v2_HardwareTests" -Filter "*"
    if ($hardwareExit -ne 0) {
        $failures += "MB_DDF_v2_HardwareTests=$hardwareExit"
    }

    Write-Step "Running MB_DDF_HW unit test binary"
    $hwUnitExit = Invoke-TestBinary -BinaryName "MB_DDF_HW_Tests" -Filter "*"
    if ($hwUnitExit -ne 0) {
        $failures += "MB_DDF_HW_Tests=$hwUnitExit"
    }

    Write-Step "Running MB_DDF_HW smoke test binary"
    $hwSmokeExit = Invoke-TestBinary -BinaryName "MB_DDF_HW_Smoke" -Filter "*"
    if ($hwSmokeExit -ne 0) {
        $failures += "MB_DDF_HW_Smoke=$hwSmokeExit"
    }

    Write-Step "Full Test Summary"
    if ($failures.Count -eq 0) {
        Write-Info "All requested test binaries completed successfully"
        exit 0
    }

    foreach ($failure in $failures) {
        Write-ErrorLine $failure
    }
    exit 1
}
finally {
    Pop-Location
}
