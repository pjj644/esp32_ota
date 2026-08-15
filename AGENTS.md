# AGENTS.md

ESP32-WROOM-32 双 OTA 测试工程（自研固件 OTA + 经 UART 刷写 STM32F103）。验证手段无测试/CI，全部靠**烧录 + 读串口日志**。下面只列会踩坑的硬事实。

## 构建/烧录（Windows, 自定义 IDF 安装路径）

`idf.py` 不在 PATH，必须先手动设环境变量（无 export 脚本）：

```powershell
$env:IDF_PATH="D:\esp32\.espressif\v6.0.1\esp-idf"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\tools\python\v6.0.1\venv"
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
```

- 编译: `& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build`（工作目录 = 工程根）
- 烧录: esptool 手工写 4 段（板子是 40MHz DIO、4MB）:
  `esptool --chip esp32 -b 460800 -p COM4 write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m 0x1000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xd000 build\ota_data_initial.bin 0x10000 build\local_test.bin`
- 串口: COM4, 烧录 460800 / monitor 115200。设备无自动断电，改 `main.c` 后必须重新烧录。

### 后台抓日志（重要工具）

`idf build monitor` 交互式会卡 shell。要无人值守抓日志：
`Start-Process python idf_monitor.py -ArgumentList "-p","COM4","-b","115200","--timestamps","build\local_test.elf" -RedirectStandardOutput $log`，monitor 连接时会把设备复位（日志从 boot 开始）。

### 已知坑

- **COM4 被残留 monitor 占用** → 报 `PermissionError(13)`。先查并杀残留: `Get-CimInstance Win32_Process | ? CommandLine -match 'idf_monitor' | Stop-Process`。
- **中文日志在重定向文件里乱码**（GBK 控制台，如 `�汾���`）— 用 ASCII 关键词（`stm32_ota`、`Bootloader`、`同步`不适用…改用 `manifest`、`200`、`TIMEOUT`、`超时`）grep 即可。
- 抓日志要先确认 monitor 进程活着，再 `Start-Sleep` 等周期，避免空轮。

## OTA 触发机制（最容易误判"卡住"）

两类 OTA 都是"**版本必须严格大于才动作**"，相等=什么都不做（每 60s 只打一行跳过日志，看起来像卡死）：

- **检查周期**: 心跳 2s / /hello 10s / OTA 检查 60s（`ota_task` 与 `stm32_ota_task` 各自 60s）。验证要等 ≥70s。
- **触发 STM32 刷写**：改 `server/firmware/stm32_version.json` 版本号 **大于设备 NVS 记录**（namespace `stm32_ota`, key `last_ver`，上次成功刷写后已记录）。只改文件即可，服务器实时读，无需重启 node。
- **触发 ESP32 自身 OTA**：必须三处同步改：`sdkconfig.defaults` 的 `CONFIG_APP_PROJECT_VER` + `server/firmware/version.json`，且重新 build + 跑 `server/deploy_firmware.bat`（否则下载卡住）；烧录时含 `ota_data_initial.bin`。回滚已启用（`ota_confirm_running_app()` 在 WiFi 连通后调用）。
- 无 factory 分区（见 `partitions.csv`：ota_0/ota_1 各 1.9375MB，otadata 0x2000）。

## 服务器（node, 0 依赖）

- `cd server; node server.js`，端口 8888。`LOCAL_HOST` 硬编码在 `main/main.c`（当前 192.168.1.11），PC IP 变了要改 main.c 并重烧。
- `firmware/` 下: `version.json`（ESP32）、`stm32_version.json` + `stm32.bin`（STM32）。manifest 的 `url` 用请求 Host 头动态拼。
- Windows 防火墙须放行 node。服务器挂了设备端会显示 `local server unreachable: ESP_FAIL`。
- 偶发：一轮网络连接超时直接跳过该轮，下轮自动重试，属正常抖动，别当 bug 查。

## STM32 侧（GD32 克隆 F103）

- **克隆芯片的 ROM Bootloader 只在 9600 波特率能同步**，不是 AN3155 标准的 115200（`stm32_ota.h` 已写死 9600）。
- 协议坑（`stm32_ota.c` 注释已详述）：命令头 `[cmd][~cmd]`；地址 4 字节大端 + XOR 校验；写完 `Go 0x08000000`；页擦除 1KB/页。Get 应答含命令表 `79 0B 10 00 01 02 11 21 31 43 63 73 82 92 79`。
- 接线固定：GPIO17→PA10(U1RX), GPIO16→PA9(U1TX), GPIO4→BOOT0, GPIO5→NRST, 共地。**别复用 GPIO4/5/16/17**。
- STM32 固件是独立 CubeMX CMake 工程 `stm32/esp32_test/`，编译后用 `arm-none-eabi-objcopy -O binary esp32_test.elf esp32_test.bin` 生成 bin，再 `server/deploy_stm32.bat` 部署。
- 刷完 STM32 复位，串口打印 `APP vX.Y.Z boot` 即生效。
- **DAPLink 直连烧录（不经 OTA）**：openocd（IDF 工具链自带，`C:/Espressif/tools/openocd-esp32/v0.12.0-esp32-20260304/`）：
  `openocd -s <scripts> -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg -c "adapter speed 2000" -c "program stm32/esp32_test/build/Debug/esp32_test.elf verify reset exit"`
  DAPLink = `VID_C251&PID_F001`（HID=SWD 烧录，VCOM=**COM3** @115200 收 STM32 USART1 日志，验证过 `[OLED] ok @0x3C (hw-i2c) errs=0`）。
- 诊断探针保留在代码里：`stm32_ota_debug_probe()` / `stm32_ota_baud_probe()` / `stm32_ota_proto_probe()`（临时调试用，未接 app_main）。

## OLED（SSD1306, I2C1 = PB6/SCL, PB7/SDA）

- **F1 HAL 的 I2C `DevAddress` 必须传"左移一位的 8 位地址"（0x78 = 0x3C<<1）**。`I2C_7BIT_ADD_WRITE` 只清 bit0 不做移位（`stm32f1xx_hal_i2c.h:666`），HAL 文档写明 "must be shifted to the left"。误传 7 位 0x3C → 硬件 I2C 永远 NAK，曾被误判成"GD32 克隆硬件 I2C 坏"。
- **内容垂直重复的真正根因（v1.0.15 修复）**：`fonts.c` 的 `font_ascii_8x16` 每列 2 字节完全相同（8x8 列字节复制两份的错误编码），旧的 `DrawChar8x16` 把同一字节画到行 0-7 和行 8-15 → 每个字符出现两个叠置副本 = 整行文字上下重复。与屏/驱动/多路复用均无关（曾误查硬件 I2C、bit-bang、1/32 面板、预充电，全错）。修复：换用 LED3 的 `OLED_F8x16`（真 16px：前 8 字节=8 列上半、后 8 字节=8 列下半），`DrawChar8x16` 按 `glyph[col]` / `glyph[col+8]` 取上下字节。
- SSD1306 初始化：`0xA8 0x3F`（1/64）、`0xDA 0x12`（alternate COM）、`0x8D 0x14`（电荷泵）；`SSD1306_Update` 整屏 8 页。面板是标准 64 行（1/64 正常）。
- 地址约定：`SSD1306_t.addr` 存 7 位（0x3C/0x3D），硬件路径用 `addr<<1` 喂 HAL。
- 本地调试用 DAPLink + USART1 直连即可烧录/看串口，不必走 OTA 循环。

## 文档

根目录 `STM32_WiFi_OTA_方案.md`（中文）为方案/接线/调试总文档；`server/README.md` 有发布流程。