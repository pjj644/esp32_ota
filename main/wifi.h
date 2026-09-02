#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: 改成 ESP32 与本地电脑共同连接的 WiFi AP */
#define WIFI_SSID  "YOUR_WIFI_SSID"
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
/* 最大重连次数。超过后 wifi_sta_wait_connected() 会返回失败而不是无限阻塞。 */
#ifndef WIFI_STA_MAX_RETRY
#define WIFI_STA_MAX_RETRY  8
#endif

/**
 * 以 STA 模式初始化并启动 WiFi。
 * 内部会：
 *   - 初始化 NVS（WiFi 驱动会用它保存校准/PHY 数据）
 *   - 创建默认 event loop 与 default STA netif
 *   - 注册 WIFI_EVENT / IP_EVENT 处理器（自动重连、置位 EventGroup）
 *   - 设置 ssid/password 并 esp_wifi_start()
 *
 * 只可调用一次。
 *
 * @param ssid      目标 AP 的 SSID（最长 32 字节，不含 '\0'）
 * @param password  WPA/WPA2 密码；开放网络传 NULL 或空串
 * @return ESP_OK 表示驱动启动成功（不代表已连上 AP，需再调 wifi_sta_wait_connected）
 */
esp_err_t wifi_sta_init(const char *ssid, const char *password);

/**
 * 阻塞等待 STA 连接结果。
 * 在 wifi_sta_init() 之后调用。
 *
 * @param timeout_ms  等待超时；portMAX_DELAY 含义请传 UINT32_MAX
 * @return ESP_OK              拿到了 IP
 *         ESP_ERR_WIFI_NOT_CONNECT  重试次数耗尽
 *         ESP_ERR_TIMEOUT     超时仍未出结果
 */
esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms);

/** 查询当前是否已连接并拿到 IP（线程安全，可在任意任务里调）。 */
bool wifi_sta_is_connected(void);

/**
 * 把当前 IPv4 地址写入 buf（格式 "a.b.c.d"，至少 16 字节）。
 * 未连接时写入 "0.0.0.0"。
 */
void wifi_sta_get_ip_str(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
