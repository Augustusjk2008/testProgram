# tools/benchmark-build.ps1 - 编译性能基准测试
param(
    [int]$Iterations = 1
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "    MB_DDF_v2 编译性能基准测试" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 获取系统信息
$cpu = Get-WmiObject -Class Win32_Processor | Select-Object -First 1
$powerScheme = (powercfg /getactivescheme) -replace '.*\((.*)\).*', '$1'

Write-Host "`n系统配置:" -ForegroundColor Yellow
Write-Host "  CPU: $($cpu.Name)" -ForegroundColor Gray
Write-Host "  核心: $($cpu.NumberOfLogicalProcessors) 线程" -ForegroundColor Gray
Write-Host "  电源计划: $powerScheme" -ForegroundColor $(if($powerScheme -match "节能") { "Red" } else { "Green" })

$projectRoot = Resolve-Path "$PSScriptRoot\.."
$results = @()

for ($i = 1; $i -le $Iterations; $i++) {
    Write-Host "`n----------------------------------------" -ForegroundColor Cyan
    Write-Host "测试迭代 $i / $Iterations" -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    # 清理
    Write-Host "清理构建目录..." -ForegroundColor Yellow
    $buildDir = "$projectRoot\build"
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir
    }
    
    # 构建
    Write-Host "开始构建 (Release)..." -ForegroundColor Yellow
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    
    try {
        & "$projectRoot\build.ps1" release -Jobs $cpu.NumberOfLogicalProcessors 2>&1 | Out-Null
        $success = $LASTEXITCODE -eq 0
    } catch {
        $success = $false
    }
    
    $stopwatch.Stop()
    $elapsed = $stopwatch.Elapsed
    
    $result = [PSCustomObject]@{
        Iteration = $i
        Success = $success
        Duration = $elapsed
        Seconds = $elapsed.TotalSeconds
    }
    $results += $result
    
    $statusColor = if ($success) { "Green" } else { "Red" }
    Write-Host "结果: $($elapsed.ToString('hh\:mm\:ss')) ($($elapsed.TotalSeconds.ToString('F1')) 秒)" -ForegroundColor $statusColor
}

# 汇总
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "    测试结果汇总" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$successful = $results | Where-Object { $_.Success }
if ($successful.Count -gt 0) {
    $avg = ($successful | Measure-Object -Property Seconds -Average).Average
    $min = ($successful | Measure-Object -Property Seconds -Minimum).Minimum
    $max = ($successful | Measure-Object -Property Seconds -Maximum).Maximum
    
    Write-Host "成功次数: $($successful.Count) / $Iterations" -ForegroundColor Green
    Write-Host "平均时间: $([TimeSpan]::FromSeconds($avg).ToString('hh\:mm\:ss')) ($($avg.ToString('F1')) 秒)" -ForegroundColor Yellow
    Write-Host "最快时间: $([TimeSpan]::FromSeconds($min).ToString('hh\:mm\:ss')) ($($min.ToString('F1')) 秒)" -ForegroundColor Green
    Write-Host "最慢时间: $([TimeSpan]::FromSeconds($max).ToString('hh\:mm\:ss')) ($($max.ToString('F1')) 秒)" -ForegroundColor Red
    
    if ($powerScheme -match "节能") {
        Write-Host "`n⚠️ 警告：当前为节能模式，切换到高性能模式预计可提升 50-100%" -ForegroundColor Red
    }
} else {
    Write-Host "所有构建都失败了，请检查编译环境" -ForegroundColor Red
}

Write-Host "`n建议:" -ForegroundColor Yellow
Write-Host "  1. 如果平均时间 > 5 分钟，建议检查电源计划" -ForegroundColor Gray
Write-Host "  2. 如果平均时间 > 10 分钟，可能存在其他问题" -ForegroundColor Gray
Write-Host "  3. 对比同事的构建时间，如果差距 > 50%，继续排查" -ForegroundColor Gray
