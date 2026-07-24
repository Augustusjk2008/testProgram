#Requires -Version 5.1
<#
.SYNOPSIS
    构建并部署 DDS standalone 测试到目标板执行

.DESCRIPTION
    1. 构建测试二进制（交叉编译到AArch64）
    2. 部署到目标板
    3. 执行测试并输出结果

    可选功能（通过参数启用）：
    - 保存测试结果到本地（-SaveResults）
    - 回传覆盖率数据并生成HTML报告（-WithCoverage，需要 -SaveResults）

.PARAMETER RemoteHost
    目标板IP地址，默认192.168.1.29

.PARAMETER RemoteDir
    目标板测试目录，默认/home/sast8/user_tests

.PARAMETER TestFilter
    gtest测试过滤器，默认*（全部）

.PARAMETER TestBinaryName
    测试二进制名称，默认MB_DDF_v2_Tests

.PARAMETER BuildOnly
    仅构建不上传执行

.PARAMETER WithCoverage
    生成覆盖率报告（需要lcov工具）

.PARAMETER SaveResults
    保存测试结果到本地目录

.PARAMETER ResultsDir
    本地测试结果保存目录，默认tests/results

.EXAMPLE
    .\test-deploy.ps1
    .\test-deploy.ps1 -TestFilter "Message*"
    .\test-deploy.ps1 -RemoteHost 192.168.1.29
    .\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests
    .\test-deploy.ps1 -BuildOnly
    .\test-deploy.ps1 -SaveResults
    .\test-deploy.ps1 -SaveResults -WithCoverage
#>
param(
    [string]$RemoteHost = "192.168.1.29",
    [string]$RemoteDir = "/home/sast8/user_tests",
    [string]$TestFilter = "*",
    [string]$TestBinaryName = "MB_DDF_v2_Tests",
    [string]$TestArguments = "",
    [string]$ResultsDir = "",
    [switch]$BuildOnly,
    [switch]$SaveResults,
    [switch]$WithCoverage
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$TestsRoot = $PSScriptRoot

# 设置默认结果目录
if ([string]::IsNullOrEmpty($ResultsDir)) {
    $ResultsDir = Join-Path $TestsRoot "results"
}

# 生成时间戳（仅在需要保存结果时）
$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ResultSessionDir = $null
if ($SaveResults) {
    $ResultSessionDir = Join-Path $ResultsDir $Timestamp
}

# 颜色输出
function Write-Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Green }
function Write-Step($msg) { Write-Host "`n[STEP] $msg" -ForegroundColor Cyan }
function Write-Error($msg) { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function Write-Warn($msg) { Write-Host "[WARN] $msg" -ForegroundColor Yellow }

Write-Step "DDS Standalone Test Deploy Script"
Write-Info "Remote: $RemoteHost"
Write-Info "Target: $RemoteDir"
Write-Info "Filter: $TestFilter"
Write-Info "TestBinary: $TestBinaryName"
if ($TestArguments -ne "") { Write-Info "TestArguments: $TestArguments" }
if ($SaveResults) {
    Write-Info "Results will be saved to: $ResultSessionDir"
}
if ($WithCoverage) {
    Write-Info "Coverage report will be generated"
}

# ==============================
# 0. 准备结果目录（仅在需要时）
# ==============================
if ($SaveResults) {
    Write-Step "Preparing results directory..."

    if (-not (Test-Path $ResultSessionDir)) {
        New-Item -ItemType Directory -Path $ResultSessionDir -Force | Out-Null
    }

    # 创建子目录
    $CoverageDir = Join-Path $ResultSessionDir "coverage"
    $GcdaDir = Join-Path $ResultSessionDir "gcda"
    New-Item -ItemType Directory -Path $CoverageDir -Force | Out-Null
    New-Item -ItemType Directory -Path $GcdaDir -Force | Out-Null

    Write-Info "Results will be saved to: $ResultSessionDir"
}

# ==============================
# 1. 构建测试
# ==============================
Write-Step "Building tests..."

$BuildScript = Join-Path $ProjectRoot "build.bat"
if (-not (Test-Path $BuildScript)) {
    Write-Error "build.bat not found at $BuildScript"
    exit 1
}

# DDS-only 与 MB_DDF_HW 测试共享 tests 目录，但每次都用完整显式参数重新配置；
# DDS-only 不应被协议生成器、硬件层或 Adapter 依赖阻塞。
$BuildAction = "dds_tests"
if ($TestBinaryName -eq "MB_DDF_HW_Tests" -or $TestBinaryName -eq "MB_DDF_HW_Smoke") {
    $BuildAction = "hw_tests"
}
& $BuildScript $BuildAction

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE"
    exit 1
}

Write-Info "Build completed"

# ==============================
# 2. 查找测试二进制
# ==============================
Write-Step "Locating test binary..."

$TestsBuildDir = Join-Path $ProjectRoot "build\aarch64\tests\Debug"
$candidateTestBinaries = @(
    (Join-Path (Join-Path $TestsBuildDir "tests") $TestBinaryName),
    (Join-Path (Join-Path $TestsBuildDir "tests") ($TestBinaryName + ".exe")),
    (Join-Path $TestsBuildDir $TestBinaryName),
    (Join-Path $TestsBuildDir ($TestBinaryName + ".exe"))
)
$TestBinaryPath = ""
foreach ($candidate in $candidateTestBinaries) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $TestBinaryPath = $candidate
        break
    }
}
if (-not $TestBinaryPath) {
    $foundTestBinary = Get-ChildItem -Path $TestsBuildDir -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq $TestBinaryName -or $_.Name -eq ($TestBinaryName + ".exe") } |
        Select-Object -First 1
    if ($foundTestBinary) {
        $TestBinaryPath = $foundTestBinary.FullName
    }
}

if (-not $TestBinaryPath) {
    Write-Error "Test binary $TestBinaryName not found in build output"
    Write-Error "Checked tests build directory: $TestsBuildDir"
    exit 1
}

Write-Info "Found: $TestBinaryPath"

if ($BuildOnly) {
    Write-Info "Build-only mode, skipping deploy"
    exit 0
}

# ==============================
# 3. 检查目标板连通性
# ==============================
Write-Step "Checking target connectivity..."

$pingResult = Test-Connection -ComputerName $RemoteHost -Count 1 -Quiet -ErrorAction SilentlyContinue
if (-not $pingResult) {
    Write-Error "Target $RemoteHost is not reachable"
    exit 1
}

Write-Info "Target is online"

# ==============================
# 4. 部署测试二进制
# ==============================
Write-Step "Deploying to target..."

# 创建远程目录
ssh root@$RemoteHost "mkdir -p $RemoteDir" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to create remote directory. Check SSH key authentication."
    exit 1
}

# 上传测试文件
scp $TestBinaryPath root@${RemoteHost}:${RemoteDir}/$TestBinaryName
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to upload test binary"
    exit 1
}

# 设置执行权限
ssh root@$RemoteHost "chmod +x $RemoteDir/$TestBinaryName" 2>$null

Write-Info "Deploy completed"

# ==============================
# 5. 执行测试
# ==============================
Write-Step "Running tests on target..."
Write-Host "----------------------------------------"

$TestOutputFile = $null
if ($TestBinaryName -eq "MB_DDF_HW_Smoke") {
    $testCmd = "cd $RemoteDir && ./$TestBinaryName $TestArguments 2>&1"
} else {
    $filterArgument = "--gtest_filter='$TestFilter'"
    if ($TestArguments -match '(^|\s)--gtest_filter(?:=|\s)') {
        $filterArgument = ""
    }
    $testCmd = "cd $RemoteDir && ./$TestBinaryName $filterArgument --gtest_color=yes $TestArguments 2>&1"
}

if ($SaveResults) {
    $TestOutputFile = Join-Path $ResultSessionDir "test_output.txt"
}

# 执行测试并捕获输出
$TestOutput = ssh root@$RemoteHost $testCmd 2>&1
$testExitCode = $LASTEXITCODE

# 显示输出
$TestOutput | ForEach-Object { Write-Host $_ }

# 保存到文件（仅在需要时）
if ($SaveResults -and $TestOutputFile) {
    $TestOutput | Out-File -FilePath $TestOutputFile -Encoding UTF8
    Write-Host "----------------------------------------"
    Write-Info "Test output saved to: $TestOutputFile"
} else {
    Write-Host "----------------------------------------"
}

# ==============================
# 6. 回传覆盖率数据（如果启用）
# ==============================
if ($WithCoverage -and $SaveResults) {
    Write-Step "Collecting coverage data from target..."

    # 检查远程是否有gcda文件
    $checkGcdaCmd = "ls $RemoteDir/*.gcda 2>/dev/null | wc -l"
    $gcdaCount = ssh root@$RemoteHost $checkGcdaCmd 2>$null

    if ($gcdaCount -gt 0) {
        Write-Info "Found $gcdaCount .gcda files on target"

        # 下载gcda文件
        scp root@${RemoteHost}:${RemoteDir}/*.gcda $GcdaDir/
        if ($LASTEXITCODE -eq 0) {
            Write-Info "Downloaded .gcda files to: $GcdaDir"

            # 同时下载.gcno文件（从构建目录复制）
            $GcnoSourceDir = Join-Path $TestsBuildDir "tests\CMakeFiles\$TestBinaryName.dir"
            if (Test-Path $GcnoSourceDir) {
                $GcnoDestDir = Join-Path $ResultSessionDir "gcno"
                New-Item -ItemType Directory -Path $GcnoDestDir -Force | Out-Null

                # 复制gcno文件
                Get-ChildItem -Path $GcnoSourceDir -Filter "*.gcno" -Recurse | ForEach-Object {
                    Copy-Item $_.FullName $GcnoDestDir -Force
                }
                Write-Info "Copied .gcno files to: $GcnoDestDir"
            }
        } else {
            Write-Warn "Failed to download .gcda files"
        }
    } else {
        Write-Warn "No .gcda files found on target (tests may not have coverage enabled)"
    }
}

# ==============================
# 7. 生成HTML覆盖率报告
# ==============================
if ($WithCoverage -and $SaveResults) {
    Write-Step "Generating coverage report..."

    # 检查是否有lcov工具
    $lcovPath = (Get-Command lcov -ErrorAction SilentlyContinue)
    $genhtmlPath = (Get-Command genhtml -ErrorAction SilentlyContinue)

    if (-not $lcovPath -or -not $genhtmlPath) {
        Write-Warn "lcov/genhtml not found in PATH, skipping coverage report generation"
        Write-Warn "Install lcov/genhtml on Windows or rerun without -WithCoverage"
    } else {
        $InfoFile = Join-Path $ResultSessionDir "coverage.info"
        $HtmlDir = Join-Path $CoverageDir "html"

        # 查找所有.gcno和.gcda文件
        $GcnoFiles = Get-ChildItem -Path "$ResultSessionDir" -Filter "*.gcno" -Recurse
        $GcdaFiles = Get-ChildItem -Path $GcdaDir -Filter "*.gcda" -ErrorAction SilentlyContinue

        if ($GcnoFiles.Count -gt 0 -and $GcdaFiles.Count -gt 0) {
            Write-Info "Generating coverage report locally (PowerShell)..."

            try {
                & lcov --capture --directory $ResultSessionDir `
                       --base-directory $ProjectRoot `
                       --output-file $InfoFile 2>&1

                if ($LASTEXITCODE -ne 0 -or -not (Test-Path $InfoFile)) {
                    throw "lcov capture failed"
                }

                & lcov --remove $InfoFile '/usr/*' '*/.deps/*' '*/googletest/*' `
                       --output-file $InfoFile 2>&1
                if ($LASTEXITCODE -ne 0) {
                    Write-Warn "lcov --remove returned non-zero, continuing with current coverage.info"
                }

                New-Item -ItemType Directory -Path $HtmlDir -Force | Out-Null
                & genhtml $InfoFile --output-directory $HtmlDir `
                          --title "MB_DDF_v2 Test Coverage" 2>&1

                if ($LASTEXITCODE -eq 0 -and (Test-Path "$HtmlDir\index.html")) {
                    Write-Info "HTML coverage report generated: $HtmlDir\index.html"
                } else {
                    Write-Warn "genhtml failed, HTML report was not generated"
                }
            } catch {
                Write-Warn "Failed to generate coverage report automatically"
                Write-Warn $_
            }
        } else {
            Write-Warn "No coverage files found (.gcno/.gcda)"
            Write-Warn "Ensure tests are built with --coverage flag"
        }
    }
}

# ==============================
# 8. 生成测试摘要（仅在保存结果时）
# ==============================
Write-Step "Test Summary"

if ($SaveResults) {
    $SummaryFile = Join-Path $ResultSessionDir "summary.txt"

    $SummaryContent = @"
MB_DDF_v2 Test Summary
===================
Date: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
Remote Host: $RemoteHost
Test Filter: $TestFilter
Test Binary: $TestBinaryName

Results:
- Exit Code: $testExitCode
- Status: $(if ($testExitCode -eq 0) { "PASSED" } else { "FAILED" })

Output Files:
- Test Output: $TestOutputFile
- Coverage Data: $GcdaDir
- HTML Report: $CoverageDir\html\

To view coverage report:
  Open: $CoverageDir\html\index.html
"@

    $SummaryContent | Out-File -FilePath $SummaryFile -Encoding UTF8

    # 解析测试结果
    $PassedTests = 0
    $FailedTests = 0
    $TotalTests = 0

    foreach ($line in $TestOutput) {
        if ($line -match "^\[  PASSED  \] (\d+) test") {
            $PassedTests = [int]$matches[1]
        }
        if ($line -match "^\[  FAILED  \] (\d+) test") {
            $FailedTests = [int]$matches[1]
        }
        if ($line -match "^\[==========\] (\d+) test") {
            $TotalTests = [int]$matches[1]
        }
    }

    Write-Info "Total Tests: $TotalTests"
    Write-Info "Passed: $PassedTests"
    Write-Info "Failed: $FailedTests"
    Write-Info "Summary saved to: $SummaryFile"

    # 创建latest符号链接/标记
    $LatestLink = Join-Path $ResultsDir "latest"
    if (Test-Path $LatestLink) {
        Remove-Item $LatestLink -Force
    }
    # 在Windows上创建 junction 或 just a text file with path
    $Timestamp | Out-File -FilePath "$LatestLink.txt" -Encoding UTF8

    Write-Info "Latest result: $ResultSessionDir"
}

# 解析测试结果（始终显示）
$PassedTests = 0
$FailedTests = 0
$TotalTests = 0

foreach ($line in $TestOutput) {
    if ($line -match "^\[  PASSED  \] (\d+) test") {
        $PassedTests = [int]$matches[1]
    }
    if ($line -match "^\[  FAILED  \] (\d+) test") {
        $FailedTests = [int]$matches[1]
    }
    if ($line -match "^\[==========\] (\d+) test") {
        $TotalTests = [int]$matches[1]
    }
}

if ($TotalTests -gt 0) {
    Write-Info "Total Tests: $TotalTests"
    Write-Info "Passed: $PassedTests"
    Write-Info "Failed: $FailedTests"
}

if ($testExitCode -eq 0) {
    Write-Info "All tests PASSED"
} else {
    Write-Error "Some tests FAILED (exit code: $testExitCode)"
}

exit $testExitCode
