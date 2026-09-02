# ESP32 & STM32 双路 OTA 固件升级系统

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0+-blue.svg)](https://github.com/espressif/esp-idf)
[![STM32](https://img.shields.io/badge/STM32-HAL%20F103-blue.svg)](https://www.st.com/)
[![Node.js](https://img.shields.io/badge/Node.js-Server%20(Zero--deps)-green.svg)](https://nodejs.org/)
[![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)](LICENSE)

本项目是一个功能完备、开箱即用的**主从多 MCU 双路 OTA（Over-The-Air）无线固件升级系统**。

在许多嵌入式物联网产品中，主控通常分为通信 MCU（如具备 WiFi 的 ESP32）和低功耗/实时控制从 MCU（如 STM32）。传统的 OTA 方案往往只升级 ESP32 本身，STM32 仍需预留外部烧录口（SWD/串口）。本项目通过 ESP32 充当无线网关，配合硬件引脚电平控制与串口协议透传，实现了**对 ESP32 本身以及对 STM32 的全无线远程固件升级**。

---

## 🌟 核心特性

- 📶 **ESP32 自身 WiFi OTA**：
  - 基于 ESP-IDF 双分区（`ota_0` / `ota_1` 各 1.9375MB，见 `partitions.csv`）乒乓无缝热切换。
  - 具备固件自检与安全回滚保护（Rollback Protection），升级异常自动撤销，防止变砖。
- ⚡ **STM32 无侵入式 WiFi-to-UART OTA (AN3155)**：
  - **零代码侵入**：STM32 应用端无需移植任何 Bootloader 或 Flash 擦写代码，固件完全跑在裸机/标准应用区。
  - **引脚接管进系统 ROM**：ESP32 通过控制 STM32 的 `BOOT0` 与 `NRST` 引脚，模拟人工按键，强制让 STM32 进入芯片自带的 System ROM Bootloader。
  - **AN3155 协议驱动**：ESP32 内部实现完整的 ST AN3155 串口协议机，支持自动 0x7F 握手、扇区擦除、256B 分块写入校验与 Go 命令跳转。
  - **克隆芯片深度兼容**：针对市场上常见的 GD32/国产克隆 F103 芯片实测优化（9600 波特率 8E1、双阶段握手容错）。
  - **NVS 状态防重复刷写**：ESP32 本地 NVS 记录 STM32 上次成功刷写版本，未发布新版本时跳过擦写，避免 Flash 寿命磨损。
- 🎯 **STM32 传感器与姿态解算示例应用**：
  - **6 轴 IMU (MPU-6050)**：硬件 I2C2 挂载，开机上电静止 100 组采样自动校准陀螺仪零偏，一阶互补滤波解算欧拉角（Roll / Pitch / Yaw）。
  - **0.96 寸 OLED (SSD1306)**：硬件 I2C1 挂载，真 16px 点阵字体实时显示运行时间、姿态数据与固件版本。
  - **串口日志反馈**：USART1 上电打印 `APP vX.Y.Z boot`，直观验证 OTA 刷写生效。
- 🚀 **零依赖本地 OTA HTTP 服务器**：
  - 基于 Node.js 原生 API 编写，无任何第三方 npm 依赖。
  - 支持 `/hello` 连通性测试、`/ota/manifest`、`/ota/stm32_manifest` 动态下发固件元数据及二进制流式传输。
  - 自动通过 HTTP 请求 `Host` 字段反向拼接下载 URL，局域网切换无需改服务端代码。
- 🛠️ **全自动化工程工具链 (PowerShell 7 + 双击 .bat)**：
  - `config/paths.ps1` 统一常量管理中心（统一配置 IP、串口、编译工具链）。
  - 自动 SemVer 版本自增（patch+1）、自动编译、ELF 提取 BIN、自动拷贝发布并同步 JSON 清单。

---

## 📐 硬件接线与拓扑

### 1. 整体通信拓扑

```
+------------------+          WiFi (HTTP)           +---------------------+
| 本地 OTA 服务器  | <----------------------------> |   ESP32-WROOM-32    |
| (PC Node.js)     |                                | (WiFi 网关 + OTA)   |
+------------------+                                +----------+----------+
                                                               |
                                   GPIO4 (BOOT0) / GPIO5 (NRST)| 硬件控制
                                   GPIO17 (TX) / GPIO16 (RX)   | UART2 (9600 8E1)
                                                               v
                                                    +---------------------+
                                                    |  STM32F103 / GD32   |
                                                    | (从机应用 + 姿态)   |
                                                    +----+-----------+----+
                                                         |           |
                                               I2C1 (PB6/PB7)     I2C2 (PB10/PB11)
                                                         v           v
                                                    +---------+ +---------+
                                                    | SSD1306 | | MPU6050 |
                                                    | 0.96 OLED| | 6轴IMU |
                                                    +---------+ +---------+
```

### 2. 引脚连接明细表

#### (1) ESP32 与 STM32 互联（OTA 烧写与串口通信）

| ESP32 引脚 | STM32 引脚 | 信号方向 | 说明 |
|---|---|---|---|
| **GPIO17 (U2TXD)** | **PA10 (USART1_RX)** | ESP32 → STM32 | OTA 烧写数据与命令发送 |
| **GPIO16 (U2RXD)** | **PA9 (USART1_TX)** | STM32 → ESP32 | Bootloader 应答与 ACK 接收 |
| **GPIO4** | **BOOT0** | ESP32 → STM32 | 高电平使 STM32 进入 ROM Bootloader |
| **GPIO5** | **NRST** | ESP32 → STM32 | 低电平脉冲硬件复位 STM32 |
| **GND** | **GND** | 双向 | **必须共地** |

> ⚠️ **【高能预警 / 踩坑警告】**：
> 如果使用 DAPLink 查看 STM32 串口日志，**DAPLink 的 VCOM TX 绝不能连接 PA10**！
> 否则 DAPLink VCOM TX 会和 ESP32 GPIO17 抢线拉扯电平，导致 STM32 ROM 永远收不到干净的 `0x7F` 同步帧，报 `同步无 ACK` 错误（详见 `docs/PROBLEMS.md` §5.1）。DAPLink 仅保留 RX 接收 PA9 日志即可。

#### (2) STM32 外设连接

| 外设 | 外设引脚 | STM32 引脚 | 接口 | 默认 I2C 地址 (7-bit / 8-bit) |
|---|---|---|---|---|
| **SSD1306 OLED** | SCL / SDA | **PB6 / PB7** | 硬件 I2C1 | `0x3C` (HAL 传 `0x78`) |
| **MPU-6050** | SCL / SDA | **PB10 / PB11** | 硬件 I2C2 | `0x68` (HAL 传 `0xD0`) |
| **DAPLink / 调试串口** | SWD / VCOM RX | SWDIO / SWCLK / **PA9** | SWD + USART1 | 115200 8N1 打印系统日志与版本横幅 |

---

## 🔄 STM32 OTA 刷写时序与原理

```
ESP32 (ota_manager_task)                    STM32 (ROM Bootloader)
         |                                             |
         |-- 1. GET /ota/stm32_manifest ------------->| (PC 服务器)
         |<-- 返回 version, size, url -----------------|
         |                                             |
         |-- 2. 对比本地 NVS 记录的上次版本            |
         |   (若版本相等直接跳过; 若版本更新则继续)    |
         |                                             |
         |-- 3. HTTP 流式下载 bin 固件到堆内存 --------|
         |                                             |
         |-- 4. GPIO4(BOOT0)=高电平 ------------------>|
         |-- 5. GPIO5(NRST) 低电平脉冲 (100ms) ------->| 芯片硬件复位
         |      (等待 200ms 等待 ROM 启动就绪)         | 此时进入系统 ROM Bootloader
         |                                             |
         |-- 6. 发送 0x7F 同步帧 --------------------->|
         |<-- 应答 ACK (0x79) ------------------------| (9600 波特率)
         |                                             |
         |-- 7. 发送 Get 命令 (0x00) ----------------->|
         |<-- 返回芯片支持的命令列表 -----------------|
         |                                             |
         |-- 8. 擦除页命令 (0x43) -------------------->|
         |<-- 擦除完成 ACK (0x79) --------------------|
         |                                             |
         |-- 9. 循环 256 字节分包写 (0x31 + XOR 校验) ->| 逐块写入 Flash
         |<-- 每包应答 ACK (0x79) --------------------| (进度 10%...100%)
         |                                             |
         |-- 10. 发送 Go 命令 (0x21, 地址 0x08000000)->|
         |-- 11. GPIO4(BOOT0)=低电平 ----------------->|
         |-- 12. GPIO5(NRST) 复位脉冲 ---------------->| 芯片重启进入全新用户 APP
         |                                             | 串口打印: [STM32] APP vX.Y.Z boot
         |-- 13. 更新 ESP32 NVS 中的 last_ver ---------|
```

---

## 📂 仓库目录结构

```
esp32_ota/
├── CMakeLists.txt              # ESP-IDF 顶层 CMakeLists
├── sdkconfig.defaults          # ESP32 默认配置 (含 CONFIG_APP_PROJECT_VER)
├── partitions.csv              # 双 OTA 分区表定义 (ota_0 / ota_1 各 1.9375MB)
│
├── main/                       # ESP32 固件源码 (C / ESP-IDF)
│   ├── main.c                  # 系统入口、心跳、自检信息与网络任务
│   ├── ota_manager.c/.h        # 双 OTA 统一管理者 (一键启动 ESP32 + STM32 轮询)
│   ├── ota.c/.h                # ESP32 自身 OTA 核心逻辑 (esp_https_ota, 回滚确认)
│   ├── stm32_ota.c/.h          # STM32 AN3155 协议驱动 (引脚控制、协议帧、Flash 擦写)
│   ├── wifi.c/.h               # WiFi STA 初始化与事件处理
│   ├── http_client.c/.h        # HTTP GET 客户端工具库
│   └── led.c/.h                # 板载 LED 状态指示
│
├── stm32/esp32_test/           # STM32 固件源码 (STM32CubeMX + CMake + GCC)
│   ├── Core/
│   │   ├── Src/main.c          # STM32 应用主循环 (APP_VERSION_STR 版本定义)
│   │   ├── Src/mpu6050.c/.h    # MPU6050 驱动 (I2C2、零偏校准、互补滤波姿态解算)
│   │   └── Src/ssd1306.c/.h    # SSD1306 OLED 驱动 (I2C1、图形与 8x16 点阵渲染)
│   └── esp32_test.ioc          # STM32CubeMX 工程配置文件
│
├── server/                     # 本地 OTA HTTP 服务器 (Node.js 零依赖)
│   ├── server.js               # 服务端入口 (处理 /hello, /ota/manifest 等路由)
│   └── firmware/               # 固件发布目录
│       ├── version.json        # ESP32 当前发布版本号
│       ├── firmware.bin        # ESP32 待升级二进制固件
│       ├── stm32_version.json  # STM32 当前发布版本号
│       └── stm32.bin           # STM32 待升级二进制固件
│
├── config/
│   └── paths.ps1               # 统一配置中心 (串口号/IP/路径/工具链常量)
│
├── scripts/                    # 自动化工作流脚本 (PowerShell 7 + .bat 包装器)
│   ├── build_esp32.ps1/.bat    # 升版本 + 编译 ESP32 + 部署 firmware.bin + 同步 JSON
│   ├── build_stm32.ps1/.bat    # 升版本 + 编译 STM32 + 提取 BIN + 部署 + 同步 JSON
│   ├── flash_esp32.ps1/.bat    # 本地 USB 直烧 ESP32 (esptool)
│   ├── flash_stm32.ps1/.bat    # 本地 DAPLink 直烧 STM32 (openocd)
│   ├── update_all.ps1/.bat     # 一键构建两端、烧录并启动服务
│   ├── server_start.ps1/.bat   # 启动/重启 OTA 服务器
│   └── monitor_esp32.ps1/.bat  # 后台串口监视与日志捕捉
│
└── docs/                       # 核心技术文档
    ├── PROBLEMS.md             # ★ 详尽避坑宝典 (硬件/协议/工具链踩坑合集)
    ├── STM32_WiFi_OTA_方案.md  # 详细方案设计与协议时序解析
    └── 学习路线.md             # 渐进式学习指南与自测清单
```

---

## ⚡ 快速上手指南

### 1. 环境准备

确保开发主机已安装以下环境：
- **ESP-IDF** (建议 v5.1+ 或 v6.x)
- **ARM GNU Toolchain** (`arm-none-eabi-gcc`, `arm-none-eabi-objcopy`)
- **OpenOCD** (如需使用 DAPLink 本地烧录)
- **Node.js** (>= 18.x)
- **PowerShell 7** (`pwsh`)

### 2. 配置与参数设定

所有跨平台路径与本地硬件参数统一收拢在 `config/paths.ps1` 中：

1. **设定电脑局域网 IP 与 WiFi**：
   - 打开 `main/main.c`，修改 `LOCAL_HOST` 为电脑当前局域网 IP（如 `192.168.1.11`）：
     ```c
     #define LOCAL_HOST "192.168.1.11"
     ```
   - 同步修改 `config/paths.ps1` 中的 `$P.LOCAL_HOST`。
   - 打开 `main/wifi.h`，配置你的 WiFi 名称与密码：
     ```c
     #define WIFI_SSID "YOUR_WIFI_SSID"
     #define WIFI_PASS "YOUR_WIFI_PASSWORD"
     ```

2. **配置串口号**（在 `config/paths.ps1` 中）：
   ```powershell
   $P.ESP32_COM   = "COM6"   # ESP32 调试/烧录串口
   $P.DAPLINK_COM = "COM3"   # DAPLink VCOM 串口 (用于接收 STM32 PA9 启动日志)
   ```

### 3. 启动本地 OTA 服务器

进入 `server/` 目录或直接双击 `scripts/server_start.bat`：

```powershell
.\scripts\server_start.bat
```

控制台将输出：
```
[esp32-test-server] listening on http://0.0.0.0:8888
```

可通过浏览器或 `curl` 测试连通性：
```bash
curl http://192.168.1.11:8888/hello
```

### 4. 初始烧录

首次烧录可以通过脚本一键编译并烧入两颗芯片：

```powershell
# 编译并烧录 ESP32
.\scripts\build_esp32.bat
.\scripts\flash_esp32.bat

# 编译并烧录 STM32 (通过 DAPLink)
.\scripts\build_stm32.bat
.\scripts\flash_stm32.bat
```

---

## 🚀 无线 OTA 升级实战

在初次烧录完成并接好排线后，后续固件更新**完全脱离烧录器，全部通过 WiFi 无线触发**！

### 场景 A：发布与升级 STM32 固件

1. **修改代码并构建发布**：
   在 STM32 工程中修改逻辑（或保持不变），运行：
   ```powershell
   # 自动递增版本号 (如 1.0.1 -> 1.0.2)，自动编译并发布至 server/firmware/
   .\scripts\build_stm32.bat

   # 或显式指定版本号发布：
   .\scripts\build_stm32.ps1 -Version 1.1.0
   ```
2. **等待 ESP32 自动探测并刷写**：
   - ESP32 的 `ota_manager` 每 60s 轮询一次 `/ota/stm32_manifest`。
   - 发现远端版本高于本地 NVS 记录时，ESP32 自动拉取 `stm32.bin`，拉低复位进 Bootloader，9600 波特率擦写 Flash。
3. **验证生效**：
   - 观察 ESP32 串口日志输出：`写入进度 10%...100%` -> `STM32 OTA 成功`。
   - 观察 DAPLink 串口（115200 波特率）或 OLED 屏幕，输出最新版本号：
     ```text
     [STM32 esp32_test] APP v1.0.2 boot
     ```

### 场景 B：发布与升级 ESP32 自身固件

1. **构建并发布新固件**：
   ```powershell
   # 自动提升 sdkconfig.defaults 版本号、编译并部署 firmware.bin 与 version.json
   .\scripts\build_esp32.bat
   ```
2. **自动升级与重启**：
   - ESP32 检查到版本更新后，流式写入空闲的 OTA 分区。
   - 写入并校验通过后自动重启切换分区，运行新固件。
   - WiFi 连接成功后自动调用 `ota_confirm_running_app()` 确认生效，防止回滚。

---

## 💡 关键排坑与实测经验 (从 PROBLEMS.md 精选)

本项目在实机联调过程中沉淀了大量硬件底层细节与经验，强烈建议在遇到异常时参阅 `docs/PROBLEMS.md`：

1. **DAPLink VCOM TX 绝不能接 PA10**：
   DAPLink 的串口 TX 若连在 PA10 上，会与 ESP32 的 GPIO17 冲突抢线，导致电平拉扯破坏 `0x7F` 握手信号，表现为 STM32 OTA `同步 3 次无 ACK`。**DAPLink 仅保留 RX 接 PA9 接收日志即可**。
2. **GD32 / 克隆 F103 ROM Bootloader 波特率陷阱**：
   ST 官方 AN3155 协议标注支持更高波特率，但实测国产克隆 F103 芯片在系统内部 RC 振荡器时钟下，**仅在 9600 波特率 (8E1 偶校验) 下才能稳定同步**。`stm32_ota.c` 已默认采用 9600。
3. **STM32 HAL I2C 7位/8位地址左移**：
   使用 `HAL_I2C_Mem_*` 函数时，传入的 `DevAddress` 必须是**左移一位后的 8 位地址**（例如 SSD1306 为 `0x3C << 1 = 0x78`，MPU6050 为 `0x68 << 1 = 0xD0`）。直接传 7 位地址会导致 HAL 永远返回 NAK。
4. **GCC newlib-nano 浮点打印问题**：
   若使用 `--specs=nano.specs`，`snprintf` 默认不支持 `%f` 浮点格式化。必须在 CMake 中添加链接选项：
   ```cmake
   target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
   ```
5. **版本号规则（严格大于）**：
   OTA 逻辑中新版本号必须**严格大于（>）当前版本**才会触发刷写。版本相等时会直接跳过并输出日志，避免重复磨损 Flash。

---

## 📚 延伸阅读

- [docs/PROBLEMS.md](docs/PROBLEMS.md) —— 所有已知踩坑、底层硬件排障与实测记录清单。
- [docs/STM32_WiFi_OTA_方案.md](docs/STM32_WiFi_OTA_方案.md) —— AN3155 协议帧格式、时序要求与详细设计方案。
- [docs/学习路线.md](docs/学习路线.md) —— 渐进式源码走读路线图与实验自测清单。
- [AGENTS.md](AGENTS.md) —— 项目地图与自动化 Harness 索引。

---

## 📄 License

本项目遵循 MIT 开源许可证。