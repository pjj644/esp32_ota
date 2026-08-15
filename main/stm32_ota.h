/*
 * ESP32 -> STM32F103 OTA 模块头文件
 *
 * 通过 UART2 + 芯片 ROM 系统 Bootloader (AN3155 协议) 给 STM32 刷固件。
 * 接线 (详见仓库根目录 STM32_WiFi_OTA_方案.md):
 *   ESP32 GPIO17 (U2TXD) -> STM32 PA10 (USART1_RX)
 *   ESP32 GPIO16 (U2RXD) -> STM32 PA9  (USART1_TX)
 *   ESP32 GPIO4          -> STM32 BOOT0
 *   ESP32 GPIO5          -> STM32 NRST
 *   GND 互联 (必须共地)
 */
#ifndef STM32_OTA_H
#define STM32_OTA_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UART 与控制引脚配置 */
#define STM32_UART_PORT    UART_NUM_2
#define STM32_UART_TX_PIN  17
#define STM32_UART_RX_PIN  16
/* 实测: 该板（疑似 GD32 克隆 F103, 8MHz HSI）ROM Bootloader 仅在 9600 可同步 */
#define STM32_UART_BAUD    9600
#define STM32_BOOT0_GPIO   GPIO_NUM_4
#define STM32_NRST_GPIO    GPIO_NUM_5

/**
 * 执行一次完整的 STM32 OTA 检查与刷写:
 *   1) GET http://host:port/ota/stm32_manifest  -> {"version":"...","url":"..."}
 *   2) 与 NVS 里上次成功刷写的版本比较, 无新版直接返回
 *   3) 下载 stm32.bin 到堆内存 -> 进 Bootloader -> 擦除 -> 写入 -> Go -> 复位
 *
 * 失败仅打日志并返回错误码, 不阻塞调用方; 下轮调用会重试。
 */
esp_err_t stm32_ota_check_and_update(const char *host, uint16_t port);

/** 诊断探针 (临时调试用): 逐阶段定位 STM32 未进入 Bootloader 的原因。 */
void stm32_ota_debug_probe(void);

/** 多波特率探测 (临时调试用): 依次以 9600~460800 尝试 Bootloader 同步。 */
void stm32_ota_baud_probe(void);

/** 精细协议探测 (临时调试用): 9600 下逐条命令 dump Bootloader 原始响应。 */
void stm32_ota_proto_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* STM32_OTA_H */