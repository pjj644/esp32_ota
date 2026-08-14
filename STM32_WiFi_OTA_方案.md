# ESP32 经 WiFi 给 STM32F103 刷固件 —— 设计方案与调试手册

> 目标：在本地电脑修改完 STM32 / ESP32 代码后，不拆机、不接 ST-Link，直接通过 ESP32 的
> WiFi 把新固件刷进对应芯片。
> - ESP32 自身固件：走已有 HTTP OTA（`main/ota.c`，双槽 + 回滚），**无需改动**。
> - STM32 固件：本方案新增。**复用 STM32F103 芯片 ROM 里自带的 USART 系统 Bootloader
>   （AN3155 协议）**，ESP32 作为主机通过 UART 写入 Flash。

---

## 1. 总体架构

```
PC (Node 服务器 :8888, server/server.js)
  ├─ GET /ota/manifest         ──► ESP32 固件 OTA（已有，不动）
  ├─ GET /ota/firmware.bin     ──► ESP32 固件下载（已有，不动）
  │
  ├─ GET /ota/stm32_manifest   ──► {"version":"1.0.0","url":"http://<pc>:8888/ota/stm32.bin","size":N}   (新增)
  └─ GET /ota/stm32.bin        ──► STM32 固件二进制，流式发送（新增）
                                    │
                                    ▼
                            ESP32 (WiFi STA)
                              │  ├─ GPIO4 ──► STM32 BOOT0   （刷写时拉高进入系统 Bootloader）
                              │  ├─ GPIO5 ──► STM32 NRST    （低脉冲 100ms 复位）
                              │  └─ UART2 GPIO17(TX)/GPIO16(RX)  ──► STM32 USART1 PA10(RX)/PA9(TX)
                              │         │ AN3155 协议：同步 0x7F → 擦除 0x43 → 写入 0x31 → Goto 0x21
                              │         ▼
                              │  STM32F103C8T6（芯片 ROM 系统 Bootloader，无需写任何 OTA 代码）
                              │  Flash 0x08000000  ← 新固件
                              ▼
                           完成：BOOT0 拉低 + 复位 → STM32 运行新固件
```

### 为什么选 ROM Bootloader（方案 A）
| 对比项 | A. ROM 系统 Bootloader（本方案） | B. 自定义 IAP Bootloader |
|---|---|---|
| STM32 代码改动 | **零改动**（可选加一行版本打印） | 需写完整 IAP 程序 + 链接脚本 + 中断向量重映射（F1 有坑） |
| 刷写工具链 | 芯片自带，标准 AN3155 协议 | 自研协议，要自己实现 Flash 驱动 |
| 写坏恢复 | 随时 BOOT0=1 复位即可重刷，**永不砖** | 若 IAP 区被破坏需 ST-Link 救砖 |
| 缺点 | 需接 BOOT0/NRST 两根杜邦线；无断点续传、无 CRC | 免接线，可自定义校验/断点续传 |

开发迭代阶段首选 A，稳定后若想省掉接线再考虑 B。

---

## 2. 原理简介：STM32F103 系统 Bootloader（AN3155）

1. **进入条件**：`BOOT1(PB2)=0`、`BOOT0=1`，然后复位 → 芯片执行 ROM 中的 USART Bootloader，
   从 **USART1（PA9 TX / PA10 RX）@ 115200 8N1** 监听命令。
2. **通信协议**（AN3155）：
   - 同步：主机发 `0x7F` → Bootloader 回 `0x79`(ACK) 或 `0x1F`(NACK)。
   - 命令帧：`[命令码][参数...][校验和]`，校验和 = 前面所有字节的 **XOR**。
   - 本次用到的命令：
     - `0x00` Get —— 读固件版本 + 支持命令表（调试用）
     - `0x43` Erase —— 擦除 Flash（F103 中容量：**每页 1KB**，逐页擦除最稳妥）
     - `0x31` Write —— 写入，**每块 ≤ 256 字节**，块内地址需 4 字节对齐，每块等 ACK
     - `0x21` Go —— 跳到指定地址执行（`0x08000000` 启动应用）
3. **写入速度**：115200 ≈ 11.5 KB/s，256B/块 + ACK 往返；32KB 固件约 5~10 秒。
4. **地址**：应用区起始 `0x08000000`，Flash 总容量 64KB（F103C8T6）。

---

## 3. 杜邦线接线（重点）

| ESP32 (WROOM-32) | 引脚功能 | STM32 (F103C8T6 最小系统) | 说明 |
|---|---|---|---|
| **GPIO17** | UART2 TXD | **PA10** (USART1_RX) | 串口交叉：ESP TX → STM RX |
| **GPIO16** | UART2 RXD | **PA9** (USART1_TX) | 串口交叉：ESP RX → STM TX |
| **GPIO4** | BOOT0 控制 | **BOOT0** | 刷写时拉高；板上已有 10K 下拉，默认低=正常启动 |
| **GPIO5** | NRST 控制 | **NRST** | 低脉冲 100ms 触发复位 |
| **GND** | 地 | **GND** | **必须共地，否则 UART 无法通信** |
| — | — | PB2 (BOOT1) | 保持低（板上默认下拉/跳线默认），不接 |

**接线注意：**
1. 两板**都断电**后再接线，最后接 GND。
2. 全部为 3.3V 电平，**直接杜邦线跨接即可**，无需电平转换、无需串联电阻。
3. BOOT0 板上自带下拉，平时（ESP32 上电默认 GPIO4 输出低）STM32 正常启动应用，不影响原有功能。
4. 若你的 WROOM-32 模块 PSRAM 占用了 GPIO16/17（WROVER 才可能），换空闲引脚并在代码里改宏。
5. 建议把 UART2 三线 + BOOT0 + NRST + GND 做成一段 6P 排线，方便插拔。

---

## 4. 需要改动的文件清单

### 4.1 服务器端（server/）
| 文件 | 改动 |
|---|---|
| `server/server.js` | 新增 `GET /ota/stm32_manifest`（读 `firmware/stm32_version.json`）与 `GET /ota/stm32.bin`（流式发送） |
| `server/deploy_stm32.bat` | **新建**：把 STM32 编译产物 bin 复制到 `firmware/stm32.bin`，提示更新版本 json |
| `server/firmware/stm32_version.json` | **新建**：`{"version":"1.0.0"}`（发布时手动改） |
| `server/README.md` | 补充 STM32 端点说明（可选） |

### 4.2 ESP32 端（main/）
| 文件 | 改动 |
|---|---|
| `main/stm32_ota.h` | **新建**：对外 API + 引脚/超时宏 |
| `main/stm32_ota.c` | **新建**：UART2 驱动、GPIO4/5 控制、AN3155 协议、下载+刷写流程 |
| `main/main.c` | `nvs_flash_init()` + 新建 `stm32_ota_task`（每 60s 检查） |
| `main/CMakeLists.txt` | 注册 `stm32_ota.c` |

### 4.3 STM32 端（stm32/esp32_test/）—— 可选
| 文件 | 改动 |
|---|---|
| `Core/Src/main.c` | USER CODE 区：启动时 `printf` 一行 `APP vX.Y.Z`，便于刷写后确认新固件生效（依赖 `usart1_printf` 重定向，或 `HAL_UART_Transmit` 直接发） |

> STM32 **不改也能刷**。版本判断用 ESP32 自己的 NVS（`last_stm32_ver`），不依赖 STM32 上报。

### 4.4 ESP32 刷写流程（状态机）
```
stm32_ota_task 每 60s：
  1. GET /ota/stm32_manifest  → 解析 version/url/size
  2. NVS 读 last_stm32_ver；semver 比较，无新版 → 本轮结束
  3. 下载 stm32.bin 到堆内存（校验 size ≤ 64KB 且剩余堆足够，缺一不可）
  4. 进 Bootloader：GPIO4=1（BOOT0）→ GPIO5 低 100ms 复位 → 释放 → 等 200ms
  5. 同步：发 0x7F，等 ACK（失败重试 3 次）
  6. Get(0x00)：打印 Bootloader 版本与命令表（调试定位用）
  7. 擦除：0x43 逐页擦除 ceil(size/1024) 页，等 ACK
  8. 写入：0x31 按 256B/块 + XOR 校验 + 等 ACK，打印进度 %（每 10%）
  9. Go(0x21) → 0x08000000（不依赖 ACK）
 10. GPIO4=0（BOOT0）→ GPIO5 低 100ms 复位 → 等 1s 让应用启动
 11. NVS 写 last_stm32_ver = manifest 版本，打印完成
失败 → 日志打印失败阶段 + 错误码，NVS 不更新，下轮自动重试
```

---

## 5. 固件生成与发布流程（STM32）

```powershell
# 1. 修改 STM32 代码后编译（本机已装 arm-none-eabi 工具链）
cd D:\esp32\esp_project\local_test\stm32\esp32_test
cmake --preset ... 或按现有 CMakePresets 配置构建
cmake --build build

# 2. 生成纯二进制（.elf → .bin），或后续在 CMakeLists 加 POST_BUILD 自动生成
arm-none-eabi-objcopy -O binary build/esp32_test.elf stm32.bin

# 3. 部署到服务器固件目录
D:\esp32\esp_project\local_test\server\deploy_stm32.bat

# 4. 手动更新 firmware/stm32_version.json 版本号（如 "1.0.1"）
```

**注意**：`stm32_version.json` 版本号 > ESP32 NVS 里记录的上次版本时才会触发刷写，
与 ESP32 OTA 的"防反复刷写"机制一致。

---

## 6. 调试步骤（分阶段，每阶段可独立验证）

### 阶段 0：服务器端点自测（无需硬件）
```powershell
cd D:\esp32\esp_project\local_test\server
node server.js
# 另开一个窗口：
curl http://<电脑IP>:8888/ota/stm32_manifest
curl -o test.bin http://<电脑IP>:8888/ota/stm32.bin   # 检查文件大小与原 bin 一致
```

### 阶段 1：验证 STM32 能进系统 Bootloader（先不接 ESP32）
1. STM32 板上 BOOT0 跳线帽拨到 **1**（接 3.3V）。
2. 按复位键（或断电重上电）。
3. 用 **STM32CubeProgrammer → UART 模式**（或任意串口助手）连 USART1：
   - CubeProgrammer：选 115200，点 Connect，能读到 Chip ID `0x410` 即成功。
   - 串口助手：发 `7F`，收到 `79` 即同步成功。
4. 验证后把 BOOT0 跳线拨回 **0** 复位，恢复应用启动。
   > 阶段 1 通过 = 这块板 ROM Bootloader 正常，问题只会出在 ESP32 侧的时序/接线。

### 阶段 2：接好杜邦线，ESP32 只读联调（不擦不写）
1. 按第 3 节接线。
2. ESP32 先实现：UART2 初始化 + GPIO4/5 + `同步(0x7F)` + `Get(0x00)`。
3. 观察 ESP32 日志：
   - 期望：`STM32 OTA: sync OK (ACK)`、`bootloader vX.Y`、命令表 `[00 01 02 11 21 31 43]`。
   - 失败：超时无 ACK → 查接线（交叉、共地）、GPIO4 是否真为高、NRST 复位是否生效。
   - 建议此阶段在日志里打印 BOOT0/NRST 电平，用万用表实测 GPIO4 高、复位瞬间 GPIO5 低。
4. 可选再验证：`Read Memory (0x11)` 读 `0x08000000` 前 64 字节，与 ST-Link 读出内容比对。

### 阶段 3：首次完整刷写（对照验证）
1. 用 ST-Link 把当前 `stm32.bin` 烧进 STM32，并**用 ST-Link 读回保存一份**作为基准。
2. 再走一次 **读模式**（不进 bootloader）：GPIO4=0 复位，确认应用正常运行。
3. 用 ESP32 走完整流程：进 Bootloader → 同步 → 擦除 → 写入 → Go → 复位。
4. 验证：ST-Link 读回 Flash，与基准 bin `fc /b` 对比（或用 HxD 对比）一致；应用行为正常。
   > 刷写中途失败也先别慌：断电/复位后 ESP32 下轮会自动重刷，STM32 不会砖。

### 阶段 4：WiFi 端到端（日常工作流）
1. 修改 STM32 代码（改版本号打印），编译 → `deploy_stm32.bat` → 改 `stm32_version.json`。
2. 观察 ESP32 日志：manifest 拉取 → 版本比较 → 下载 → 刷写进度 → `STM32 OTA done vX.Y.Z`。
3. STM32 串口/行为确认新固件生效。
4. 回归：验证 ESP32 自身 OTA（`firmware.bin`）流程不受影响。

### 阶段 5：恢复性测试（写坏恢复）
1. 刷写过程中拔掉 STM32 电源（模拟断电），或人为把 stm32.bin 传坏。
2. 重新上电，ESP32 下一轮检测会重新擦写；确认 STM32 最终恢复运行新固件。

---

## 7. 故障排查表

| 现象 | 可能原因 | 排查 |
|---|---|---|
| ESP32 拉 manifest 失败 | PC IP / 防火墙 / 服务器未启动 | 与 ESP32 OTA 同一排查路径（见 server/README.md） |
| 同步 0x7F 无响应（无 ACK） | 接线交叉错、未共地、BOOT0 没拉高、复位没生效 | 万用表量 GPIO4 是否高电平、NRST 是否出现低脉冲；用 CubeProgrammer 手动确认阶段 1 通过 |
| 同步有响应，但擦除/写入超时 | 块大小 > 256B、地址未 4 字节对齐、bin 大小算错 | 检查 0x31/0x43 帧的 XOR 校验和实现 |
| 写入成功但应用不运行 | Go 地址错误 / BOOT0 复位后仍为高 | Go 必须是 0x08000000；写完后 GPIO4 必须拉低再复位 |
| 固件反复刷写 | NVS `last_stm32_ver` 没写进去 | 检查 NVS 读写返回值，首次运行要先 `nvs_flash_init()` |
| 刷完变砖（无反应） | 断电中断/固件本身坏 | 不用 ST-Link：BOOT0 由 ESP32 控制，断电重启后 ESP32 自动重刷即可恢复 |

---

## 8. 注意事项与风险

1. **刷写窗口**：擦除→写入完成之间**不要给 STM32 断电**（半写固件不可启动，但可恢复重刷）。
2. **USART1 复用**：刷写占用 USART1（PA9/PA10），与应用共用同一组引脚；ESP32 的 `stm32_ota_task`
   仅在需要刷写时驱动该串口，其余时间任务休眠，不影响应用使用。
3. **明文 HTTP**：与 ESP32 OTA 一样基于 HTTP，仅限学习/局域网；生产需 HTTPS + 服务器校验。
4. **版本一致性**：`stm32_version.json` 由人工维护，务必与 STM32 固件实际版本一致，否则会
   出现"版本没变但内容变了 / 版本变了内容没变"的混乱。
5. **NVS 依赖**：ESP32 需初始化 `nvs_flash`（分区表中已有 nvs 分区）；若换了一块 STM32 板，
   可手动清空 ESP32 的 `last_stm32_ver` 强制重刷。
