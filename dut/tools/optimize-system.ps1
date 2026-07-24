# tools/optimize-system.ps1 - 系统优化脚本（需管理员权限）
#Requires -RunAsAdministrator

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "    MB_DDF_v2 编译环境优化工具" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 1. 设置卓越性能模式
Write-Host "`n[1/6] 设置卓越性能电源计划..." -ForegroundColor Yellow
try {
    # 尝试启用卓越性能模式
    $ultimate = powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61 2>$null
    powercfg /setactive e9a42b02-d5df-448d-aa00-03f14749eb61 2>$null
    Write-Host "   ✓ 已启用卓越性能模式" -ForegroundColor Green
} catch {
    # 回退到高性能
    powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c 2>$null
    Write-Host "   ✓ 已启用高性能模式" -ForegroundColor Green
}

# 2. 禁用 Windows Search 索引（在开发目录）
Write-Host "`n[2/6] 优化 Windows Search..." -ForegroundColor Yellow
$projectPath = (Resolve-Path "$PSScriptRoot\..").Path
$indexingOptions = Get-WmiObject -Class Win32_Volume -Filter "DriveLetter='$($projectPath[0]):'"
if ($indexingOptions) {
    Write-Host "   ℹ 可以考虑禁用 $($projectPath[0]): 盘的索引以提高 I/O 性能" -ForegroundColor Gray
    Write-Host "     控制面板 -> 索引选项 -> 修改 -> 取消勾选开发目录" -ForegroundColor Gray
}

# 3. 设置进程优先级（对当前 PowerShell）
Write-Host "`n[3/6] 优化进程优先级..." -ForegroundColor Yellow
$process = Get-Process -Id $PID
$process.PriorityClass = 'High'
Write-Host "   ✓ 当前 PowerShell 进程优先级已设为 High" -ForegroundColor Green

# 4. 检查后台占用程序
Write-Host "`n[4/6] 检查高 CPU 占用进程..." -ForegroundColor Yellow
$highCpu = Get-Process | Where-Object {$_.CPU -gt 100} | Select-Object -First 5 Name, CPU, WorkingSet
if ($highCpu) {
    Write-Host "   发现以下进程 CPU 占用较高：" -ForegroundColor Yellow
    $highCpu | ForEach-Object { Write-Host "     - $($_.Name): CPU=$($_.CPU.ToString('F0'))" -ForegroundColor Gray }
    Write-Host "   建议在编译期间关闭这些程序" -ForegroundColor Yellow
} else {
    Write-Host "   ✓ 未发现异常高 CPU 占用进程" -ForegroundColor Green
}

# 5. 系统信息汇总
Write-Host "`n[5/6] 系统信息汇总:" -ForegroundColor Yellow
$os = Get-CimInstance Win32_OperatingSystem
$memory = [math]::Round($os.TotalVisibleMemorySize / 1MB, 2)
$freeMemory = [math]::Round($os.FreePhysicalMemory / 1MB, 2)
Write-Host "   内存总量: $memory GB" -ForegroundColor Gray
Write-Host "   可用内存: $freeMemory GB" -ForegroundColor $(if($freeMemory -lt 4) { "Red" } else { "Green" })

$cpu = Get-WmiObject -Class Win32_Processor | Select-Object -First 1
Write-Host "   CPU: $($cpu.Name)" -ForegroundColor Gray
Write-Host "   核心数: $($cpu.NumberOfCores) 核 / $($cpu.NumberOfLogicalProcessors) 线程" -ForegroundColor Gray

# 6. 检查磁盘类型
Write-Host "`n[6/6] 磁盘信息:" -ForegroundColor Yellow
Get-PhysicalDisk | ForEach-Object {
    $type = if ($_.MediaType) { $_.MediaType } else { "未知" }
    Write-Host "   $($_.FriendlyName): $type" -ForegroundColor $(if($type -eq "SSD") { "Green" } else { "Yellow" })
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "    优化完成！建议重新编译测试" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "使用以下命令开始高性能编译:" -ForegroundColor Yellow
Write-Host "   .\build.ps1 release" -ForegroundColor Green
Write-Host "`n其他建议:" -ForegroundColor Yellow
Write-Host "   1. 编译时关闭浏览器、IDE 等大内存程序" -ForegroundColor Gray
Write-Host "   2. 保持笔记本连接电源" -ForegroundColor Gray
Write-Host "   3. 确保散热良好（避免 CPU 降频）" -ForegroundColor Gray
Write-Host "   4. 考虑安装 ccache: winget install ccache" -ForegroundColor Gray
