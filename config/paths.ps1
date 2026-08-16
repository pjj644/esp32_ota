# paths.ps1 - 本项目全部路径/端口/常量中心
#
# 用法: 任何 scripts/*.ps1 先 dot-source 本文件 (common.ps1 已自动包含):
#   . (Join-Path $PSScriptRoot "..\config\paths.ps1")
# 之后通过 $P 哈希表访问常量, 例如 $P.IDF_PATH, $P.ESP32_COM。
# 修改机器相关配置 (串口、IP、工具链路径) 只需改这一个文件。

$P = @{}

# ---- 工程根 ----
$P.ROOT = Split-Path $PSScriptRoot -Parent

# ---- ESP-IDF 工具链 (自定义安装路径, 不在 PATH 里) ----
$P.IDF_PATH          = "D:\esp32\.espressif\v6.0.1\esp-idf"
$P.IDF_PYTHON_ENV    = "C:\Espressif\tools\python\v6.0.1\venv"   # venv 里的 python.exe
$P.IDF_TOOLS         = "C:\Espressif\tools"
$P.ESP_IDF_VERSION   = "6.0.1"      # 不设则 idf.py 报 TypeError
$P.ESP_ROM_ELF_DIR   = "C:\Espressif\tools\esp-rom-elfs\20241011\esp-rom-elfs"  # 缺了只警告, 补上安心
$P.ESP_TOOLCHAIN_BIN = "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin"  # 含 addr2line, monitor 需要
$P.NINJA_BIN         = "C:\Espressif\tools\ninja\1.12.1"
$P.CMAKE_BIN         = "C:\Espressif\tools\cmake\4.0.3\bin"
$P.CCACHE_BIN        = "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"
$P.IDF_EXE_BIN       = "C:\Espressif\tools\idf-exe\1.0.3"

# ---- 串口 ----
$P.ESP32_COM  = "COM6"   # ESP32 (CH340) 串口: 烧录(460800) + 日志 monitor(115200)
$P.DAPLINK_COM = "COM3"  # DAPLink VCOM: 收 STM32 USART1 日志 @115200

# ---- 本地服务器 (OTA) ----
$P.LOCAL_HOST = "192.168.1.11"   # 电脑局域网 IP, 必须与 main\main.c 的 LOCAL_HOST 一致
$P.LOCAL_PORT = 8888
$P.SERVER_DIR = Join-Path $P.ROOT "server"

# ---- ESP32 工程文件 ----
$P.SDK_DEFAULTS   = Join-Path $P.ROOT "sdkconfig.defaults"        # CONFIG_APP_PROJECT_VER
$P.SDK_GENERATED  = Join-Path $P.ROOT "sdkconfig"                 # 生成文件, 版本不符需删
$P.PARTITIONS_CSV = Join-Path $P.ROOT "partitions.csv"
$P.ESP32_MAIN_C   = Join-Path $P.ROOT "main\main.c"               # LOCAL_HOST/LOCAL_PORT
$P.ESP32_BUILD    = Join-Path $P.ROOT "build"
$P.ESP32_BIN      = Join-Path $P.ROOT "build\local_test.bin"
$P.ESP32_ELF      = Join-Path $P.ROOT "build\local_test.elf"
$P.BOOTLOADER_BIN = Join-Path $P.ROOT "build\bootloader\bootloader.bin"
$P.PARTTABLE_BIN  = Join-Path $P.ROOT "build\partition_table\partition-table.bin"
$P.OTA_DATA_BIN   = Join-Path $P.ROOT "build\ota_data_initial.bin"

# ---- STM32 工程 (CubeMX, 独立 CMake) ----
$P.STM32_DIR     = Join-Path $P.ROOT "stm32\esp32_test"
$P.STM32_MAIN_C  = Join-Path $P.ROOT "stm32\esp32_test\Core\Src\main.c"   # APP_VERSION_STR
$P.STM32_BUILD   = Join-Path $P.ROOT "stm32\esp32_test\build\Debug"
$P.STM32_ELF     = Join-Path $P.ROOT "stm32\esp32_test\build\Debug\esp32_test.elf"
$P.STM32_BIN     = Join-Path $P.ROOT "stm32\esp32_test\build\Debug\esp32_test.bin"
$P.ARM_OBJCOPY   = "D:\tools\arm-gnu\bin\arm-none-eabi-objcopy.exe"
$P.OPENOCD_BIN   = "C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260304\openocd-esp32\bin\openocd.exe"
$P.OPENOCD_SCRIPTS = "C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260304\openocd-esp32\share\openocd\scripts"

# ---- server/firmware (OTA 发布位) ----
$P.FIRMWARE_DIR    = Join-Path $P.ROOT "server\firmware"
$P.VERSION_JSON    = Join-Path $P.ROOT "server\firmware\version.json"        # ESP32 版本
$P.STM32_VER_JSON  = Join-Path $P.ROOT "server\firmware\stm32_version.json"  # STM32 版本
$P.FIRMWARE_BIN    = Join-Path $P.ROOT "server\firmware\firmware.bin"
$P.STM32_FW_BIN    = Join-Path $P.ROOT "server\firmware\stm32.bin"

# ---- 日志 ----
$P.LOG_DIR      = Join-Path $P.ROOT "logs"
$P.SERVER_LOG   = Join-Path $P.ROOT "logs\server.log"
$P.MONITOR_LOG  = Join-Path $P.ROOT "logs\esp32_monitor.log"

# ---- 文档 ----
$P.DOCS_DIR      = Join-Path $P.ROOT "docs"
$P.PROBLEMS_MD   = Join-Path $P.ROOT "docs\PROBLEMS.md"        # 踩坑/问题总汇
$P.PLAN_MD       = Join-Path $P.ROOT "docs\STM32_WiFi_OTA_方案.md"  # 方案/接线/调试总文档

# 返回给调用方
return $P
