[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('help', 'configure', 'build', 'test', 'tui', 'run', 'ports', 'clean')]
    [string]$Action = 'help',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$BuildDir = 'build_vs',
    [string]$TestConfig = 'configs/mbddf_system_status.testcfg.json',
    [string]$HalConfig = 'configs/mbddf_pc_hal.json',
    [string]$ProtocolCsvDir = '',
    [string]$Control = '',
    [string]$Port = '',
    [string]$QtPrefix = '',
    [string]$TestRegex = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$GTestVersion = '1.15.2'
$GTestSha256 = 'F179EC217F9B3B3F3C6E8B02D3E7EDA997B49E4CE26D6B235C9053BEC9C0BF9F'

function Show-HwtestHelp {
    @'
hwtest.ps1 <action> [options]

Actions:
  help       Show this help without configuring or building.
  configure  Configure a product-only Visual Studio build.
  build      Configure and build all product targets.
  test       Configure, build, and run the complete CTest suite.
  tui        Build and start the staged SYSTEM_STATUS console.
  run        Build and run SYSTEM_STATUS once.
  ports      Build the TUI and list serial ports without opening one.
  clean      Run the CMake clean target; keep the configured build tree.

Common options:
  -Configuration Debug|Release
  -BuildDir <path>
  -TestConfig <path>
  -HalConfig <path>
  -ProtocolCsvDir <path>
  -Control <ResourceId>
  -Port <port-name>
  -QtPrefix <Qt CMake prefix>
  -TestRegex <CTest regex>
  -SkipBuild

Examples:
  .\hwtest.ps1 build
  .\hwtest.ps1 test
  .\hwtest.ps1 test -TestRegex "^(HalTypesTest|TuiShellTest)\."
  .\hwtest.ps1 ports
  .\hwtest.ps1 tui -Port COM7
  .\hwtest.ps1 run -Control CONTROL_SERIAL -Port COM7
  .\hwtest.ps1 run -HalConfig .\configs\my_pc_hal.json
'@ | Write-Host
}

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath $($Arguments -join ' ')"
    }
}

function Assert-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label does not exist: $Path"
    }
}

function Assert-Directory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label does not exist: $Path"
    }
}

function Ensure-GTestSource {
    $depsDir = Join-Path $RepoRoot 'tmp\deps'
    $archivePath = Join-Path $depsDir "googletest-v$GTestVersion.zip"
    $sourcePath = Join-Path $depsDir "googletest-$GTestVersion"
    $sourceCMake = Join-Path $sourcePath 'CMakeLists.txt'
    if (Test-Path -LiteralPath $sourceCMake -PathType Leaf) {
        return $sourcePath
    }

    New-Item -ItemType Directory -Path $depsDir -Force | Out-Null
    $downloadRequired = $true
    if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
        $downloadRequired = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $GTestSha256
    }
    if ($downloadRequired) {
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($null -eq $curl) {
            throw 'curl.exe is required to download the pinned GoogleTest source.'
        }
        Invoke-Native $curl.Source @(
            '--fail', '--location', '--http1.1', '--retry', '5',
            '--output', $archivePath,
            "https://github.com/google/googletest/archive/refs/tags/v$GTestVersion.zip"
        )
    }

    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    if ($actualHash -ne $GTestSha256) {
        throw "GoogleTest archive hash mismatch: expected $GTestSha256, got $actualHash"
    }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $depsDir -Force
    Assert-File $sourceCMake 'GoogleTest source CMakeLists.txt'
    return $sourcePath
}

function Configure-Build([bool]$WithTests) {
    $testing = if ($WithTests) { 'ON' } else { 'OFF' }
    $arguments = @(
        '-S', $RepoRoot,
        '-B', $BuildPath,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        "-DBUILD_TESTING=$testing"
    )
    if (-not [string]::IsNullOrWhiteSpace($QtPrefix)) {
        $arguments += "-DCMAKE_PREFIX_PATH=$(Resolve-RepoPath $QtPrefix)"
    }
    if ($WithTests) {
        $gtestSource = Ensure-GTestSource
        $arguments += "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$($gtestSource.Replace('\', '/'))"
    }
    Invoke-Native 'cmake' $arguments
}

function Build-Targets([string[]]$Targets = @()) {
    $arguments = @('--build', $BuildPath, '--config', $Configuration, '--parallel')
    if ($Targets.Count -gt 0) {
        $arguments += '--target'
        $arguments += $Targets
    }
    Invoke-Native 'cmake' $arguments
}

function Resolve-ProtocolAssets {
    $path = $ProtocolCsvDir
    if ([string]::IsNullOrWhiteSpace($path)) {
        $path = $env:MB_DDF_PROTOCOL_CSV_DIR
    }
    if ([string]::IsNullOrWhiteSpace($path)) {
        $path = 'H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv'
    }
    $resolved = Resolve-RepoPath $path
    Assert-Directory $resolved 'MB_DDF protocol CSV directory'
    return $resolved
}

function Application-Arguments {
    $arguments = @('--test-config', $TestConfigPath, '--hal-config', $HalConfigPath)
    if (-not [string]::IsNullOrWhiteSpace($Control)) {
        $arguments += @('--control', $Control.Trim())
    }
    if (-not [string]::IsNullOrWhiteSpace($Port)) {
        $arguments += @('--serial-port', $Port.Trim())
    }
    return $arguments
}

try {
    if ($Action -eq 'help') {
        Show-HwtestHelp
        exit 0
    }

    $BuildPath = Resolve-RepoPath $BuildDir
    $TestConfigPath = Resolve-RepoPath $TestConfig
    $HalConfigPath = Resolve-RepoPath $HalConfig

    switch ($Action) {
        'configure' {
            if ($SkipBuild) { throw '-SkipBuild is not valid with configure.' }
            Configure-Build $false
        }
        'build' {
            if ($SkipBuild) { throw '-SkipBuild is not valid with build.' }
            Configure-Build $false
            Build-Targets
        }
        'test' {
            $env:MB_DDF_PROTOCOL_CSV_DIR = Resolve-ProtocolAssets
            Configure-Build $true
            if (-not $SkipBuild) {
                Build-Targets
            }
            $arguments = @('--test-dir', $BuildPath, '-C', $Configuration, '--output-on-failure')
            if (-not [string]::IsNullOrWhiteSpace($TestRegex)) {
                $arguments += @('-R', $TestRegex)
            }
            Invoke-Native 'ctest' $arguments
        }
        'tui' {
            Assert-File $TestConfigPath 'Test configuration'
            Assert-File $HalConfigPath 'HAL configuration'
            $env:MB_DDF_PROTOCOL_CSV_DIR = Resolve-ProtocolAssets
            if (-not $SkipBuild) {
                Configure-Build $false
                Build-Targets @('hwtest_tui')
            }
            $executable = Join-Path $BuildPath "src\app\$Configuration\hwtest_tui.exe"
            Assert-File $executable 'TUI executable'
            & $executable @(Application-Arguments)
            exit $LASTEXITCODE
        }
        'run' {
            Assert-File $TestConfigPath 'Test configuration'
            Assert-File $HalConfigPath 'HAL configuration'
            $env:MB_DDF_PROTOCOL_CSV_DIR = Resolve-ProtocolAssets
            if (-not $SkipBuild) {
                Configure-Build $false
                Build-Targets @('hwtest_pc_runner')
            }
            $executable = Join-Path $BuildPath "src\app\$Configuration\hwtest_pc_runner.exe"
            Assert-File $executable 'runner executable'
            & $executable @(Application-Arguments)
            exit $LASTEXITCODE
        }
        'ports' {
            if (-not $SkipBuild) {
                Configure-Build $false
                Build-Targets @('hwtest_tui')
            }
            $executable = Join-Path $BuildPath "src\app\$Configuration\hwtest_tui.exe"
            Assert-File $executable 'TUI executable'
            @('ports', 'quit') | & $executable @(Application-Arguments)
            exit $LASTEXITCODE
        }
        'clean' {
            if ($SkipBuild) { throw '-SkipBuild is not valid with clean.' }
            $cache = Join-Path $BuildPath 'CMakeCache.txt'
            if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
                Write-Host "Build tree is already absent: $BuildPath"
                exit 0
            }
            Invoke-Native 'cmake' @('--build', $BuildPath, '--config', $Configuration, '--target', 'clean')
        }
    }
    exit 0
}
catch {
    Write-Error $_.Exception.Message
    exit 2
}
