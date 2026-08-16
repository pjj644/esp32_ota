# flash_esp32.ps1 - 直接烧录 ESP32 (不经 OTA)
#
# 用法:
#   .\scripts\flash_esp32.ps1                 # 用默认串口 (COM6)
#   .\scripts\flash_esp32.ps1 -Port COM4      # 指定串口
#
# 写 4 段: bootloader(0x1000) + partition-table(0x8000) + ota_data_initial(0xd000) + app(0x10000)。
# 板子: 40MHz DIO, 4MB Flash。烧录前会自动杀掉残留的 idf_monitor (否则串口被占报 PermissionError)。

param(
    [string]$Port = $null,
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    if (-not $Port) { $Port = $P.ESP32_COM }
    Stop-Residual 'idf_monitor'
    Start-Sleep -Seconds 1

    $idfPy = Get-IdfPy
    Write-Host "== 烧录 ESP32 @ $Port (460800) ..."
    & $idfPy[0] -m esptool --chip esp32 -b 460800 -p $Port write-flash `
        --flash-mode dio --flash-size 4MB --flash-freq 40m `
        0x1000 $P.BOOTLOADER_BIN `
        0x8000 $P.PARTTABLE_BIN `
        0xd000 $P.OTA_DATA_BIN `
        0x10000 $P.ESP32_BIN
    if ($LASTEXITCODE -ne 0) { throw "esptool 烧录失败" }
    Write-Host "== [OK] ESP32 烧录完成 (含 ota_data_initial.bin, OTA 状态已重置)"

    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
