[CmdletBinding(PositionalBinding = $false)]
param(
    [string] $RemoteHost = "192.168.1.29",
    [string] $RemoteUser = "root",
    [string] $RemoteDir = "/home/sast8/tmp",
    [int] $RemoteGdbPort = 1234,
    [string] $Password = "",
    [switch] $ForegroundGdbserver,
    [switch] $Run,
    [switch] $SourceDebug,
    [switch] $FullHardware,
    [switch] $Com3Echo,
    [switch] $HardwareTest,
    [string] $SpiFlashTestAddress = "",
    [switch] $DryRun
)

$ErrorActionPreference = "Stop"

if ($FullHardware -and -not $Run) {
    throw "-FullHardware requires -Run."
}
if ($Com3Echo -and -not $Run) {
    throw "-Com3Echo requires -Run."
}
if ($HardwareTest -and -not $Run) {
    throw "-HardwareTest requires -Run."
}
if (($SourceDebug -and ($Com3Echo -or $HardwareTest -or $FullHardware)) -or
    ($Com3Echo -and $HardwareTest) -or
    ($Com3Echo -and $FullHardware) -or
    ($HardwareTest -and $FullHardware)) {
    throw "-SourceDebug, -Com3Echo, -HardwareTest, and -FullHardware are mutually exclusive."
}
if ($SpiFlashTestAddress -and -not $FullHardware) {
    throw "-SpiFlashTestAddress requires -Run -FullHardware."
}
if ($SpiFlashTestAddress -and
    $SpiFlashTestAddress -notmatch '^(0[xX][0-9a-fA-F]+|[0-9]+)$') {
    throw "-SpiFlashTestAddress must be a decimal or 0x-prefixed hexadecimal address."
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
    throw "Unable to resolve script directory."
}

function Get-PlaintextPassword([string] $p) {
    if ($p -and $p.Trim() -ne "") {
        return $p
    }
    $secure = Read-Host "Remote password" -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    }
    finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

function Get-CommandPath([string] $name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        return ""
    }

    $p = $cmd.PSObject.Properties["Source"]
    if ($null -ne $p -and $cmd.Source -and $cmd.Source.Trim() -ne "") {
        return $cmd.Source
    }

    $p = $cmd.PSObject.Properties["Path"]
    if ($null -ne $p -and $cmd.Path -and $cmd.Path.Trim() -ne "") {
        return $cmd.Path
    }

    $p = $cmd.PSObject.Properties["Definition"]
    if ($null -ne $p -and $cmd.Definition -and $cmd.Definition.Trim() -ne "") {
        return $cmd.Definition
    }
    return ""
}

function Update-LaunchJsonMiDebuggerPath([string] $projectRoot, [string] $buildDir) {
    $vsCodeDir = Join-Path $projectRoot ".vscode"
    $cc1 = Join-Path $vsCodeDir "compile_commands.json"
    $cc2 = Join-Path $buildDir "compile_commands.json"
    $ccPath = ""
    if (Test-Path $cc1) { $ccPath = $cc1 }
    elseif (Test-Path $cc2) { $ccPath = $cc2 }
    else {
        Write-Host "miDebuggerPath: compile_commands.json not found."
        return
    }

    $ccLines = Get-Content -LiteralPath $ccPath
    $ccText = ($ccLines -join "`n")
    $m = [Regex]::Match($ccText, '([A-Za-z]:\\\\[^"]*?\\\\aarch64-none-linux-gnu-g(?:cc|\\+\\+)\\.exe)', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    $gdbPath = ""
    if ($m.Success) {
        $compilerPathEscaped = $m.Groups[1].Value
        $compilerPath = ($compilerPathEscaped -replace '\\\\', '\')
        $gdbPath = ($compilerPath -replace 'aarch64-none-linux-gnu-g(?:cc|\+\+)\.exe', 'aarch64-none-linux-gnu-gdb.exe')
    }
    else {
        $cmdGdb = Get-Command "aarch64-none-linux-gnu-gdb.exe" -ErrorAction SilentlyContinue
        if ($null -ne $cmdGdb -and $cmdGdb.Path) {
            $gdbPath = $cmdGdb.Path
        }
        else {
            $cmdGcc = Get-Command "aarch64-none-linux-gnu-gcc.exe" -ErrorAction SilentlyContinue
            $cmdGpp = $null
            if ($null -eq $cmdGcc -or -not $cmdGcc.Path) {
            $cmdGpp = Get-Command "aarch64-none-linux-gnu-g++.exe" -ErrorAction SilentlyContinue
            }
            $compilerResolved = ""
            if ($null -ne $cmdGcc -and $cmdGcc.Path) { $compilerResolved = $cmdGcc.Path }
            elseif ($null -ne $cmdGpp -and $cmdGpp.Path) { $compilerResolved = $cmdGpp.Path }
            if ($compilerResolved -and $compilerResolved.Trim() -ne "") {
                $gdbPath = ($compilerResolved -replace 'aarch64-none-linux-gnu-g(?:cc|\+\+)\.exe', 'aarch64-none-linux-gnu-gdb.exe')
            }
            else {
                $toolchainBinEnv = [Environment]::GetEnvironmentVariable("TOOLCHAIN_BIN")
                if ($toolchainBinEnv -and (Test-Path $toolchainBinEnv)) {
                    $candidate = Join-Path $toolchainBinEnv "aarch64-none-linux-gnu-gdb.exe"
                    if (Test-Path $candidate) {
                        $gdbPath = $candidate
                    }
                }
            }
        }
    }
    if (-not $gdbPath -or $gdbPath.Trim() -eq "") {
        Write-Host "miDebuggerPath: unable to resolve gdb path; skip updating launch.json."
        return
    }
    if (-not (Test-Path $gdbPath)) {
        Write-Host ("miDebuggerPath: gdb not found at " + $gdbPath + " (still updating launch.json)")
    }

    $launchPath = Join-Path $vsCodeDir "launch.json"
    if (-not (Test-Path $launchPath)) {
        Write-Host "miDebuggerPath: .vscode/launch.json not found; skip updating."
        return
    }
    
    # 修复：使用 UTF8 编码读取文件
    $launchText = [System.IO.File]::ReadAllText($launchPath, [System.Text.Encoding]::UTF8)

    $gdbPathJson = ($gdbPath -replace '\\', '\\\\')
    $pattern = '"miDebuggerPath"\s*:\s*"[^"]*"'
    if ([Regex]::IsMatch($launchText, $pattern)) {
        $updated = [Regex]::Replace($launchText, $pattern, ('"miDebuggerPath": "' + $gdbPathJson + '"'))
        # 修复：使用 UTF8 编码写入文件
        [System.IO.File]::WriteAllText($launchPath, $updated, [System.Text.Encoding]::UTF8)
        Write-Host ("Updated .vscode/launch.json miDebuggerPath => " + $gdbPath)
    }
    else {
        Write-Host "miDebuggerPath: key not found in launch.json; no change made."
    }
}

function Update-LaunchJsonSysroot([string] $projectRoot, [string] $buildDir) {
    $vsCodeDir = Join-Path $projectRoot ".vscode"
    $cc1 = Join-Path $vsCodeDir "compile_commands.json"
    $cc2 = Join-Path $buildDir "compile_commands.json"
    $ccPath = ""
    if (Test-Path $cc1) { $ccPath = $cc1 }
    elseif (Test-Path $cc2) { $ccPath = $cc2 }
    else {
        Write-Host "sysroot: compile_commands.json not found."
        return
    }

    $ccLines = Get-Content -LiteralPath $ccPath
    $ccText = ($ccLines -join "`n")
    $sysroot = ""
    $m1 = [Regex]::Match($ccText, '--sysroot\s*=\s*"([^"]+)"', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($m1.Success) {
        $sysroot = $m1.Groups[1].Value
    }
    else {
        $m2 = [Regex]::Match($ccText, '--sysroot\s*=\s*([^\s"]+)', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($m2.Success) {
            $sysroot = $m2.Groups[1].Value
        }
        else {
            $m3 = [Regex]::Match($ccText, '-isysroot\s+"([^"]+)"', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if ($m3.Success) {
                $sysroot = $m3.Groups[1].Value
            }
            else {
                $m4 = [Regex]::Match($ccText, '-isysroot\s+([^\s"]+)', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
                if ($m4.Success) {
                    $sysroot = $m4.Groups[1].Value
                }
            }
        }
    }
    if (-not $sysroot -or $sysroot.Trim() -eq "") {
        Write-Host "sysroot: unable to parse from compile_commands.json; skip updating."
        return
    }
    $sysrootUnescaped = ($sysroot -replace '\\\\', '\')

    $launchPath = Join-Path $vsCodeDir "launch.json"
    if (-not (Test-Path $launchPath)) {
        Write-Host "sysroot: .vscode/launch.json not found; skip updating."
        return
    }
    
    # 修复：使用 UTF8 编码读取文件
    $launchText = [System.IO.File]::ReadAllText($launchPath, [System.Text.Encoding]::UTF8)

    $sysrootJson = ($sysrootUnescaped -replace '\\', '\\\\')
    $pattern = '"text"\s*:\s*"set sysroot [^"]*"'
    if ([Regex]::IsMatch($launchText, $pattern)) {
        $updated = [Regex]::Replace($launchText, $pattern, ('"text": "set sysroot ' + $sysrootJson + '"'))
        # 修复：使用 UTF8 编码写入文件
        [System.IO.File]::WriteAllText($launchPath, $updated, [System.Text.Encoding]::UTF8)
        
        Write-Host ("Updated .vscode/launch.json sysroot => " + $sysrootUnescaped)
    }
    else {
        Write-Host "sysroot: set sysroot entry not found in launch.json; no change made."
    }
}

function Update-LaunchJsonProgram(
    [string] $projectRoot,
    [string] $projectName,
    [string] $buildProfile,
    [string] $buildConfig
) {
    $vsCodeDir = Join-Path $projectRoot ".vscode"
    $launchPath = Join-Path $vsCodeDir "launch.json"
    if (-not (Test-Path $launchPath)) {
        Write-Host "program: .vscode/launch.json not found; skip updating."
        return
    }
    
    $launchText = [System.IO.File]::ReadAllText($launchPath, [System.Text.Encoding]::UTF8)

    $programPath = '${workspaceFolder}/build/aarch64/' + $buildProfile + '/' +
        $buildConfig + '/' + $projectName
    $pattern = '"program"\s*:\s*"[^"]*"'
    
    if ([Regex]::IsMatch($launchText, $pattern)) {
        $updated = [Regex]::Replace($launchText, $pattern, ('"program": "' + $programPath + '"'))
        [System.IO.File]::WriteAllText($launchPath, $updated, [System.Text.Encoding]::UTF8)
        Write-Host ("Updated .vscode/launch.json program => " + $programPath)
    }
    else {
        Write-Host "program: key not found in launch.json; no change made."
    }
}

function Invoke-WithSshAskPass([string] $passwordPlain, [scriptblock] $Action) {
    if ($DryRun) {
        & $Action
        return
    }
    if (-not $passwordPlain -or $passwordPlain.Trim() -eq "") {
        & $Action
        return
    }

    $tempDir = $env:TEMP
    if (-not $tempDir -or $tempDir.Trim() -eq "") {
        $tempDir = [System.IO.Path]::GetTempPath()
    }
    $askpassPath = Join-Path $tempDir ("ssh_askpass_" + ([Guid]::NewGuid().ToString("N")) + ".cmd")

    $prevAskPass = $env:SSH_ASKPASS
    $prevAskPassRequire = $env:SSH_ASKPASS_REQUIRE
    $prevDisplay = $env:DISPLAY

    try {
        Set-Content -LiteralPath $askpassPath -Encoding ASCII -Value ("@echo off`r`necho " + $passwordPlain + "`r`n")
        $env:SSH_ASKPASS = $askpassPath
        $env:SSH_ASKPASS_REQUIRE = "force"
        if (-not $env:DISPLAY -or $env:DISPLAY.Trim() -eq "") {
            $env:DISPLAY = "dummy:0"
        }
        & $Action
    }
    finally {
        $env:SSH_ASKPASS = $prevAskPass
        $env:SSH_ASKPASS_REQUIRE = $prevAskPassRequire
        $env:DISPLAY = $prevDisplay
        if (Test-Path $askpassPath) {
            Remove-Item -LiteralPath $askpassPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-ExternalCapture([string] $exe, [string[]] $Arguments) {
    $cmdline = $exe + " " + ($Arguments -join " ")
    Write-Host $cmdline
    if ($DryRun) {
        return @{
            ExitCode = 0
            Output = ""
        }
    }
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $exe @Arguments 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    return @{
        ExitCode = $exitCode
        Output = $output
    }
}

function Ensure-SshKeyAuth([string] $HostName, [string] $UserName) {
    if ($DryRun) {
        Write-Host "DryRun: skip SSH key validation and installation."
        return
    }

    $ssh = Get-CommandPath "ssh.exe"
    if (-not $ssh -or $ssh.Trim() -eq "") {
        $ssh = Get-CommandPath "ssh"
    }
    if (-not $ssh -or $ssh.Trim() -eq "") {
        throw "ssh not found. Please install OpenSSH Client (Windows optional feature) or ensure ssh is in PATH."
    }

    $sshKeygen = Get-CommandPath "ssh-keygen.exe"
    if (-not $sshKeygen -or $sshKeygen.Trim() -eq "") {
        $sshKeygen = Get-CommandPath "ssh-keygen"
    }
    if (-not $sshKeygen -or $sshKeygen.Trim() -eq "") {
        throw "ssh-keygen not found. Please install OpenSSH Client."
    }

    $userHome = $env:USERPROFILE
    if (-not $userHome -or $userHome.Trim() -eq "") {
        $userHome = $env:HOME
    }
    if (-not $userHome -or $userHome.Trim() -eq "") {
        throw "Cannot determine user home directory for SSH keys."
    }

    $sshDir = Join-Path $userHome ".ssh"
    if (-not (Test-Path $sshDir)) {
        New-Item -ItemType Directory -Path $sshDir -Force | Out-Null
    }

    $keyPath = Join-Path $sshDir "id_ed25519"
    $pubKeyPath = $keyPath + ".pub"
    if (-not (Test-Path $keyPath) -or -not (Test-Path $pubKeyPath)) {
        Write-Host "SSH key not found. ssh-keygen will run once; you can just press Enter for empty passphrase."
        Invoke-External $sshKeygen @("-t", "ed25519", "-f", $keyPath)
    }

    $testArgs = @("-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "$UserName@$HostName", "echo ok")
    if ($DryRun) {
        Invoke-ExternalCapture $ssh $testArgs | Out-Null
        Write-Host "DryRun: skip SSH key validation and installation."
        return
    }

    $test1 = Invoke-ExternalCapture $ssh $testArgs
    if ($test1.ExitCode -eq 0) {
        Write-Host "SSH key authentication succeeded."
        return
    }

    $out1 = ""
    if ($null -ne $test1 -and $null -ne $test1.Output) {
        $out1 = [string]$test1.Output
    }
    $hostKeyMismatch = ($out1 -match "REMOTE HOST IDENTIFICATION HAS CHANGED" -or $out1 -match "Host key verification failed")
    $networkError = ($out1 -match "Could not resolve hostname" -or $out1 -match "Connection timed out" -or $out1 -match "No route to host" -or $out1 -match "Connection refused")

    if ($networkError) {
        throw ("SSH connection failed:`n" + $out1)
    }

    if ($hostKeyMismatch) {
        Write-Host "Host key mismatch detected; removing known_hosts entry."
        Invoke-External $sshKeygen @("-R", $HostName)
    }

    Write-Host "Installing SSH public key to remote; you may be prompted for password."
    $passwordPlain = Get-PlaintextPassword $Password
    $scp = Get-CommandPath "scp.exe"
    if (-not $scp -or $scp.Trim() -eq "") {
        $scp = Get-CommandPath "scp"
    }
    if (-not $scp -or $scp.Trim() -eq "") {
        throw "scp not found. Please install OpenSSH Client (Windows optional feature) or ensure scp is in PATH."
    }

    $remotePubKeyPath = "/tmp/windows_pubkey_" + ([Guid]::NewGuid().ToString("N")) + ".pub"
    $scpInstallArgs = @(
        "-q",
        "-o", "StrictHostKeyChecking=no",
        "-o", "PreferredAuthentications=keyboard-interactive,password",
        "-o", "PubkeyAuthentication=no",
        $pubKeyPath,
        "$UserName@${HostName}:$remotePubKeyPath"
    )
    $sshInstallArgs = @(
        "-o", "StrictHostKeyChecking=no",
        "-o", "PreferredAuthentications=keyboard-interactive,password",
        "-o", "PubkeyAuthentication=no",
        "$UserName@$HostName",
        "umask 077; mkdir -p ~/.ssh && cat $remotePubKeyPath >> ~/.ssh/authorized_keys && rm -f $remotePubKeyPath"
    )
    if (-not $DryRun) {
        Invoke-WithSshAskPass -passwordPlain $passwordPlain -Action {
            $copyResult = Invoke-ExternalCapture $scp $scpInstallArgs
            if ($copyResult.ExitCode -ne 0) {
                throw ("Failed to copy public key to remote host:`n" + $copyResult.Output)
            }

            $installResult = Invoke-ExternalCapture $ssh $sshInstallArgs
            if ($installResult.ExitCode -ne 0) {
                throw ("Failed to install SSH public key to remote host:`n" + $installResult.Output)
            }
        }
    }
    else {
        $cmdline1 = $scp + " " + ($scpInstallArgs -join " ")
        $cmdline2 = $ssh + " " + ($sshInstallArgs -join " ")
        Write-Host $cmdline1
        Write-Host $cmdline2
    }

    $test2 = Invoke-ExternalCapture $ssh $testArgs
    Write-Host ("SSH BatchMode test exit: " + $test2.ExitCode)
    if ($test2.ExitCode -ne 0) {
        $out2 = ""
        if ($null -ne $test2 -and $null -ne $test2.Output) {
            $out2 = [string]$test2.Output
        }
        if ($out2 -and $out2.Trim() -ne "") {
            Write-Host $out2
        }
        $hostKeyMismatch2 = ($out2 -match "REMOTE HOST IDENTIFICATION HAS CHANGED" -or $out2 -match "Host key verification failed")
        if ($hostKeyMismatch2) {
            Write-Host "Host key mismatch detected after install; removing known_hosts entry and retrying."
            Invoke-External $sshKeygen @("-R", $HostName)
            if (-not $DryRun) {
                Invoke-WithSshAskPass -passwordPlain $passwordPlain -Action {
                    $copyResult2 = Invoke-ExternalCapture $scp $scpInstallArgs
                    if ($copyResult2.ExitCode -ne 0) {
                        throw ("Failed to copy public key to remote host after resetting host key:`n" + $copyResult2.Output)
                    }
                    $installResult2 = Invoke-ExternalCapture $ssh $sshInstallArgs
                    if ($installResult2.ExitCode -ne 0) {
                        throw ("Failed to install SSH public key to remote host after resetting host key:`n" + $installResult2.Output)
                    }
                }
            }
            $test3 = Invoke-ExternalCapture $ssh $testArgs
            if ($test3.ExitCode -ne 0) {
                $out3 = ""
                if ($null -ne $test3 -and $null -ne $test3.Output) {
                    $out3 = [string]$test3.Output
                }
                throw ("SSH key authentication still failing after reinstalling key:`n" + $out3)
            }
        }
        else {
            throw ("SSH key authentication still failing after reinstalling key:`n" + $out2)
        }
    }
    Write-Host "SSH key authentication configured successfully."
}

function Invoke-External([string] $exe, [string[]] $Arguments) {
    $cmdline = $exe + " " + ($Arguments -join " ")
    Write-Host $cmdline
    if ($DryRun) {
        return
    }
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $exe @Arguments
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    if ($exitCode -ne 0) {
        throw "Command failed ($exitCode): $cmdline"
    }
}

function Invoke-RemoteCommand([string] $HostName, [string] $UserName, [string] $remoteCommand) {
    if ($DryRun) {
        Write-Host ("ssh " + $UserName + "@" + $HostName + " " + $remoteCommand)
        return
    }
    $ssh = Get-CommandPath "ssh.exe"
    if (-not $ssh -or $ssh.Trim() -eq "") {
        $ssh = Get-CommandPath "ssh"
    }
    if (-not $ssh -or $ssh.Trim() -eq "") {
        throw "ssh not found. Please install OpenSSH Client (Windows optional feature) or ensure ssh is in PATH."
    }
    Invoke-External $ssh @("-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "$UserName@$HostName", $remoteCommand)
}

function Copy-ToRemote([string] $HostName, [string] $UserName, [string] $localPath, [string] $remotePath) {
    if ($DryRun) {
        Write-Host ("scp " + $localPath + " " + $UserName + "@" + $HostName + ":" + $remotePath)
        return
    }
    $scp = Get-CommandPath "scp.exe"
    if (-not $scp -or $scp.Trim() -eq "") {
        $scp = Get-CommandPath "scp"
    }
    if (-not $scp -or $scp.Trim() -eq "") {
        throw "scp not found. Please install OpenSSH Client (Windows optional feature) or ensure scp is in PATH."
    }
    $remoteTemp = $remotePath + ".upload_" + ([Guid]::NewGuid().ToString("N"))
    Invoke-External $scp @("-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", $localPath, "$UserName@${HostName}:$remoteTemp")
    Invoke-RemoteCommand -HostName $HostName -UserName $UserName -remoteCommand ("sh -c 'mv -f " + $remoteTemp + " " + $remotePath + "'")
}

function Sync-RemoteTime([string] $HostName, [string] $UserName) {
    $localNow = Get-Date
    $timeToSet = $localNow.ToString("yyyy-MM-dd HH:mm:ss")
    Write-Host ("Syncing remote time => " + $timeToSet)
    Invoke-RemoteCommand -HostName $HostName -UserName $UserName -remoteCommand ("date -s '" + $timeToSet + "' >/dev/null && date '+%F %T %z'")
}

$projectRoot = Resolve-ProjectRoot
$buildBatPath = Join-Path $projectRoot "build.bat"
if (-not (Test-Path $buildBatPath)) {
    throw "build.bat not found: $buildBatPath"
}

$projectNameOverride = "MB_DDF_v2"

Write-Host ("ProjectName: " + $projectNameOverride)

$buildConfig = "Debug"
if ($Run) {
    $buildConfig = "Release"
}
$buildProfile = "echo"
$buildAction = $buildConfig.ToLower()
$appMode = "ECHO"
if ($SourceDebug) {
    # The recursive source-debug image is intentionally Debug-only, including
    # when -Run is used to execute it directly on the target.
    $buildConfig = "Debug"
    $buildProfile = "source_debug"
    $buildAction = "source_debug"
    $appMode = "SOURCE_DEBUG (DYT entry)"
}
elseif ($FullHardware) {
    $buildProfile = "demo"
    $buildAction = "hw_" + $buildConfig.ToLower()
    $appMode = "DEMO"
}
elseif ($HardwareTest) {
    $buildProfile = "hw_test"
    $buildAction = "hw_test_" + $buildConfig.ToLower()
    $appMode = "HW_TEST"
}
$buildDir = Join-Path $projectRoot (
    Join-Path (Join-Path "build\aarch64" $buildProfile) $buildConfig
)
Write-Host ("Build Configuration: " + $buildConfig)
Write-Host ("Application Image: " + $appMode)
Write-Host ("Build Directory: " + $buildDir)

Write-Host "cmd /c .\build.bat $buildAction"
if (-not $DryRun) {
    cmd /c (Join-Path $projectRoot "build.bat") $buildAction
    if ($LASTEXITCODE -ne 0) {
        throw "build.bat $buildAction failed with exit code $LASTEXITCODE"
    }
}

if ($DryRun) {
    Write-Host "DryRun: skip updating .vscode/launch.json."
}
else {
    $null = Update-LaunchJsonMiDebuggerPath -projectRoot $projectRoot -buildDir $buildDir
    $null = Update-LaunchJsonSysroot -projectRoot $projectRoot -buildDir $buildDir
    $null = Update-LaunchJsonProgram -projectRoot $projectRoot -projectName $projectNameOverride -buildProfile $buildProfile -buildConfig $buildConfig
}

$remote = "$RemoteUser@$RemoteHost"

Ensure-SshKeyAuth -HostName $RemoteHost -UserName $RemoteUser

Sync-RemoteTime -HostName $RemoteHost -UserName $RemoteUser

Invoke-RemoteCommand -HostName $RemoteHost -UserName $RemoteUser -remoteCommand ("mkdir -p " + $RemoteDir)

$targetBaseName = $projectNameOverride
try {
    Invoke-RemoteCommand -HostName $RemoteHost -UserName $RemoteUser -remoteCommand ("sh -c 'pkill -f ""gdbserver :$RemoteGdbPort"" 2>/dev/null || true; pkill -f ""$targetBaseName"" 2>/dev/null || true'")
}
catch {
    Write-Host "Warning: failed to run remote pkill cleanup; continuing."
}

$vsCodeDir = Join-Path $projectRoot ".vscode"
if (-not $DryRun -and -not (Test-Path $vsCodeDir)) {
    New-Item -ItemType Directory -Path $vsCodeDir -Force | Out-Null
}
$gdbserverMarker = Join-Path $vsCodeDir ".gdbserver_uploaded"

if (-not (Test-Path $gdbserverMarker)) {
    $gdbserverLocal = Join-Path $projectRoot "tools\\gdbserver"
    if (-not $DryRun -and -not (Test-Path $gdbserverLocal)) {
        throw "Local gdbserver not found: $gdbserverLocal"
    }
    Copy-ToRemote -HostName $RemoteHost -UserName $RemoteUser -localPath $gdbserverLocal -remotePath ($RemoteDir.TrimEnd("/") + "/gdbserver")
    if (-not $DryRun) {
        Set-Content -LiteralPath $gdbserverMarker -Value ([DateTime]::Now.ToString("s"))
    }
}
else {
    Write-Host "Skip uploading gdbserver (marker exists)."
}

$candidateLocalBinaries = @(
    (Join-Path $buildDir $projectNameOverride),
    (Join-Path $buildDir ($projectNameOverride + ".exe"))
)
$localBinaryPath = ""
foreach ($p in $candidateLocalBinaries) {
    if ($p -and (Test-Path $p)) {
        $localBinaryPath = $p
        break
    }
}
if (-not $localBinaryPath) {
    if ($DryRun) {
        $localBinaryPath = $candidateLocalBinaries[0]
        Write-Host "DryRun: use expected binary path without requiring a build artifact: $localBinaryPath"
    }
    else {
        throw "Built binary not found. Checked: $($candidateLocalBinaries -join ', ')"
    }
}

$remoteBinaryPath = ($RemoteDir.TrimEnd('/') + "/" + $projectNameOverride)
Copy-ToRemote -HostName $RemoteHost -UserName $RemoteUser -localPath $localBinaryPath -remotePath $remoteBinaryPath

Invoke-RemoteCommand -HostName $RemoteHost -UserName $RemoteUser -remoteCommand ("cd " + $RemoteDir + " && chmod +x *")

$remoteDirNormalized = $RemoteDir.TrimEnd('/')

if ($Run) {
    $sshPath = "ssh"
    if (-not $DryRun) {
        $sshPath = Get-CommandPath "ssh.exe"
        if (-not $sshPath -or $sshPath.Trim() -eq "") {
            $sshPath = Get-CommandPath "ssh"
        }
        if (-not $sshPath -or $sshPath.Trim() -eq "") {
            throw "ssh not found. Please install OpenSSH Client (Windows optional feature) or ensure ssh is in PATH."
        }
    }
     
    #  $confirmation = Read-Host "Deploy complete. Do you want to run the program? [Y/n]"
    #  if ($confirmation -and $confirmation.Trim().ToLower() -eq "n") {
    #      Write-Host "Run cancelled by user."
    #      exit 0
    #  }

    Write-Host "Running program directly via SSH..."
    $runProgram = "./$projectNameOverride"
    $runEnvironment = @()
    if ($FullHardware) {
        Write-Warning "Full hardware capability mode enabled, including DH main engine 2 ignition."
        $runEnvironment += "MB_DDF_HW_FULL_DEMO=1"
    }
    if ($SpiFlashTestAddress) {
        $runEnvironment += "MB_DDF_HW_SPI_FLASH_TEST_ADDRESS=$SpiFlashTestAddress"
    }
    if ($runEnvironment.Count -gt 0) {
        $runProgram = ($runEnvironment -join " ") + " " + $runProgram
    }
    $runCmd = "cd $remoteDirNormalized && chmod +x * && $runProgram"
    $args = @("-t", "-o", "StrictHostKeyChecking=no", "$RemoteUser@$RemoteHost", $runCmd)
    
    Write-Host ($sshPath + " " + ($args -join " "))
    if (-not $DryRun) {
        # 使用 -NoNewWindow 让输出显示在当前终端
        $process = Start-Process -FilePath $sshPath -ArgumentList $args -Wait -NoNewWindow -PassThru
        exit $process.ExitCode
    }
    exit 0
}

if ($ForegroundGdbserver) {
    $sshPath = "ssh"
    if (-not $DryRun) {
        $sshPath = Get-CommandPath "ssh.exe"
        if (-not $sshPath -or $sshPath.Trim() -eq "") {
            $sshPath = Get-CommandPath "ssh"
        }
        if (-not $sshPath -or $sshPath.Trim() -eq "") {
            throw "ssh not found. Please install OpenSSH Client (Windows optional feature) or ensure ssh is in PATH."
        }
    }
    $foregroundCmd = "cd $remoteDirNormalized && chmod +x * && ./gdbserver :$RemoteGdbPort $remoteBinaryPath"
    $args = @("-o", "StrictHostKeyChecking=no", "$RemoteUser@$RemoteHost", $foregroundCmd)
    Write-Host ($sshPath + " " + ($args -join " "))
    if (-not $DryRun) {
        Start-Process -FilePath $sshPath -ArgumentList $args | Out-Null
    }
}
else {
    $remoteGdbserverCmd = "sh -c 'cd $remoteDirNormalized && nohup ./gdbserver :$RemoteGdbPort $remoteBinaryPath > gdbserver.log 2>&1 < /dev/null & echo gdbserver_started'"
    Invoke-RemoteCommand -HostName $RemoteHost -UserName $RemoteUser -remoteCommand $remoteGdbserverCmd
}

Write-Host ""
Write-Host ("Remote: " + $remote)
Write-Host ("RemoteDir: " + $RemoteDir)
Write-Host ("Program: " + $remoteBinaryPath)
Write-Host ("GDB Server: " + ($RemoteDir.TrimEnd("/") + "/gdbserver"))
Write-Host ("Port: " + $RemoteGdbPort)
Write-Host ""
Write-Host "Remote deploy complete; you can press F5 to start debugging."

