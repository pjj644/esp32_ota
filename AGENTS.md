# AGENTS.md - 项目总览与信息索引

ESP32-WROOM-32 双 OTA 测试工程：ESP32 自研固件 OTA + 经 UART（AN3155 协议）刷写 STM32F103
（GD32 克隆）。验证手段无测试/CI，全部靠**烧录 + 读串口日志**。

本文件只做"项目介绍 + 哪里有什么"。**遇到问题/踩坑先查 `docs/PROBLEMS.md`**。

## 顶层结构（harness 架构）

```
local_test/
├── AGENTS.md              本文件: 项目地图与查询索引
├── CMakeLists.txt         ESP-IDF 工程根 (idf.py 必须在仓库根构建, 勿移动)
├── sdkconfig.defaults     ESP32 配置 (CONFIG_APP_PROJECT_VER 版本号)
├── sdkconfig              构建生成, 版本不符需删(见 PROBLEMS.md §1.2), git 忽略
├── partitions.csv         ota_0/ota_1 各 1.9375MB, 无 factory
├── main/                  ESP32 应用源码 (main.c / wifi / http_client / ota / stm32_ota / led)
├── server/                node OTA 服务器 (0 依赖) + firmware/ 发布位
│   └── firmware/          version.json / stm32_version.json / firmware.bin / stm32.bin
├── stm32/esp32_test/      STM32 CubeMX CMake 工程 (Core/Src/main.c 含 APP_VERSION_STR)
├── config/
│   └── paths.ps1          全部路径/串口/工具链/版本文件 常量中心 (改配置只动这里)
├── scripts/               常用脚本 (见下)
├── docs/                  文档
│   ├── PROBLEMS.md        ★所有踩坑/问题总汇 (构建/OTA/服务器/STM32/DAPLink/OLED/日志)
│   ├── STM32_WiFi_OTA_方案.md   方案/接线/调试总文档
│   └── 学习路线.md
├── logs/                  运行日志 (server.log, esp32_monitor.log)
└── backup/                备份 (sdkconfig.bak/.old, legacy 脚本)
```

## 查询索引（遇到问题去哪查）

| 需要 | 去哪 |
|---|---|
| 踩坑/已知问题（构建失败、OTA 不生效、同步无 ACK、OLED 异常…） | `docs/PROBLEMS.md` |
| 接线图、调试步骤、协议细节 | `docs/STM32_WiFi_OTA_方案.md` |
| 所有路径、串口、IP、工具链路径 | `config/paths.ps1` |
| 发布/构建/烧录/监视脚本 | `scripts/`（见下） |
| 服务器 API/发布流程 | `server/README.md` |

## scripts/ 脚本（PowerShell 7，每个脚本配同名 .bat 双击包装器）

> Windows 默认执行策略 Restricted 禁止直接运行 .ps1——**双击 .bat 或运行
> `.\scripts\xxx.bat`**（内部 `pwsh -ExecutionPolicy Bypass`），或在 pwsh 里
> `Set-ExecutionPolicy -Scope Process Bypass` 后直接跑 .ps1。

| 脚本 | 功能 |
|---|---|
| `build_esp32.ps1` | 升版本(可 -Version 指定) → 编译 → 部署 firmware.bin + 同步 version.json（自动处理 sdkconfig 重生成坑） |
| `build_stm32.ps1` | 升版本 → cmake 编译 → objcopy → 部署 stm32.bin + 同步 stm32_version.json |
| `flash_esp32.ps1` | esptool 直烧 4 段（含 ota_data_initial.bin），自动杀残留 monitor |
| `flash_stm32.ps1` | openocd + DAPLink 直烧 STM32（不经 OTA） |
| `update_all.ps1` | 一键: 构建 ESP32+STM32 → 直烧两者 → 启动服务器 |
| `server_start.ps1` | 启动/重启 OTA 服务器（-Watch 实时跟踪日志） |
| `monitor_esp32.ps1` | 后台 idf_monitor 抓日志到 logs/（无人值守） |
| `common.ps1` | 共享函数库（dot-source，勿直接运行） |

所有脚本共享 `config/paths.ps1` 常量；每个脚本头部有用法注释。

## 关键硬事实（详见 PROBLEMS.md 对应章节）

- **版本必须"严格大于"才动作**，相等=跳过（每 60s 一行，像卡死）。验证等 ≥70s。
- **OTA 触发**: ESP32 = 三处同步(sdkconfig.defaults + version.json + 重新 build 部署)；
  STM32 = 版本 > 设备 NVS `stm32_ota/last_ver`，服务器实时读文件无需重启。
- **接线固定**:
  - OTA/UART: GPIO17→PA10(U1RX), GPIO16→PA9(U1TX), GPIO4→BOOT0, GPIO5→NRST, 共地。别复用。
  - OLED (I2C1): PB6→SCL, PB7→SDA (0x3C, 128x64 SSD1306)。
  - MPU6050 (I2C2): PB10→SCL, PB11→SDA (0x68, 6轴IMU, 互补滤波解算 Roll/Pitch/Yaw并在OLED实时显示)。
- **DAPLink VCOM TX 绝不能接 PA10**（PROBLEMS.md §5.1，最大的坑）。
- 串口: COM6=ESP32(CH340, 烧录460800/日志115200)，COM3=DAPLink VCOM(收 STM32 日志@115200)。
- PC IP（当前 192.168.1.11）改动需同步 `main/main.c` 的 LOCAL_HOST + `config/paths.ps1` 并重烧。
- 中文日志重定向后乱码 → 用 ASCII 关键词 grep（`manifest`、`200`、`TIMEOUT`、`heartbeat`）。
