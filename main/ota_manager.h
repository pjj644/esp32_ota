/*
 * OTA 管理器 —— 将 ESP32 自身 OTA 与 STM32 经 UART 的 OTA 统一封装。
 *
 * 目标: 让 main.c 仅需一次调用即可启动双 OTA 轮询, 无需在业务层维护
 *       FreeRTOS 任务、轮询周期、启动延时等细节。
 *
 * 用法 (在 WiFi 连上拿到 IP 后):
 *     ota_manager_config_t cfg = {
 *         .host = "192.168.1.11",
 *         .port = 8888,
 *     };
 *     ota_manager_start(&cfg);  // 周期/延时为 0 时自动取默认值
 *
 * 或最简:
 *     ota_manager_start_simple("192.168.1.11", 8888);
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MANAGER_DEFAULT_PERIOD_MS  60000
#define OTA_MANAGER_DEFAULT_DELAY_MS   10000

typedef struct {
    const char *host;              // 必填, 服务器 IP/域名
    uint16_t    port;              // 必填, 如 8888 (传 0 则省略端口)
    uint32_t    esp_period_ms;     // ESP32 OTA 轮询周期, 0=60000
    uint32_t    stm32_period_ms;   // STM32 OTA 轮询周期, 0=60000
    uint32_t    startup_delay_ms;  // 首次检查前延时, 0=10000
} ota_manager_config_t;

/**
 * 启动双 OTA 管理器 (ESP32 + STM32 各一个后台任务)。
 * 内部会自动:
 *   - 确认当前固件 valid (取消回滚保护)
 *   - 各自延时 startup_delay_ms 后每 period_ms 轮询一次
 *   - 有新版本时分别走 esp_https_ota / AN3155 UART 刷写
 *
 * @param config  配置, host 不能为空
 * @return ESP_OK              已启动
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_INVALID_STATE 已启动过 (重复调用)
 *         ESP_FAIL            任务创建失败
 */
esp_err_t ota_manager_start(const ota_manager_config_t *config);

/** 简化版: 仅传 host/port, 其余取默认值。等价于 ota_manager_start(&cfg)。 */
esp_err_t ota_manager_start_simple(const char *host, uint16_t port);

/** 停止并清理 OTA 任务 (可选, 一般无需调用)。 */
void ota_manager_stop(void);

/** 查询是否已启动。 */
bool ota_manager_is_running(void);

#ifdef __cplusplus
}
#endif
