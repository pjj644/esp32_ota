# flash_stm32.ps1 - DAPLink (openocd) 直连烧录 STM32, 不经 OTA
#
# 用法:
#   .\scripts\flash_stm32.ps1                # 烧 build\Debug\esp32_test.elf
#   .\scripts\flash_stm32.ps1 -Elf 其他.elf  # 指定 elf
#
# 前置: DAPLink 接 SWD (HID), 目标 STM32F103; 烧录期间会拉 NRST。
# 注意: openocd 输出带 ANSI 转义, 显示前已剥离; 失败时看原始输出。

param(
    [string]$Elf = "",
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    if (-not $Elf) { $Elf = $P.STM32_ELF }
    if (-not (Test-Path $Elf)) { throw "elf 不存在: $Elf (先跑 scripts\build_stm32.ps1)" }

    Stop-Residual 'openocd'
    Start-Sleep -Seconds 1

    Write-Host "== openocd 烧录 STM32: $Elf"
    $out = & $P.OPENOCD_BIN -s $P.OPENOCD_SCRIPTS `
        -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg `
        -c "adapter speed 2000" `
        -c "program $Elf verify reset exit" 2>&1
    $out | ForEach-Object { $_ -replace '\x1b\[[0-9;]*m', '' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "openocd 烧录失败 (见上方输出)" }
    Write-Host "== [OK] STM32 已烧录并复位"

    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
