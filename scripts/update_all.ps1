# update_all.ps1 - 一键: 全面更新 ESP32+STM32 -> 直接烧录 -> 启动 OTA 服务
#
# 用法:
#   .\scripts\update_all.ps1                            # 两者 patch+1, 全部烧录, 启动服务器
#   .\scripts\update_all.ps1 -Esp32Version 1.1.0 -Stm32Version 2.0.0
#   .\scripts\update_all.ps1 -SkipStm32Flash            # STM32 只部署到 server, 走 OTA 刷写
#
# 流程: build_esp32 (升版+编译+部署) -> build_stm32 (升版+编译+部署)
#       -> flash_esp32 (esptool 直烧) -> flash_stm32 (openocd 直烧, 可跳过)
#       -> 启动服务器。烧录前自动杀掉残留 monitor, 烧完请重跑 monitor_esp32.ps1 抓日志。

param(
    [string]$Esp32Version = "",
    [string]$Stm32Version = "",
    [switch]$SkipStm32Flash,
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    # ===== 1) 构建并部署 (自动升版本) =====
    Write-Host "`n########## [1/5] 构建 ESP32 ##########"
    & (Join-Path $PSScriptRoot "build_esp32.ps1") -Version $Esp32Version -NoPause
    if ($LASTEXITCODE -ne 0) { throw "build_esp32 失败" }

    Write-Host "`n########## [2/5] 构建 STM32 ##########"
    & (Join-Path $PSScriptRoot "build_stm32.ps1") -Version $Stm32Version -NoPause
    if ($LASTEXITCODE -ne 0) { throw "build_stm32 失败" }

    # ===== 2) 直接烧录 ESP32 =====
    Write-Host "`n########## [3/5] 烧录 ESP32 ##########"
    & (Join-Path $PSScriptRoot "flash_esp32.ps1") -NoPause
    if ($LASTEXITCODE -ne 0) { throw "flash_esp32 失败" }

    # ===== 3) 直接烧录 STM32 (可选) =====
    if (-not $SkipStm32Flash) {
        Write-Host "`n########## [4/5] 烧录 STM32 (DAPLink) ##########"
        & (Join-Path $PSScriptRoot "flash_stm32.ps1") -NoPause
        if ($LASTEXITCODE -ne 0) { throw "flash_stm32 失败" }
    } else {
        Write-Host "`n########## [4/5] 跳过 STM32 直烧 (SkipStm32Flash), 交给 OTA 推送 ##########"
    }

    # ===== 4) 启动服务器 =====
    Write-Host "`n########## [5/5] 启动 OTA 服务器 ##########"
    Start-OtaServer

    $espNew = Get-JsonVersion $P.VERSION_JSON
    $stmNew = Get-JsonVersion $P.STM32_VER_JSON
    Write-Host ""
    Write-Host "== 全部完成: ESP32 v$espNew, STM32 v$stmNew"
    Write-Host "   日志抓取: scripts\monitor_esp32.ps1"
    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
