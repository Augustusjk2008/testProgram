[CmdletBinding(PositionalBinding = $false)]
param(
    [switch] $Package,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $ApplicationArguments
)

$ErrorActionPreference = "Stop"

$PythonExe = "C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe"
$EnvironmentRoot = Split-Path -Parent $PythonExe
$QtRoot = Join-Path $EnvironmentRoot "Lib\site-packages\PyQt5\Qt5"
$QtPlugins = Join-Path $QtRoot "plugins"
$QtBin = Join-Path $QtRoot "bin"
$QtPlatformPlugins = Join-Path $QtPlugins "platforms"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$PackageName = "MB_DDF_HW_Test_PC"
$PackageRoot = Join-Path $ProjectRoot "build\test_pyqt"
$PackageDist = Join-Path $PackageRoot "dist"
$PackageWork = Join-Path $PackageRoot "work"
$PackageEntry = Join-Path $PSScriptRoot "package_entry.py"
$CatalogDirectory = Join-Path $ProjectRoot "docs\design\product_protocol_csv"

if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    Write-Error "Pinned Python interpreter not found: $PythonExe"
    exit 1
}
if (-not (Test-Path -LiteralPath $QtPlugins -PathType Container)) {
    Write-Error "Qt plugins directory not found: $QtPlugins"
    exit 1
}
if (-not (Test-Path -LiteralPath $QtBin -PathType Container)) {
    Write-Error "Qt bin directory not found: $QtBin"
    exit 1
}
if (-not (Test-Path -LiteralPath $QtPlatformPlugins -PathType Container)) {
    Write-Error "Qt platform plugin directory not found: $QtPlatformPlugins"
    exit 1
}

$env:QT_PLUGIN_PATH = $QtPlugins
$env:QT_QPA_PLATFORM_PLUGIN_PATH = $QtPlatformPlugins
$env:PATH = "$QtBin;$EnvironmentRoot\Library\bin;$env:PATH"

Push-Location $ProjectRoot
try {
    if ($Package) {
        if (-not (Test-Path -LiteralPath $PackageEntry -PathType Leaf)) {
            Write-Error "Package entry point not found: $PackageEntry"
            exit 1
        }
        if (-not (Test-Path -LiteralPath $CatalogDirectory -PathType Container)) {
            Write-Error "Product protocol CSV directory not found: $CatalogDirectory"
            exit 1
        }

        $PyInstallerVersion = (& $PythonExe -c "import PyInstaller; print(PyInstaller.__version__)" 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            Write-Error "PyInstaller is not available in pyqt5_env: $PyInstallerVersion"
            exit 1
        }
        Write-Host "Python: $PythonExe"
        Write-Host "PyInstaller: $PyInstallerVersion"
        if ($PyInstallerVersion -eq "6.17.0") {
            Write-Warning "PyInstaller 6.17.0 is not validated for the project's Windows 7 target; verify the package on Windows 7 SP1 x64 before delivery."
        }

        New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null
        $PyInstallerArguments = @(
            "--noconfirm",
            "--clean",
            "--onedir",
            "--windowed",
            "--noupx",
            "--name", $PackageName,
            "--paths", $ProjectRoot,
            "--distpath", $PackageDist,
            "--workpath", $PackageWork,
            "--specpath", $PackageRoot,
            "--add-data", ($CatalogDirectory + ";docs\design\product_protocol_csv"),
            $PackageEntry
        )
        & $PythonExe -m PyInstaller @PyInstallerArguments
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }

        $PackageExe = Join-Path $PackageDist (Join-Path $PackageName ($PackageName + ".exe"))
        if (-not (Test-Path -LiteralPath $PackageExe -PathType Leaf)) {
            Write-Error "Package completed without the expected executable: $PackageExe"
            exit 1
        }
        Write-Host "Package: $PackageExe"
        exit 0
    }

    & $PythonExe -m test_pyqt @ApplicationArguments
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
