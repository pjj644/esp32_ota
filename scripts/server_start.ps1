# server_start.ps1 - 启动/重启 OTA 服务器 (+可选实时监视日志)
#
# 用法:
#   .\scripts\server_start.ps1          # 重启服务器
#   .\scripts\server_start.ps1 -Watch   # 重启后实时跟踪 server.log (Ctrl+C 退出)

param(
    [switch]$Watch,
    [switch]$NoPause
)

. (Join-Path $PSScriptRoot "common.ps1")

try {
    Start-OtaServer
    if ($Watch) {
        Write-Host "== 实时跟踪服务器日志 (Ctrl+C 退出):"
        Get-Content -Path $P.SERVER_LOG -Wait -Tail 30
    } else {
        Pause-IfNeeded ""
    }
}
catch {
    Write-Host "== [错误] $($_.Exception.Message)" -ForegroundColor Red
    Pause-IfNeeded ""
}
