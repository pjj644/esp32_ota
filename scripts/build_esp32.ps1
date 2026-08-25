# build_esp32.ps1 - 构建 ESP32 固件: 升版本 -> 编译 -> 部署到 server/firmware/
#
# 用法:
#   .\scripts\build_esp32.ps1                        # 版本自动 patch+1 (1.0.8 -> 1.0.9)
#   .\scripts\build_esp32.ps1 -Version 1.1.0         # 指定新版本
#   .\scripts\build_esp32.ps1 -NoVersionBump         # 版本不变, 只重新编译部署 (改代码用)
#
# 产出: build/local_test.bin -> server/firmware/firmware.bin + version.json 同步。
# 注意: 版本号必须三处一致 (sdkconfig.defaults / version.json / bin 内嵌), 本脚本已自动处理。

param(
    [string]$Version = "",
    [switch]$NoVersionBump,
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    # ===== 1) 版本 =====
    $cur = Get-JsonVersion $P.VERSION_JSON
    $new = Get-NewVersion $cur $Version
    if ($NoVersionBump) {
        if ($Version) { throw "-NoVersionBump 与 -Version 不能同时使用" }
        $new = $cur
        Write-Host "== ESP32 版本保持: $cur (NoVersionBump)"
    } else {
        if ($new -eq $cur) { throw "ESP32 新版本与当前相同 ($new), 请用 -Version 指定或 -NoVersionBump" }
        Write-Host "== ESP32 版本: $cur -> $new"
        Set-Esp32Version $new
    }

    # ===== 2) 编译 (含 sdkconfig 强制重生成 + LOCAL_HOST 一致性检查) =====
    $idfPy = Get-IdfPy
    Sync-Sdkconfig $new
    [void](Test-LocalHostConsistency)
    Write-Host "== 编译 ESP32 ..."
    & $idfPy[0] $idfPy[1] build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build 失败" }

    # ===== 3) 部署到 server/firmware + 验证 bin 内嵌版本 =====
    Copy-Item -Force $P.ESP32_BIN $P.FIRMWARE_BIN
    $txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($P.FIRMWARE_BIN))
    if (-not $txt.Contains($new)) { throw "警告: firmware.bin 未包含版本 $new, 检查 sdkconfig 重生成" }
    Write-Host "== [OK] ESP32 v$new 已部署: server/firmware/firmware.bin (bin 含版本号已验证)"

    Write-Host ""
    Write-Host "== 完成。设备 OTA 检查(<=60s) 将自动升级。"
    Write-Host "   若本机 IP 变了, 记得同步修改 main\main.c 的 LOCAL_HOST 并重新烧录。"
    Pause-IfNeeded ""
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    Pause-IfNeeded ""
}
