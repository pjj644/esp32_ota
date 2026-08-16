# common.ps1 - 共享函数库 (所有 scripts/*.ps1 先 dot-source 本文件)
#
# 用法: . (Join-Path $PSScriptRoot "common.ps1")
# 提供: $P 路径常量 (来自 ..\config\paths.ps1) + 以下函数。

. (Join-Path $PSScriptRoot "..\config\paths.ps1") | Out-Null
$ErrorActionPreference = "Stop"

# 交互式暂停 (双击运行时窗口不闪退; -NoPause 跳过)
function Pause-IfNeeded {
    param([string]$msg = "")
    if (-not $script:NoPause) {
        if ($msg) { Write-Host $msg }
        Read-Host "按回车键退出..."
    }
}

# 读取 server/firmware/*.json 里的版本
function Get-JsonVersion {
    param([string]$file)
    $j = Get-Content -Raw $file | ConvertFrom-Json
    return $j.version
}

# 计算新版本: 指定则校验 X.Y.Z, 否则 patch+1
function Get-NewVersion {
    param([string]$cur, [string]$wanted)
    if ($wanted) {
        if ($wanted -notmatch '^\d+\.\d+\.\d+$') {
            throw "版本格式错误: $wanted (需要 X.Y.Z)"
        }
        return $wanted
    }
    $p = $cur.Split('.')
    return "$($p[0]).$($p[1]).$([int]$p[2] + 1)"
}

# 设置 ESP-IDF 构建环境 (export.ps1 不可用, 见 docs/PROBLEMS.md)
function Set-IdfEnv {
    $env:IDF_PATH = $P.IDF_PATH
    $env:IDF_PYTHON_ENV_PATH = $P.IDF_PYTHON_ENV
    $env:IDF_TOOLS_PATH = $P.IDF_TOOLS
    $env:ESP_IDF_VERSION = $P.ESP_IDF_VERSION
    $env:ESP_ROM_ELF_DIR = $P.ESP_ROM_ELF_DIR
    $env:PATH = ($P.ESP_TOOLCHAIN_BIN + ';' + $P.NINJA_BIN + ';' + $P.CMAKE_BIN + ';' +
                 $P.CCACHE_BIN + ';' + $P.IDF_EXE_BIN + ';' + $env:PATH)
}

# 返回 idf.py 的 python 调用前缀 (数组)
function Get-IdfPy {
    Set-IdfEnv
    return @("$($P.IDF_PYTHON_ENV)\Scripts\python.exe", "$($P.IDF_PATH)\tools\idf.py")
}

# 大坑修复 (docs/PROBLEMS.md #2): 生成的 sdkconfig 里版本 != 目标时, 备份并删除强制重生成,
# 否则 sdkconfig.defaults 的版本改动不生效, OTA 永远"看起来失败"。
function Sync-Sdkconfig {
    param([string]$targetVer)
    if (Test-Path $P.SDK_GENERATED) {
        $line = (Select-String '^CONFIG_APP_PROJECT_VER="' $P.SDK_GENERATED | Select-Object -First 1).Line
        if ($line -notmatch "`"$targetVer`"$") {
            $bk = Join-Path (Split-Path $P.SDK_GENERATED -Parent) "backup"
            New-Item -ItemType Directory -Path $bk -Force | Out-Null
            Copy-Item $P.SDK_GENERATED (Join-Path $bk "sdkconfig.bak") -Force
            Remove-Item $P.SDK_GENERATED
            Write-Host "== [提示] sdkconfig 版本不符 ($line), 已备份并强制重新生成"
        }
    }
}

# 写 ESP32 版本到 sdkconfig.defaults + version.json
function Set-Esp32Version {
    param([string]$ver)
    (Get-Content $P.SDK_DEFAULTS) -replace 'CONFIG_APP_PROJECT_VER="[^"]*"', "CONFIG_APP_PROJECT_VER=`"$ver`"" |
        Set-Content -Encoding ascii $P.SDK_DEFAULTS
    Set-Content -Encoding ascii $P.VERSION_JSON ('{"version":"' + $ver + '"}')
}

# 写 STM32 版本到 main.c APP_VERSION_STR + stm32_version.json
function Set-Stm32Version {
    param([string]$ver)
    (Get-Content $P.STM32_MAIN_C) -replace '#define\s+APP_VERSION_STR\s+"[^"]*"', "#define APP_VERSION_STR  `"$ver`"" |
        Set-Content -Encoding ascii $P.STM32_MAIN_C
    Set-Content -Encoding ascii $P.STM32_VER_JSON ('{"version":"' + $ver + '"}')
}

# 杀掉残留进程 (node server.js / idf_monitor / openocd), 防止串口被占
function Stop-Residual {
    param([string]$pattern)
    Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -match $pattern } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# 启动 OTA 服务器 (杀旧的 -> 起新的 -> /hello 健康检查)
function Start-OtaServer {
    Stop-Residual 'node.*server\.js'
    Start-Sleep -Seconds 1
    New-Item -ItemType Directory -Path $P.LOG_DIR -Force | Out-Null
    Start-Process node -ArgumentList "server.js" `
        -WorkingDirectory $P.SERVER_DIR `
        -RedirectStandardOutput $P.SERVER_LOG `
        -RedirectStandardError "$($P.SERVER_LOG).err" -WindowStyle Hidden
    Start-Sleep -Seconds 2
    try {
        $r = Invoke-RestMethod "http://127.0.0.1:$($P.LOCAL_PORT)/hello" -TimeoutSec 5
        Write-Host "== [OK] 服务器已启动: $r  (日志: $($P.SERVER_LOG))"
    } catch {
        Write-Warning "服务器启动但 /hello 未响应, 查看 $($P.SERVER_LOG)"
    }
}
