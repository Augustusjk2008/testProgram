param(
    [Parameter(Position = 0)]
    [ValidateSet("clean", "debug", "release", "lib_debug", "lib_release", "dds_tests", "hw_debug", "hw_release", "hw_test_debug", "hw_test_release", "hw_tests", "help")]
    [string] $Action = "help",

    [string] $ProjectName = "MB_DDF_v2",

    [string] $Sysroot = "",

    [string] $ToolchainBin = "",

    [string] $MakePath = "",

    [string] $Arm64LibsPrefix = "H:\Resources\RTLinux\lib\arm64-libs",

    [int] $Jobs = $env:NUMBER_OF_PROCESSORS
)

$ErrorActionPreference = "Stop"

function Show-Help {
    Write-Host "Usage:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 clean"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 debug"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 release"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 lib_debug"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 lib_release"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 dds_tests"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_debug"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_release"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_test_debug"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_test_release"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build.ps1 hw_tests"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -ProjectName <name>     project/exe name (default: MB_DDF_v2)"
    Write-Host "  -Sysroot <path>         aarch64 sysroot (default: derived from compiler path)"
    Write-Host "  -ToolchainBin <path>    toolchain bin directory (default: from env or compiler path)"
    Write-Host "  -MakePath <path>        make.exe path (default: from env or PATH)"
    Write-Host "  -Arm64LibsPrefix <path> third-party libs prefix (default: H:\Resources\RTLinux\lib\arm64-libs)"
    Write-Host "  -Jobs <n>               parallel build jobs (default: 20)"
    Write-Host ""
    Write-Host "Environment variables (preferred):"
    Write-Host "  CROSS_CXX_COMPILER / CXX          compiler to derive origin sysroot"
    Write-Host "  TOOLCHAIN_BIN                     toolchain bin directory"
    Write-Host "  MAKE_PATH / CMAKE_MAKE_PROGRAM     make.exe path"
}

function Resolve-ProjectRoot {
    if ($PSScriptRoot -and $PSScriptRoot.Trim() -ne "") {
        return (Resolve-Path $PSScriptRoot).Path
    }
    if ($PSCommandPath -and $PSCommandPath.Trim() -ne "") {
        return (Resolve-Path (Split-Path -Parent $PSCommandPath)).Path
    }
    if ($MyInvocation.ScriptName -and $MyInvocation.ScriptName.Trim() -ne "") {
        return (Resolve-Path (Split-Path -Parent $MyInvocation.ScriptName)).Path
    }
    
    throw "Unable to resolve script directory. Please ensure you are running this script from a file."
}

function Get-DefaultArm64LibsPrefix([string] $toolchainBin, [string] $sysroot) {
    if ($Arm64LibsPrefix -and $Arm64LibsPrefix.Trim() -ne "") {
        return $Arm64LibsPrefix
    }
    $candidates = @()
    if ($toolchainBin -and $toolchainBin.Trim() -ne "") {
        $p1 = Split-Path -Parent $toolchainBin
        $p2 = Split-Path -Parent $p1
        $p3 = Split-Path -Parent $p2
        $candidates += (Join-Path $p3 "libs")
    }
    if ($sysroot -and $sysroot.Trim() -ne "") {
        $s1 = Split-Path -Parent $sysroot
        $s2 = Split-Path -Parent $s1
        $s3 = Split-Path -Parent $s2
        $candidates += (Join-Path $s3 "libs")
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) {
            return $c
        }
    }
    if ($candidates.Count -gt 0) {
        return $candidates[0]
    }
    return ""
}

function Resolve-PathFromEnv([string[]] $varNames) {
    foreach ($name in $varNames) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value -and $value.Trim() -ne "") {
            return $value
        }
    }
    return ""
}

function Get-CommandResolvedPath($cmd) {
    if ($null -eq $cmd) {
        return ""
    }
    if ($cmd.Path -and $cmd.Path.Trim() -ne "") {
        return $cmd.Path
    }
    if ($cmd.Source -and $cmd.Source.Trim() -ne "") {
        return $cmd.Source
    }
    if ($cmd.Definition -and $cmd.Definition.Trim() -ne "") {
        return $cmd.Definition
    }
    return ""
}

function Resolve-CompilerPath {
    $candidate = Resolve-PathFromEnv -varNames @("CROSS_CXX_COMPILER", "CXX", "AARCH64_CXX")
    if (-not $candidate -or $candidate.Trim() -eq "") {
        $candidate = "aarch64-none-linux-gnu-g++"
    }

    if (Test-Path $candidate) {
        return (Resolve-Path $candidate).Path
    }

    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($null -ne $cmd) {
        $resolved = Get-CommandResolvedPath $cmd
        if ($resolved -and $resolved.Trim() -ne "") {
            return $resolved
        }
    }

    throw "Compiler not found: $candidate"
}

function Resolve-ToolchainBinDir {
    if ($ToolchainBin -and $ToolchainBin.Trim() -ne "") {
        return $ToolchainBin
    }

    $fromEnv = Resolve-PathFromEnv -varNames @("TOOLCHAIN_BIN", "ARM_GNU_TOOLCHAIN_BIN", "AARCH64_TOOLCHAIN_BIN")
    if ($fromEnv -and $fromEnv.Trim() -ne "") {
        return $fromEnv
    }

    $compilerPath = Resolve-CompilerPath
    return (Split-Path -Parent $compilerPath)
}

function Resolve-OriginSysroot {
    if ($Sysroot -and $Sysroot.Trim() -ne "") {
        return $Sysroot
    }

    $compilerPath = Resolve-CompilerPath
    $binDir = Split-Path -Parent $compilerPath
    
    $installRoot1 = Split-Path -Parent $binDir
    $originSysroot1 = Join-Path $installRoot1 "origin\armv8a-ucas-linux"
    if (Test-Path $originSysroot1) {
        return $originSysroot1
    }

    $installRoot2 = Split-Path -Parent $installRoot1
    $originSysroot2 = Join-Path $installRoot2 "origin\armv8a-ucas-linux"
    if (Test-Path $originSysroot2) {
        return $originSysroot2
    }

    throw "Derived sysroot not found. Checked:`n$originSysroot1`n$originSysroot2"
}

function Resolve-MakePath {
    if ($MakePath -and $MakePath.Trim() -ne "") {
        return $MakePath
    }

    $fromEnv = Resolve-PathFromEnv -varNames @("MAKE_PATH", "CMAKE_MAKE_PROGRAM", "MAKE")
    if ($fromEnv -and $fromEnv.Trim() -ne "" -and (Test-Path $fromEnv)) {
        return $fromEnv
    }

    $makeCmd = Get-Command "make.exe" -ErrorAction SilentlyContinue
    if ($null -ne $makeCmd) {
        $resolved = Get-CommandResolvedPath $makeCmd
        if ($resolved -and $resolved.Trim() -ne "") {
            return $resolved
        }
    }

    $mingwMakeCmd = Get-Command "mingw32-make.exe" -ErrorAction SilentlyContinue
    if ($null -ne $mingwMakeCmd) {
        $resolved = Get-CommandResolvedPath $mingwMakeCmd
        if ($resolved -and $resolved.Trim() -ne "") {
            return $resolved
        }
    }

    throw "make.exe not found. Provide -MakePath or set MAKE_PATH. Also tried mingw32-make.exe."
}

function Ensure-Tool([string] $toolName, [string] $toolchainBin) {
    $toolPath = Join-Path $toolchainBin $toolName
    if (Test-Path $toolPath) {
        return $toolPath
    }
    $cmd = Get-Command $toolName -ErrorAction SilentlyContinue
    if ($null -ne $cmd) {
        $resolved = Get-CommandResolvedPath $cmd
        if ($resolved -and $resolved.Trim() -ne "") {
            return $resolved
        }
    }
    throw "Tool not found: $toolName. Searched: $toolPath and PATH."
}

function Ensure-ToolAvailable([string] $toolName) {
    $cmd = Get-Command $toolName -ErrorAction SilentlyContinue
    if ($null -ne $cmd) {
        return
    }
    throw "Tool not found in PATH: $toolName"
}

function Add-PathIfExists([string] $path) {
    if ($path -and $path.Trim() -ne "" -and (Test-Path $path)) {
        $env:PATH = ($path + ";" + $env:PATH)
    }
}

function Format-ExitCode([int64] $exitCode) {
    $code = [int64] $exitCode
    if ($code -lt 0) {
        $code += 4294967296
    }
    return ("0x{0:X8}" -f $code)
}

function Ensure-NativeToolRuns([string] $toolPath, [string] $toolName) {
    if (-not $toolPath -or $toolPath.Trim() -eq "" -or -not (Test-Path $toolPath)) {
        throw "$toolName not found: $toolPath"
    }

    & $toolPath --version > $null 2>&1
    if ($LASTEXITCODE -ne 0) {
        $exitCodeHex = Format-ExitCode -exitCode $LASTEXITCODE
        throw "$toolName failed to run with exit code $exitCodeHex. On Windows this usually means a missing runtime DLL. Ensure its bin directory is in PATH and install the required Windows/VC++ runtime."
    }
}

function Clean-Build([string] $projectRoot) {
    $buildRoot = Join-Path $projectRoot "build"
    if (Test-Path $buildRoot) {
        Remove-Item -Recurse -Force $buildRoot
    }
    $gdbUploaded = Join-Path $projectRoot ".vscode\.gdbserver_uploaded"
    if (Test-Path $gdbUploaded) {
        Write-Host "Cleaning .vscode\.gdbserver_uploaded..."
        Remove-Item -Force $gdbUploaded
    }
}

function Configure-And-Build(
    [string] $projectRoot,
    [ValidateSet("Debug", "Release")] [string] $config,
    [ValidateSet("echo", "hw_test", "demo", "tests")] [string] $buildProfile,
    [ValidateSet("ECHO", "HW_TEST", "DEMO")] [string] $appMode,
    [ValidateSet("ON", "OFF")] [string] $buildHardwareLayer,
    [ValidateSet("ON", "OFF")] [string] $buildHwDdsAdapter,
    [ValidateSet("ON", "OFF")] [string] $enableTests,
    [ValidateSet("ON", "OFF")] [string] $enableHwUnitTests,
    [ValidateSet("ON", "OFF")] [string] $enableHwSmokeTests
) {
    $resolvedToolchainBin = Resolve-ToolchainBinDir
    $resolvedSysroot = Resolve-OriginSysroot
    $resolvedMakePath = Resolve-MakePath
    if (-not (Test-Path $resolvedSysroot)) {
        throw "Sysroot not found: $resolvedSysroot"
    }

    $arm64Libs = Get-DefaultArm64LibsPrefix -toolchainBin $resolvedToolchainBin -sysroot $resolvedSysroot
    $env:ARM64_LIBS_PREFIX = $arm64Libs

    Add-PathIfExists -path (Split-Path -Parent $resolvedMakePath)
    Add-PathIfExists -path $resolvedToolchainBin
    Ensure-NativeToolRuns -toolPath $resolvedMakePath -toolName "make"

    $gcc = "aarch64-none-linux-gnu-gcc"
    $gpp = "aarch64-none-linux-gnu-g++"
    Ensure-ToolAvailable -toolName $gcc
    Ensure-ToolAvailable -toolName $gpp
    $gccCmd = Get-Command $gcc -ErrorAction SilentlyContinue
    $gppCmd = Get-Command $gpp -ErrorAction SilentlyContinue
    $gccPathResolved = Get-CommandResolvedPath $gccCmd
    $gppPathResolved = Get-CommandResolvedPath $gppCmd

    $generator = "Unix Makefiles"
    $makeForCmake = $resolvedMakePath

    $buildDir = Join-Path $projectRoot (Join-Path (Join-Path "build\aarch64" $buildProfile) $config)
    $compileDbPath = Join-Path $buildDir "compile_commands.json"

    # CMake still accepts these legacy environment variables. Keep them aligned
    # with the explicit cache arguments so a stale caller environment cannot
    # select a different image inside this isolated build directory.
    $env:BUILD_HARDWARE_LAYER = $buildHardwareLayer
    $env:BUILD_HW_DDS_ADAPTER = $buildHwDdsAdapter
    $env:ENABLE_TESTS = $enableTests
    $env:ENABLE_HW_UNIT_TESTS = $enableHwUnitTests
    $env:ENABLE_HW_SMOKE_TESTS = $enableHwSmokeTests

    Write-Host "==== Configure-And-Build Diagnostics ===="
    Write-Host ("ProjectRoot: " + $projectRoot)
    Write-Host ("BuildConfig: " + $config)
    Write-Host ("BuildProfile: " + $buildProfile)
    Write-Host ("AppMode: " + $appMode)
    Write-Host ("ProjectName: " + $ProjectName)
    Write-Host ("ToolchainBin: " + $resolvedToolchainBin)
    Write-Host ("Sysroot: " + $resolvedSysroot)
    Write-Host ("MakeProgram: " + $resolvedMakePath)
    Write-Host ("Generator: " + $generator)
    Write-Host ("ARM64_LIBS_PREFIX: " + $arm64Libs)
    Write-Host ("GCC Path: " + $gccPathResolved)
    Write-Host ("G++ Path: " + $gppPathResolved)
    Write-Host ("BuildDir: " + $buildDir)
    Write-Host ("CompileCommands: " + $compileDbPath)

    $cmakeArgs = @(
        "-S", $projectRoot,
        "-B", $buildDir,
        "-G", $generator,
        "-DCMAKE_MAKE_PROGRAM=$makeForCmake",
        "-DPROJECT_NAME_OVERRIDE=$ProjectName",
        "-DCMAKE_BUILD_TYPE=$config",
        "-DCMAKE_SYSTEM_NAME=Linux",
        "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
        "-DCMAKE_SYSROOT=$resolvedSysroot",
        "-DCMAKE_C_COMPILER=$gcc",
        "-DCMAKE_CXX_COMPILER=$gpp",
        "-DCROSS_COMPILE=ON",
        "-DCROSS_SYSROOT=$resolvedSysroot",
        "-DCROSS_C_COMPILER=$gcc",
        "-DCROSS_CXX_COMPILER=$gpp",
        "-DBUILD_LIBRARY=OFF",
        "-DMB_DDF_APP_MODE=$appMode",
        "-DBUILD_HARDWARE_LAYER=$buildHardwareLayer",
        "-DBUILD_HW_DDS_ADAPTER=$buildHwDdsAdapter",
        "-DENABLE_TESTS=$enableTests",
        "-DENABLE_HW_UNIT_TESTS=$enableHwUnitTests",
        "-DENABLE_HW_SMOKE_TESTS=$enableHwSmokeTests"
    )
    $cmakeArgsString = ($cmakeArgs -join " ")
    Write-Host ("CMake Configure Args: " + $cmakeArgsString)

    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
    cmake --build $buildDir -j $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }

    $vsCodeDir = Join-Path $projectRoot ".vscode"
    if (-not (Test-Path $vsCodeDir)) {
        New-Item -ItemType Directory -Path $vsCodeDir -Force | Out-Null
    }

    $compileDbSourcePath = $compileDbPath
    if (-not (Test-Path $compileDbSourcePath)) {
        $foundCompileDb = Get-ChildItem -Path $buildDir -Recurse -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer -and $_.Name -ieq "compile_commands.json" } |
            Select-Object -First 1
        if ($foundCompileDb) {
            $compileDbSourcePath = $foundCompileDb.FullName
        }
    }

    if (Test-Path $compileDbSourcePath) {
        $compileDbDestPath = Join-Path $vsCodeDir "compile_commands.json"
        Copy-Item -Path $compileDbSourcePath -Destination $compileDbDestPath -Force
        Write-Host "Copied compile_commands.json to: $compileDbDestPath"
    }
    else {
        Write-Host "compile_commands.json not found under build directory: $buildDir"
    }

    $targetBaseName = $ProjectName
    $candidates = @(
        (Join-Path $buildDir $targetBaseName),
        (Join-Path $buildDir ($targetBaseName + ".exe")),
        (Join-Path (Join-Path $buildDir "bin") $targetBaseName),
        (Join-Path (Join-Path $buildDir "bin") ($targetBaseName + ".exe")),
        (Join-Path (Join-Path $buildDir $config) $targetBaseName),
        (Join-Path (Join-Path $buildDir $config) ($targetBaseName + ".exe"))
    )

    $exePath = ""
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            $exePath = $candidate
            break
        }
    }

    if (-not $exePath -or $exePath.Trim() -eq "") {
        $found = Get-ChildItem -Path $buildDir -Recurse -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer -and ($_.Name -eq $targetBaseName -or $_.Name -eq ($targetBaseName + ".exe")) } |
            Select-Object -First 1
        if ($found) {
            $exePath = $found.FullName
        }
    }

    if (-not $exePath -or $exePath.Trim() -eq "" -or -not (Test-Path $exePath)) {
        $checkedList = ($candidates -join "`n")
        throw "Build completed but executable not found. Checked:`n$checkedList`nSearched under: $buildDir"
    }
    Write-Host "Built: $exePath"
}

function Configure-And-Build-Library(
    [string] $projectRoot,
    [ValidateSet("Debug", "Release")] [string] $config,
    [ValidateSet("lib", "tests")] [string] $buildProfile = "lib",
    [ValidateSet("ON", "OFF")] [string] $enableTests = "OFF"
) {
    $resolvedToolchainBin = Resolve-ToolchainBinDir
    $resolvedSysroot = Resolve-OriginSysroot
    $resolvedMakePath = Resolve-MakePath
    if (-not (Test-Path $resolvedSysroot)) {
        throw "Sysroot not found: $resolvedSysroot"
    }

    $arm64Libs = Get-DefaultArm64LibsPrefix -toolchainBin $resolvedToolchainBin -sysroot $resolvedSysroot
    $env:ARM64_LIBS_PREFIX = $arm64Libs

    Add-PathIfExists -path (Split-Path -Parent $resolvedMakePath)
    Add-PathIfExists -path $resolvedToolchainBin
    Ensure-NativeToolRuns -toolPath $resolvedMakePath -toolName "make"

    $gcc = "aarch64-none-linux-gnu-gcc"
    $gpp = "aarch64-none-linux-gnu-g++"
    Ensure-ToolAvailable -toolName $gcc
    Ensure-ToolAvailable -toolName $gpp
    $gccCmd = Get-Command $gcc -ErrorAction SilentlyContinue
    $gppCmd = Get-Command $gpp -ErrorAction SilentlyContinue
    $gccPathResolved = Get-CommandResolvedPath $gccCmd
    $gppPathResolved = Get-CommandResolvedPath $gppCmd

    $generator = "Unix Makefiles"
    $makeForCmake = $resolvedMakePath

    $buildDir = Join-Path $projectRoot (Join-Path (Join-Path "build\aarch64" $buildProfile) $config)
    $compileDbPath = Join-Path $buildDir "compile_commands.json"

    $env:BUILD_HARDWARE_LAYER = "OFF"
    $env:BUILD_HW_DDS_ADAPTER = "OFF"
    $env:ENABLE_TESTS = $enableTests
    $env:ENABLE_HW_UNIT_TESTS = "OFF"
    $env:ENABLE_HW_SMOKE_TESTS = "OFF"

    Write-Host "==== Configure-And-Build-Library Diagnostics ===="
    Write-Host ("ProjectRoot: " + $projectRoot)
    Write-Host ("BuildConfig: " + $config)
    Write-Host ("BuildProfile: " + $buildProfile)
    Write-Host ("ProjectName: " + $ProjectName)
    Write-Host ("ToolchainBin: " + $resolvedToolchainBin)
    Write-Host ("Sysroot: " + $resolvedSysroot)
    Write-Host ("MakeProgram: " + $resolvedMakePath)
    Write-Host ("Generator: " + $generator)
    Write-Host ("ARM64_LIBS_PREFIX: " + $arm64Libs)
    Write-Host ("GCC Path: " + $gccPathResolved)
    Write-Host ("G++ Path: " + $gppPathResolved)
    Write-Host ("BuildDir: " + $buildDir)
    Write-Host ("CompileCommands: " + $compileDbPath)

    $cmakeArgs = @(
        "-S", $projectRoot,
        "-B", $buildDir,
        "-G", $generator,
        "-DCMAKE_MAKE_PROGRAM=$makeForCmake",
        "-DPROJECT_NAME_OVERRIDE=$ProjectName",
        "-DCMAKE_BUILD_TYPE=$config",
        "-DCMAKE_SYSTEM_NAME=Linux",
        "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
        "-DCMAKE_SYSROOT=$resolvedSysroot",
        "-DCMAKE_C_COMPILER=$gcc",
        "-DCMAKE_CXX_COMPILER=$gpp",
        "-DCROSS_COMPILE=ON",
        "-DCROSS_SYSROOT=$resolvedSysroot",
        "-DCROSS_C_COMPILER=$gcc",
        "-DCROSS_CXX_COMPILER=$gpp",
        "-DBUILD_LIBRARY=ON",
        "-DMB_DDF_APP_MODE=ECHO",
        "-DBUILD_HARDWARE_LAYER=OFF",
        "-DBUILD_HW_DDS_ADAPTER=OFF",
        "-DENABLE_TESTS=$enableTests",
        "-DENABLE_HW_UNIT_TESTS=OFF",
        "-DENABLE_HW_SMOKE_TESTS=OFF"
    )
    $cmakeArgsString = ($cmakeArgs -join " ")
    Write-Host ("CMake Configure Args: " + $cmakeArgsString)

    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
    cmake --build $buildDir -j $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }

    $vsCodeDir = Join-Path $projectRoot ".vscode"
    if (-not (Test-Path $vsCodeDir)) {
        New-Item -ItemType Directory -Path $vsCodeDir -Force | Out-Null
    }

    $compileDbSourcePath = $compileDbPath
    if (-not (Test-Path $compileDbSourcePath)) {
        $foundCompileDb = Get-ChildItem -Path $buildDir -Recurse -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer -and $_.Name -ieq "compile_commands.json" } |
            Select-Object -First 1
        if ($foundCompileDb) {
            $compileDbSourcePath = $foundCompileDb.FullName
        }
    }

    if (Test-Path $compileDbSourcePath) {
        $compileDbDestPath = Join-Path $vsCodeDir "compile_commands.json"
        Copy-Item -Path $compileDbSourcePath -Destination $compileDbDestPath -Force
        Write-Host "Copied compile_commands.json to: $compileDbDestPath"
    }
    else {
        Write-Host "compile_commands.json not found under build directory: $buildDir"
    }

    if ($buildProfile -eq "tests") {
        Write-Host "DDS-only test build completed: $buildDir"
        return
    }

    # Find and display library files
    $libCandidates = @(
        (Join-Path $buildDir ("lib" + $ProjectName + ".a")),
        (Join-Path $buildDir ("lib" + $ProjectName + ".so")),
        (Join-Path (Join-Path $buildDir "lib") ("lib" + $ProjectName + ".a")),
        (Join-Path (Join-Path $buildDir "lib") ("lib" + $ProjectName + ".so")),
        (Join-Path (Join-Path $buildDir $config) ("lib" + $ProjectName + ".a")),
        (Join-Path (Join-Path $buildDir $config) ("lib" + $ProjectName + ".so"))
    )

    $foundLibs = @()
    foreach ($candidate in $libCandidates) {
        if ($candidate -and (Test-Path $candidate)) {
            $foundLibs += $candidate
        }
    }

    if ($foundLibs.Count -eq 0) {
        $found = Get-ChildItem -Path $buildDir -Recurse -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer -and ($_.Name -like "*.a" -or $_.Name -like "*.so") } |
            Select-Object -First 10
        if ($found) {
            foreach ($lib in $found) {
                $foundLibs += $lib.FullName
            }
        }
    }

    if ($foundLibs.Count -gt 0) {
        Write-Host ""
        Write-Host "Library files built:"
        foreach ($lib in $foundLibs) {
            Write-Host "  $lib"
        }
    }

    # Create install directories and copy files
    $installDir = Join-Path $projectRoot "install"
    $installLibsDir = Join-Path $installDir "libs"
    $installIncludeDir = Join-Path $installDir "include"

    # Create directories if they don't exist
    if (-not (Test-Path $installLibsDir)) {
        New-Item -ItemType Directory -Path $installLibsDir -Force | Out-Null
    }
    if (-not (Test-Path $installIncludeDir)) {
        New-Item -ItemType Directory -Path $installIncludeDir -Force | Out-Null
    }

    # Copy library files to install/libs
    foreach ($lib in $foundLibs) {
        $libName = Split-Path -Leaf $lib
        $destPath = Join-Path $installLibsDir $libName
        Copy-Item -Path $lib -Destination $destPath -Force
        Write-Host "Installed: $libName -> $installLibsDir"
    }

    # Copy header files from src to install/include
    $sourceHeadersDir = Join-Path $projectRoot "src"
    if (Test-Path $sourceHeadersDir) {
        $headerFiles = Get-ChildItem -Path $sourceHeadersDir -Recurse -Filter "*.h" -ErrorAction SilentlyContinue
        foreach ($header in $headerFiles) {
            # Calculate relative path from src
            $relativePath = $header.FullName.Substring($sourceHeadersDir.Length + 1)
            $destPath = Join-Path $installIncludeDir $relativePath
            $destDir = Split-Path -Parent $destPath
            if (-not (Test-Path $destDir)) {
                New-Item -ItemType Directory -Path $destDir -Force | Out-Null
            }
            Copy-Item -Path $header.FullName -Destination $destPath -Force
        }
        Write-Host "Installed: $($headerFiles.Count) header files -> $installIncludeDir"
    }

    Write-Host ""
    Write-Host "Install paths:"
    Write-Host "  Libraries: $installLibsDir"
    Write-Host "  Headers:   $installIncludeDir"

    # Display installed files
    if (Test-Path $installLibsDir) {
        $installedLibs = @(Get-ChildItem -Path $installLibsDir -Filter "*.a" -ErrorAction SilentlyContinue)
        $installedSos = @(Get-ChildItem -Path $installLibsDir -Filter "*.so" -ErrorAction SilentlyContinue)
        $allLibs = $installedLibs + $installedSos
        if ($allLibs.Count -gt 0) {
            Write-Host ""
            Write-Host "Installed libraries:"
            foreach ($lib in $allLibs) {
                Write-Host "  $($lib.Name)"
            }
        }
    }
}

$projectRoot = Resolve-ProjectRoot

switch ($Action) {
    "help" {
        Show-Help
        exit 0
    }
    "clean" {
        Clean-Build -projectRoot $projectRoot
        Write-Host "Cleaned build outputs."
        exit 0
    }
    "debug" {
        Configure-And-Build -projectRoot $projectRoot -config "Debug" -buildProfile "echo" -appMode "ECHO" -buildHardwareLayer "ON" -buildHwDdsAdapter "OFF" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "release" {
        Configure-And-Build -projectRoot $projectRoot -config "Release" -buildProfile "echo" -appMode "ECHO" -buildHardwareLayer "ON" -buildHwDdsAdapter "OFF" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "lib_debug" {
        Configure-And-Build-Library -projectRoot $projectRoot -config "Debug" -buildProfile "lib" -enableTests "OFF"
        exit 0
    }
    "lib_release" {
        Configure-And-Build-Library -projectRoot $projectRoot -config "Release" -buildProfile "lib" -enableTests "OFF"
        exit 0
    }
    "dds_tests" {
        Configure-And-Build-Library -projectRoot $projectRoot -config "Debug" -buildProfile "tests" -enableTests "ON"
        exit 0
    }
    "hw_debug" {
        Configure-And-Build -projectRoot $projectRoot -config "Debug" -buildProfile "demo" -appMode "DEMO" -buildHardwareLayer "ON" -buildHwDdsAdapter "ON" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "hw_release" {
        Configure-And-Build -projectRoot $projectRoot -config "Release" -buildProfile "demo" -appMode "DEMO" -buildHardwareLayer "ON" -buildHwDdsAdapter "ON" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "hw_test_debug" {
        Configure-And-Build -projectRoot $projectRoot -config "Debug" -buildProfile "hw_test" -appMode "HW_TEST" -buildHardwareLayer "ON" -buildHwDdsAdapter "OFF" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "hw_test_release" {
        Configure-And-Build -projectRoot $projectRoot -config "Release" -buildProfile "hw_test" -appMode "HW_TEST" -buildHardwareLayer "ON" -buildHwDdsAdapter "OFF" -enableTests "OFF" -enableHwUnitTests "OFF" -enableHwSmokeTests "OFF"
        exit 0
    }
    "hw_tests" {
        Configure-And-Build -projectRoot $projectRoot -config "Debug" -buildProfile "tests" -appMode "DEMO" -buildHardwareLayer "ON" -buildHwDdsAdapter "ON" -enableTests "ON" -enableHwUnitTests "ON" -enableHwSmokeTests "ON"
        exit 0
    }
}
