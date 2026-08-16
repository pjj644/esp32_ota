# ESP32 经 WiFi 给 STM32F103 刷固件 —— 设计方案与调试手册

> 目标：在本地电脑修改完 STM32 / ESP32 代码后，不拆机、不接 ST-Link，直接通过 ESP32 的
> WiFi 把新固件刷进对应芯片。
> - ESP32 自身固件：走已有 HTTP OTA（`main/ota.c`，双槽 + 回滚）。
> - STM32 固件：**复用 STM32F103 芯片 ROM 里自带的 USART 系统 Bootloader（AN3155 协议）**，
>   ESP32 作为主机通过 UART 写入 Flash。
> - **本板为 GD32F103 克隆**：ROM Bootloader 只在 **9600 波特率**能同步（不是 AN3155 标准的 115200）。

**状态（2026-08-16 实测）**：方案已全部落地并验证——ESP32 v1.0.8 自身 OTA 正常；
STM32 经 OTA 刷写 v1.0.19 成功（同步 → 擦除 15 页 → 写入 14368B → Go → 复位 → 应用启动）。
**最大坑**：DAPLink 的 VCOM TX 绝不能接 PA10（详见 §7.2 与 `docs/PROBLEMS.md` §5.1）。

---

## 1. 总体架构

```
PC (Node 服务器 :8888, server/server.js)
  ├─ GET /ota/manifest         ──► ESP32 固件 OTA（双槽 + 回滚）
  ├─ GET /ota/firmware.bin     ──► ESP32 固件下载
  │
  ├─ GET /ota/stm32_manifest   ──► {"version":"1.0.x","url":"http://<pc>:8888/ota/stm32.bin","size":N}
  └─ GET /ota/stm32.bin        ──► STM32 固件二进制，流式发送
                                    │
                                    ▼
                            ESP32 (WiFi STA)
                              │  ├─ GPIO4 ──► STM32 BOOT0   （刷写时拉高进入系统 Bootloader）
                              │  ├─ GPIO5 ──► STM32 NRST    （低脉冲 100ms 复位）
                              │  └─ UART2 GPIO17(TX)/GPIO16(RX)  ──► STM32 USART1 PA10(RX)/PA9(TX)
                              │         │ AN3155 协议：同步 0x7F → 擦除 0x43 → 写入 0x31 → Go 0x21
                              │         ▼
                              │  STM32F103C8T6（芯片 ROM 系统 Bootloader，STM32 侧无需写任何 OTA 代码）
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

---

## 2. 原理简介：STM32F103 系统 Bootloader（AN3155）

1. **进入条件**：`BOOT1(PB2)=0`、`BOOT0=1`，然后复位 → 芯片执行 ROM 中的 USART Bootloader，
   从 **USART1（PA9 TX / PA10 RX）** 监听命令。
2. **波特率（本板实测，与标准不同！）**：AN3155 标准为 115200，**本板 GD32 克隆只在 9600
   能同步**（`main/stm32_ota.h` 写死 `STM32_UART_BAUD 9600`，帧格式 8 数据位 + **偶校验** + 1 停止位）。
   > 这也是当初 `stm32_ota_baud_probe()` 逐个波特率试出来的结论；用 115200 同步永远无 ACK。
3. **通信协议**（AN3155）：
   - 同步：主机发 `0x7F` → Bootloader 回 `0x79`(ACK) 或 `0x1F`(NACK)。
   - 命令头：`[命令码][~命令码]`（1's 补码，实测非 XOR）；数据段：`[数据...][XOR 校验]`。
   - 地址：4 字节大端（实测该克隆需 4B 地址段）。
   - 本次用到的命令：
     - `0x00` Get —— 读固件版本 + 支持命令表（调试用；同步成功标志：
       实测应答 `79 0B 10 00 01 02 11 21 31 43 63 73 82 92 79`，命令表含 0x11 Read/0x21 Go/0x31 Write/0x43 Erase/0x63 Extended Erase/0x73/0x82/0x92）
     - `0x43` Erase —— 擦除 Flash（F103 中容量：**每页 1KB**，逐页擦除最稳妥）
     - `0x31` Write —— 写入，**每块 ≤ 256 字节**，每块等 ACK
     - `0x21` Go —— 跳到指定地址执行（`0x08000000` 启动应用）
4. **写入速度**：9600 ≈ 0.96 KB/s；实测 14368B（15 页）约 20 秒（擦 0.4s + 写 20s）。
5. **地址**：应用区起始 `0x08000000`，Flash 总容量 64KB（F103C8T6），固件上限 `STM32_IMAGE_MAX`。

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
4. **这 4 根线（G16/G17/G4/G5）共用一条线束，极易松脱**——2026-08-16 曾因 G5 松导致"复位不生效"、
   表现为同步无 ACK。症状出现时先挨个重新插紧，再谈软件（见 `docs/PROBLEMS.md` §4/§5）。
5. **别复用这些 GPIO**（GPIO4/5/16/17 已被本方案占用）；WROVER 若 PSRAM 占 16/17 需换引脚并改宏。
6. 建议把 UART2 三线 + BOOT0 + NRST + GND 做成一段 6P 排线，方便插拔。

---

## 4. 需要改动的文件清单（均已落地）

### 4.1 服务器端（server/）
| 文件 | 现状 |
|---|---|
| `server/server.js` | 已含 `GET /ota/stm32_manifest`（读 `firmware/stm32_version.json`）与 `GET /ota/stm32.bin`（流式发送） |
| `server/firmware/stm32_version.json` | `{"version":"1.0.x"}`（由 `scripts/build_stm32.ps1` 自动同步） |
| `server/firmware/stm32.bin` | 发布位（由 `scripts/build_stm32.ps1` 自动部署） |
| `server/README.md` | 含 STM32 端点说明与发布流程 |

> 服务器实时读文件，发布新固件**无需重启**服务器。部署/发布流程见 §5。

### 4.2 ESP32 端（main/）
| 文件 | 现状 |
|---|---|
| `main/stm32_ota.h` | 对外 API + 引脚/波特率/超时宏（`STM32_UART_BAUD 9600` 等） |
| `main/stm32_ota.c` | UART2 驱动、GPIO4/5 控制、AN3155 协议、下载+刷写流程（含三个诊断探针，未接 app_main） |
| `main/main.c` | 已建 `stm32_ota_task`（每 60s 检查，与 ESP32 OTA 同周期） |
| `main/CMakeLists.txt` | 已注册 `stm32_ota.c` |

### 4.3 STM32 端（stm32/esp32_test/）—— 可选
| 文件 | 改动 |
|---|---|
| `Core/Src/main.c` | USER CODE 区：启动时打印 `[STM32 esp32_test] APP vX.Y.Z boot`（依赖 USART1 printf 重定向），便于刷写后确认生效 |

> STM32 **不改也能刷**。版本判断用 ESP32 自己的 NVS（namespace `stm32_ota`, key `last_ver`），
> 不依赖 STM32 上报。刷写后验证：DAPLink VCOM（COM3 @115200）听横幅，或 openocd dump Flash 找版本串。

### 4.4 ESP32 刷写流程（状态机）
```
stm32_ota_task 每 60s：
  1. GET /ota/stm32_manifest  → 解析 version/url/size
  2. NVS 读 last_ver；semver 比较，无新版 → 本轮结束（"版本检查: NVS=x 远端=y"）
  3. 下载 stm32.bin 到堆内存（校验 size ≤ 64KB 且剩余堆足够，缺一不可）
  4. 进 Bootloader：GPIO4=1（BOOT0）→ 等 20ms → GPIO5 低 100ms 复位 → 释放 → 等 200ms
  5. 同步：发 0x7F，等 ACK（失败重试 3 次）
  6. Get(0x00)：打印 Bootloader 命令表（调试定位用）
  7. 擦除：0x43 逐页擦除 ceil(size/1024) 页，等 ACK
  8. 写入：0x31 按 256B/块 + XOR 校验 + 等 ACK，打印进度 %（每 10%）
  9. Go(0x21) → 0x08000000
 10. GPIO4=0（BOOT0）→ 等 20ms → GPIO5 低 100ms 复位 → 让应用启动
 11. NVS 写 last_ver = manifest 版本，打印 "STM32 OTA 成功"
失败 → 日志打印失败阶段 + 错误码，NVS 不更新，下轮自动重试
```

---

## 5. 固件生成与发布流程（STM32）

**日常一键**（推荐，自动升版本 + 编译 + 部署 + 同步版本号）：
```powershell
.\scripts\build_stm32.ps1                    # 版本自动 patch+1
.\scripts\build_stm32.ps1 -Version 1.1.0     # 指定版本
.\scripts\build_stm32.ps1 -NoVersionBump     # 只重编译不升版（纯改代码时）
```
手动流程（等价）：
```powershell
cd D:\esp32\esp_project\local_test\stm32\esp32_test
cmake --build build\Debug                     # Ninja, Debug preset
D:\tools\arm-gnu\bin\arm-none-eabi-objcopy.exe -O binary build\Debug\esp32_test.elf build\Debug\esp32_test.bin
# 复制到 server\firmware\stm32.bin, 并同步 server\firmware\stm32_version.json
```
**触发条件**：`stm32_version.json` 版本 > ESP32 NVS `last_ver`，与 ESP32 OTA 的"防反复刷写"机制一致。
改版本号只需动 `main.c` 的 `APP_VERSION_STR` + `stm32_version.json`，两条命令 `build_stm32.ps1` 全包。
ESP32 的发布同理用 `scripts/build_esp32.ps1`；两个一起 + 直烧 + 起服务用 `scripts/update_all.ps1`。

---

## 6. 调试步骤（分阶段，每阶段可独立验证）

### 阶段 0：服务器端点自测（无需硬件）
```powershell
.\scripts\server_start.ps1                    # 启动/重启服务器（/hello 健康检查）
curl http://192.168.1.11:8888/ota/stm32_manifest
curl -o test.bin http://192.168.1.11:8888/ota/stm32.bin   # 检查文件大小与原 bin 一致
```

### 阶段 1：验证 STM32 能进系统 Bootloader（先不接 ESP32）
1. STM32 板上 BOOT0 跳线帽拨到 **1**（接 3.3V）。
2. 按复位键（或断电重上电）。
3. 用串口助手连 USART1 **@9600**（本板克隆，非 115200）：发 `7F`，收到 `79` 即同步成功。
   > 阶段 1 通过 = 这块板 ROM Bootloader 正常，问题只会出在 ESP32 侧的时序/接线。

### 阶段 2：接好杜邦线，ESP32 只读联调（不擦不写）
1. 按第 3 节接线。
2. ESP32 侧用 `stm32_ota_debug_probe()` / `stm32_ota_baud_probe()` / `stm32_ota_proto_probe()`
   （`main/stm32_ota.c` 内，临时接到 app_main 调用）。
3. 观察 ESP32 日志：
   - 期望：`Get 原始应答: 79 0B 10 00 01 02 11 21 31 43 63 73 82 92 79`（同步成功）。
   - 失败：超时无 ACK → 查接线（交叉、共地）、GPIO4 是否真为高、NRST 复位是否生效
     （万用表实测 GPIO4 高、复位瞬间 GPIO5 低）。

### 阶段 3：首次完整刷写（对照验证）
1. 用 DAPLink + openocd 读出当前 Flash 保存一份基准：
   `openocd -c "dump_image C:/.../stm_flash.bin 0x08000000 0x10000"`（**路径用正斜杠**）。
2. 用 ESP32 走完整流程：进 Bootloader → 同步 → 擦除 → 写入 → Go → 复位。
3. 验证：openocd dump 新 Flash，对比基准；或直接搜版本串 `APP v1.0.x` 已更新；应用行为正常。
   > 刷写中途失败也先别慌：断电/复位后 ESP32 下轮会自动重刷，STM32 不会砖。

### 阶段 4：WiFi 端到端（日常工作流）
1. 修改 STM32 代码 → `.\scripts\build_stm32.ps1`（升版+部署+同步版本）。
2. 观察 ESP32 日志（`.\scripts\monitor_esp32.ps1` 后台抓）：
   manifest 拉取 → 版本比较 → 下载 → 刷写进度 → `STM32 OTA 成功`。
3. DAPLink VCOM（COM3 @115200）听 `APP vX.Y.Z boot` 确认新固件生效。
4. 回归：验证 ESP32 自身 OTA（`firmware.bin`）流程不受影响。

### 阶段 5：恢复性测试（写坏恢复）
1. 刷写过程中拔掉 STM32 电源（模拟断电），或人为把 stm32.bin 传坏。
2. 重新上电，ESP32 下一轮检测会重新擦写；确认 STM32 最终恢复运行新固件。

---

## 7. 故障排查表

| 现象 | 可能原因 | 排查 |
|---|---|---|
| ESP32 拉 manifest 失败 | PC IP / 防火墙 / 服务器未启动 | 与 ESP32 OTA 同一排查路径（见 server/README.md）；瞬时超时下轮自动重试 |
| 同步 0x7F 无响应（无 ACK） | **① DAPLink VCOM TX 接在 PA10 上**；② G16/G17/G4/G5 线束松脱；③ BOOT0 没拉高、复位没生效 | ① 断开 VCOM TX（**最大坑**，见 docs/PROBLEMS.md §5.1）；② 挨个重新插紧；③ 万用表量 GPIO4 高、NRST 低脉冲 |
| 同步有响应，但擦除/写入超时 | 块大小 > 256B、地址未 4 字节对齐、bin 大小算错 | 检查 0x31/0x43 帧的 XOR 校验和实现（命令头是 `[cmd][~cmd]`） |
| 写入成功但应用不运行 | Go 地址错误 / BOOT0 复位后仍为高 | Go 必须是 0x08000000；写完后 GPIO4 必须拉低再复位 |
| 固件反复刷写 | NVS `stm32_ota/last_ver` 没写进去 | 检查 NVS 读写返回值，首次运行要先 `nvs_flash_init()` |
| 刷完变砖（无反应） | 断电中断/固件本身坏 | 不用 ST-Link：BOOT0 由 ESP32 控制，断电重启后 ESP32 自动重刷即可恢复 |
| 刷写期间日志有乱码 `STM32 says: ????` | 已删除的旧调试任务 `stm32_dbg_task` 把 UART2 重配成 115200（历史） | 现固件已无此任务；若再出现检查是否有别的任务碰 UART2 |

**所有已遇问题的完整记录** → `docs/PROBLEMS.md`（构建/OTA/服务器/STM32/DAPLink/OLED/日志 七类）。

---

## 8. 注意事项与风险

1. **刷写窗口**：擦除→写入完成之间**不要给 STM32 断电**（半写固件不可启动，但可恢复重刷）。
2. **USART1 复用**：刷写占用 USART1（PA9/PA10），与应用共用同一组引脚；ESP32 的 `stm32_ota_task`
   仅在需要刷写时驱动该串口，其余时间任务休眠，不影响应用使用。
3. **明文 HTTP**：与 ESP32 OTA 一样基于 HTTP，仅限学习/局域网；生产需 HTTPS + 服务器校验。
4. **版本一致性**：`stm32_version.json` 与 `APP_VERSION_STR` 由脚本自动同步，务必保持与实际固件一致，
   否则会出现"版本没变但内容变了 / 版本变了内容没变"的混乱。
5. **NVS 依赖**：ESP32 需初始化 `nvs_flash`（分区表中已有 nvs 分区）；若换了一块 STM32 板，
   可手动清空 ESP32 的 `stm32_ota/last_ver` 强制重刷。
6. **DAPLink 干扰**：调试用的 DAPLink 只允许 SWD（HID）+ VCOM RX（收 PA9 日志）；
   **VCOM TX 不要接任何东西**——接 PA10 会与 GPIO17 抢线，同步帧被破坏 → 无 ACK（本节最大的坑）。
7. **线束维护**：G16/G17/G4/G5 是同一束杜邦线，插拔/搬动后优先检查它们是否松脱，再动软件。
