# build_stm32.ps1 - 构建 STM32 固件: 升版本 -> 编译 -> 部署到 server/firmware/
#
# 用法:
#   .\scripts\build_stm32.ps1                        # 版本自动 patch+1 (1.0.19 -> 1.0.20)
#   .\scripts\build_stm32.ps1 -Version 2.0.0         # 指定新版本
#   .\scripts\build_stm32.ps1 -NoVersionBump         # 版本不变, 只重新编译部署 (改代码用)
#
# 产出: stm32/esp32_test/build/Debug/esp32_test.bin -> server/firmware/stm32.bin
#       + stm32_version.json 同步 (必须比设备 NVS 记录新才触发刷写)。
# 部署后等 ESP32 的 60s 检查周期即可, 无需重启服务器 (实时读文件)。

param(
    [string]$Version = "",
    [switch]$NoVersionBump,
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    # ===== 1) 版本 =====
    $cur = Get-JsonVersion $P.STM32_VER_JSON
    $new = Get-NewVersion $cur $Version
    if ($NoVersionBump) {
        if ($Version) { throw "-NoVersionBump 与 -Version 不能同时使用" }
        $new = $cur
        Write-Host "== STM32 版本保持: $cur (NoVersionBump)"
    } else {
        if ($new -eq $cur) { throw "STM32 新版本与当前相同 ($new), 请用 -Version 指定或 -NoVersionBump" }
        Write-Host "== STM32 版本: $cur -> $new"
        Set-Stm32Version $new
    }

    # ===== 2) 编译 (cmake Ninja, 需在 PATH; Get-IdfPy 已把 ninja 加进 PATH) =====
    Get-IdfPy | Out-Null
    Write-Host "== 编译 STM32 ..."
    cmake --build $P.STM32_BUILD
    if ($LASTEXITCODE -ne 0) { throw "STM32 cmake build 失败" }

    # ===== 3) objcopy 生成 bin + 部署 =====
    & $P.ARM_OBJCOPY -O binary $P.STM32_ELF $P.STM32_BIN
    if ($LASTEXITCODE -ne 0) { throw "objcopy 失败" }
    Copy-Item -Force $P.STM32_BIN $P.STM32_FW_BIN
    Write-Host "== [OK] STM32 v$new 已部署: server/firmware/stm32.bin"

    Write-Host ""
    Write-Host "== 完成。ESP32 下次检查(<=60s)将经 UART 刷写 STM32。"
    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
