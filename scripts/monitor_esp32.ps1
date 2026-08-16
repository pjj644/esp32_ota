# monitor_esp32.ps1 - 后台启动 idf_monitor 抓取 ESP32 串口日志 (无人值守)
#
# 用法:
#   .\scripts\monitor_esp32.ps1                 # 抓日志到 logs\esp32_monitor.log
#   .\scripts\monitor_esp32.ps1 -Log 自定义.log # 指定日志文件
#
# 说明:
#   - monitor 连接串口时会复位设备, 日志从 boot 开始。
#   - 中文在重定向文件里是 UTF-8, grep 时用 ASCII 关键词 (manifest/200/TIMEOUT/同步无 ACK 不适用...用 SYNCFAIL 等)。
#   - 关键: PATH 必须含 xtensa 工具链 (addr2line), 否则 monitor 一崩退出 (WinError 2)。
#   - 再跑一次本脚本 = 杀掉旧 monitor 重启抓取。

param(
    [string]$Log = "",
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    if (-not $Log) { $Log = $P.MONITOR_LOG }
    New-Item -ItemType Directory -Path $P.LOG_DIR -Force | Out-Null

    Stop-Residual 'idf_monitor'
    Start-Sleep -Seconds 1

    Get-IdfPy | Out-Null   # 设好 PATH (addr2line)
    Start-Process "$($P.IDF_PYTHON_ENV)\Scripts\python.exe" `
        -ArgumentList "$($P.IDF_PATH)\tools\idf_monitor.py","-p",$P.ESP32_COM,"-b","115200","--timestamps","$($P.ESP32_ELF)" `
        -WorkingDirectory $P.ROOT `
        -RedirectStandardOutput $Log `
        -RedirectStandardError "$Log.err" -WindowStyle Hidden
    Start-Sleep -Seconds 3

    if (-not (Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'idf_monitor' })) {
        throw "monitor 启动后立即退出, 看 $Log.err"
    }
    Write-Host "== [OK] monitor 运行中, 日志: $Log"
    Write-Host "   设备已复位, 日志从 boot 开始 (OTA 验证需等 >=70s 一轮周期)"
    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
