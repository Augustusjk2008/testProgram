#Requires -Version 5.1
<#
.SYNOPSIS
    DDS 独立版一键检查、构建、部署测试。

.EXAMPLE
    .\tests\test-dds-only.ps1 -BuildOnly
    .\tests\test-dds-only.ps1 -RemoteHost 192.168.1.50
#>
param(
    [string]$RemoteHost = "192.168.1.29",
    [string]$RemoteDir = "/home/sast8/user_tests",
    [string]$TestFilter = "Message*:*RingBuffer*:*SharedMemory*:*TopicRegistry*:*PubSub*:*DDSCore*:*SemaphoreGuard*:*ExternalEndpoint*:*ExternalPort*:*GatewayEnvelope*:*DomainGateway*:*DdsGatewayLocalBus*",
    [string]$ResultsDir = "",
    [switch]$BuildOnly,
    [switch]$SaveResults,
    [switch]$WithCoverage,
    [switch]$SkipStaticChecks
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

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

Push-Location $ProjectRoot
try {
    if (-not $SkipStaticChecks) {
        Assert-MissingPaths @(
            "src\Demo",
            "src\No8Demo",
            "src\Tools\slog_reader.cpp",
            "src\MB_DDF\Endpoint",
            "src\MB_DDF\Monitor",
            "src\MB_DDF\No8_Compatibility",
            "src\MB_DDF\PhysicalLayer",
            "src\MB_DDF\Tools"
        )
        Invoke-NoMatchCheck "futex|SYS_futex|linux/futex|FUTEX_" @("src\MB_DDF\DDS")
        Invoke-NoMatchCheck "EndpointPtr|DDSEndpoint|DDSHandle|PubAndSub|Endpoint::Port|wait_event|event_callback_|endpoint_" @("src\MB_DDF\DDS")
    }

    $deployArgs = @{
        RemoteHost = $RemoteHost
        RemoteDir = $RemoteDir
        TestFilter = $TestFilter
    }
    if ($ResultsDir -ne "") { $deployArgs.ResultsDir = $ResultsDir }
    if ($BuildOnly) { $deployArgs.BuildOnly = $true }
    if ($SaveResults) { $deployArgs.SaveResults = $true }
    if ($WithCoverage) { $deployArgs.WithCoverage = $true }

    & (Join-Path $PSScriptRoot "test-deploy.ps1") @deployArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
