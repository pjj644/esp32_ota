# 问题总汇（踩坑记录 / Troubleshooting）

> 本项目所有已知坑按类别收录于此。遇到问题时按下面索引定位：
> 构建/烧录 → §1；OTA 机制 → §2；服务器/网络 → §3；STM32 侧/接线 → §4；
> DAPLink → §5；OLED → §6；日志/调试工具 → §7。
> 方案/接线总文档: `docs/STM32_WiFi_OTA_方案.md`；路径/常量中心: `config/paths.ps1`。

---

## 1. 构建 / 烧录

### 1.1 不要用 export.ps1 —— 必失败
IDF 官方 `export.ps1` 要求所有工具装齐，本机缺 riscv32-esp-elf*/dfu-util，会报
"no installed versions" 中断。正确做法：`scripts/build_esp32.ps1` 里手工设环境变量
（IDF_PATH / IDF_PYTHON_ENV_PATH / IDF_TOOLS_PATH / ESP_IDF_VERSION / ESP_ROM_ELF_DIR +
PATH 追加 xtensa/ninja/cmake/ccache/idf-exe），集中在 `config/paths.ps1`。

### 1.2 【大坑】改 sdkconfig.defaults 后构建出的是旧版本 → OTA 看起来永远失败
IDF "Defaults policy: sdkconfig"：**生成的 `sdkconfig` 一旦存在，sdkconfig.defaults 的改动
不再生效**（版本号停留在旧值）。表现：firmware.bin 内容与 version.json 不符 → 设备下载、
刷完还是旧版本 → 每 60s 循环下载，看着像"OTA 没成功"。
修复：构建前比较 `sdkconfig` 里的 `CONFIG_APP_PROJECT_VER` 与目标版本，不一致则备份
（`backup/sdkconfig.bak`）并删除强制重生成。`build_esp32.ps1` 已自动处理（`Sync-Sdkconfig`）。

### 1.3 idf_monitor 启动即崩（WinError 2）
PATH 里缺 xtensa 工具链 → `addr2line` 找不到 → monitor 进程秒退。
必须把 `C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin` 加进 PATH。
`scripts/monitor_esp32.ps1` 已处理。

### 1.4 烧录前串口被残留 monitor 占用 → PermissionError(13)
先杀残留再烧：`Get-CimInstance Win32_Process | ? CommandLine -match 'idf_monitor' | Stop-Process`。
`flash_esp32.ps1` 已自动处理。

### 1.5 烧录参数（板子固定）
esptool 4 段、40MHz DIO、4MB：
`0x1000 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin 0x10000 local_test.bin`
**烧录时必须含 ota_data_initial.bin**（重置 OTA 选择状态）。串口烧录 460800 / 日志 115200。

### 1.6 没装 idf.py 到 PATH
`idf.py` 不在 PATH。用 `$IDF_PYTHON_ENV\Scripts\python.exe $IDF_PATH\tools\idf.py build`。

### 1.7 【CubeMX 重导出丢源文件】ssd1306.c/fonts.c 链接失败
在 CubeMX 里重新生成代码/工程（或改 .ioc 保存）会**重写 `stm32/esp32_test/cmake/stm32cubemx/CMakeLists.txt`**，
手工添加的 `Core/Src/ssd1306.c`、`Core/Src/fonts.c`（不在 .ioc 清单里）会被丢弃 →
链接报 `undefined reference to SSD1306_*`。
修复：`git checkout -- stm32/esp32_test/cmake/stm32cubemx/CMakeLists.txt` 恢复两行源文件即可。
2026-08-16 实测一次（表现为 build_stm32.ps1 链接失败）。

---

## 2. OTA 机制（最易误判"卡住"）

### 2.1 版本必须"严格大于"才动作
两类 OTA 都只在**新版本号 > 当前版本**时才刷写；相等 = 什么都不做，每 60s 只打一行跳过日志
（看起来像卡死）。

### 2.2 检查周期
心跳 2s / /hello 10s / OTA 检查 60s（ota_task 与 stm32_ota_task 各自 60s）。
验证一次 OTA 至少要等 ≥70s。

### 2.3 触发 ESP32 OTA 的三处同步
`CONFIG_APP_PROJECT_VER`（sdkconfig.defaults）+ `server/firmware/version.json` + 重新 build
并部署 firmware.bin（否则下载卡住），三者必须一致。用 `scripts/build_esp32.ps1` 一条命令完成。

### 2.4 触发 STM32 刷写
改 `stm32/esp32_test/Core/Src/main.c` 的 `APP_VERSION_STR` + `server/firmware/stm32_version.json`，
版本须大于设备 NVS 记录（namespace `stm32_ota`, key `last_ver`）。服务器实时读文件，无需重启。
用 `scripts/build_stm32.ps1`。

### 2.5 分区布局
无 factory 分区：ota_0/ota_1 各 1.9375MB，otadata 0x2000（`partitions.csv`）。
回滚已启用：`ota_confirm_running_app()` 在 WiFi 连通后调用。

---

## 3. 服务器 / 网络

### 3.1 LOCAL_HOST 硬编码
`main/main.c` 的 `LOCAL_HOST`（当前 192.168.1.11）与 `config/paths.ps1` 的 `$P.LOCAL_HOST`。
PC IP 变了必须改两处并重烧 ESP32。

### 3.2 Windows 防火墙
必须放行 node。服务器挂了设备端显示 `local server unreachable: ESP_FAIL`。

### 3.3 manifest 的 url 用请求 Host 头动态拼
设备通过请求的 Host 头拿下载地址，DNS/IP 可达性由网络保证。

### 3.4 瞬时网络超时属正常
一轮 `ESP_ERR_HTTP_CONNECT` 直接跳过该轮，下轮自动重试（曾见连续 2 轮失败 + WiFi 掉线
`IP=0.0.0.0` reason=2/205，是路由器侧瞬时问题，别当 bug 查）。

---

## 4. STM32 侧 / 接线

### 4.1 接线固定（别复用这些 GPIO）
- GPIO17 → PA10 (U1RX)，GPIO16 → PA9 (U1TX)，GPIO4 → BOOT0，GPIO5 → NRST，共地。

### 4.2 克隆芯片（GD32 F103）ROM Bootloader 只在 9600 波特率能同步
不是 AN3155 标准的 115200（`stm32_ota.h` 写死 9600）。协议细节在 `stm32_ota.c` 注释：
命令头 `[cmd][~cmd]`；地址 4 字节大端 + XOR 校验；写后 `Go 0x08000000`；页擦除 1KB/页。
Get 应答含命令表 `79 0B 10 00 01 02 11 21 31 43 63 73 82 92 79`（同步成功标志）。

### 4.3 刷完 STM32 后串口打印 `APP vX.Y.Z boot` 即生效
验证可用 DAPLink VCOM（COM3 @115200 收 USART1 日志）或 openocd dump flash 找版本字符串。

### 4.4 stm32_ota.c 里的诊断探针
`stm32_ota_debug_probe()` / `stm32_ota_baud_probe()` / `stm32_ota_proto_probe()`（临时调试用，未接 app_main）。

---

## 5. DAPLink

### 5.1 【最大坑】DAPLink 的 VCOM TX 绝不能接 PA10（2026-08-16 实测）
曾导致 STM32 OTA 同步 3 次无 ACK 连续数小时。VCOM TX 与 ESP32 GPIO17（U2TXD）同线抢 PA10，
破坏 0x7F 同步帧 → bootloader 永不应答 → `同步无 ACK`。**VCOM 只用 RX（收 PA9 日志）**。
此坑曾连续误导：怀疑 dbg 竞态（删除 `stm32_dbg_task` 后仍失败）、怀疑 G16/G17 线松、
怀疑 BOOT0 无效——断开 VCOM TX 后一轮即成功。
（同步成功标志：`Get` 应答 `79 0B 10 00 01 02 11 21 31 43 63 73 82 92 79`，
擦除 15 页 → 写入 → Go → v1.0.19 生效，openocd dump 0x08000000 验证含 `APP v1.0.19`。）

### 5.2 stm32_dbg_task 竞态（已删除，保留记录）
遗留调试任务每 2.6s 把 UART2 重配为 115200 8N1，与 OTA 刷写的 9600 8E1 竞态，
同步帧波特率错误 → 无 ACK。且刷写期间会收到乱码字节（`STM32 says: ?????`）。
已于 2026-08-16 从 `main/main.c` 删除本任务。

### 5.3 openocd 用法
- 烧录: `openocd -s <scripts> -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg
  -c "adapter speed 2000" -c "program <elf> verify reset exit"`
- 读寄存器/验证: `init` → `halt` → `reg pc` → `mdw 0x08000000 4`
- **传给 openocd 的任何路径必须用正斜杠**（`C:/...`）——openocd 用 Tcl 解析，反斜杠被当
  转义符吞掉（`D:esp32espproject...`），`program`/`dump_image` 都会报 "couldn't open"。
  `scripts/flash_stm32.ps1` 已自动转换。
- **`reset halt` 可能超时**（"timed out while waiting for target halted"），用 `halt` 代替即可。
- openocd 输出带 ANSI 转义序列，解析输出前先 `-replace '\x1b\[[0-9;]*m',''`。
- 接口就绪时会打印 `nRESET = 0` 属正常，不代表复位被拉死。

### 5.4 DAPLink 直连烧录（不经 OTA）
DAPLink = `VID_C251&PID_F001`（HID=SWD 烧录，VCOM=COM3 @115200 收 STM32 USART1 日志）。
本地调试用 DAPLink + USART1 直连即可，不必走 OTA 循环。`scripts/flash_stm32.ps1` 封装。

---

## 6. OLED（SSD1306, I2C1 = PB6/SCL, PB7/SDA）

### 6.1 F1 HAL I2C 地址必须左移一位（0x78 = 0x3C<<1）
`I2C_7BIT_ADD_WRITE` 只清 bit0 不做移位（`stm32f1xx_hal_i2c.h:666`），HAL 文档写明
"must be shifted to the left"。误传 7 位 0x3C → 硬件 I2C 永远 NAK，曾被误判成
"GD32 克隆硬件 I2C 坏"。
地址约定：`SSD1306_t.addr` 存 7 位（0x3C/0x3D），硬件路径用 `addr<<1` 喂 HAL。

### 6.2 内容垂直重复的真正根因（v1.0.15 修复）
`fonts.c` 的 `font_ascii_8x16` 每列 2 字节完全相同（8x8 列字节复制两份的错误编码），
旧 `DrawChar8x16` 把同一字节画到行 0-7 和行 8-15 → 每字符两个叠置副本 = 整行上下重复。
与屏/驱动/多路复用无关（曾误查硬件 I2C、bit-bang、1/32 面板、预充电）。
修复：换用 LED3 的 `OLED_F8x16`（真 16px：前 8 字节=8 列上半、后 8 字节=8 列下半）。

### 6.3 SSD1306 初始化要点
`0xA8 0x3F`（1/64）、`0xDA 0x12`（alternate COM）、`0x8D 0x14`（电荷泵）；
`SSD1306_Update` 整屏 8 页。面板是标准 64 行（1/64 正常）。

---

## 7. 日志 / 调试工具

### 7.1 中文日志在重定向文件里乱码
GBK 控制台显示 UTF-8 → 如 `�汾���`。**用 ASCII 关键词 grep**：`manifest`、`200`、
`TIMEOUT`、`ota`、`heartbeat`、`UART` 等。

### 7.2 后台抓日志（无人值守）
`Start-Process python idf_monitor.py -ArgumentList "-p",COM6,"-b","115200","--timestamps",<elf>
-RedirectStandardOutput <log>`；monitor 连接时会把设备复位（日志从 boot 开始）。
用 `scripts/monitor_esp32.ps1`。抓日志前先确认 monitor 进程活着，再 Start-Sleep 等周期，避免空轮。

---

## 8. MPU-6050（I2C2 = PB10/SCL, PB11/SDA）与姿态解算

### 8.1 F1 HAL 地址与突发读取
MPU-6050 挂载于硬件 I2C2。默认地址为 0x68（AD0=GND）或 0x69（AD0=VCC）。
HAL 函数（`HAL_I2C_Mem_Read` / `HAL_I2C_Mem_Write`）中的 `DevAddress` 同样必须传入左移 1 位的 8 位地址（`0xD0` 或 `0xD2`）。
读取传感器数据时使用从 `0x3B (ACCEL_XOUT_H)` 开始的 14 字节单次突发传输（Burst Read），保证加速度、温度、陀螺仪数据的原子性与高吞吐。

### 8.2 newlib-nano 浮点 snprintf 必须加 `-u _printf_float`
GCC 链接参数使用了 `--specs=nano.specs` 时，默认为了减小固件体积移除了浮点格式化输出支持（`%f` 输出为空或格式错误）。
必须在 `stm32/esp32_test/CMakeLists.txt` 中添加：
`target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)`。

### 8.3 零偏校准与互补滤波
- 上电初始化时采集 100 组陀螺仪静态采样计算零偏 `gyro_bias`，消除静止漂移。
- 姿态角解算采用互补滤波（`alpha = 0.96`），融合加速度计低频重力向量与陀螺仪高频角速度积分，兼顾响应速度与抗动态震动能力。

