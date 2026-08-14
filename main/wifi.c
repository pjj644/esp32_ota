/*
 * 简易 ESP32 WiFi STA 封装
 *  - 事件驱动：WIFI_EVENT_STA_START -> connect()
 *              WIFI_EVENT_STA_DISCONNECTED -> 重连，超过上限置 FAIL 位
 *              IP_EVENT_STA_GOT_IP -> 记录 IP，置 CONNECTED 位
 *  - wifi_sta_wait_connected() 通过 EventGroup 阻塞等待结果
 */
#include <string.h>
#include "wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

/* EventGroup 位定义 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t       *s_sta_netif        = NULL;
static int                s_retry_count      = 0;
static esp_ip4_addr_t     s_ip               = { 0 };
static bool               s_inited           = false;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_ip.addr = 0;
        if (s_retry_count < WIFI_STA_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "disconnected (reason=%d), retry %d/%d",
                     e->reason, s_retry_count, WIFI_STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "disconnected (reason=%d), retry exhausted", e->reason);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_retry_count = 0;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_init(const char *ssid, const char *password)
{
    if (s_inited) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) NVS — WiFi 驱动需要它保存 PHY 校准 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    /* 2) netif + 默认 event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* 3) WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 4) EventGroup + 事件处理器 */
    s_wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_t any_id_inst, got_ip_inst;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip_inst));

    /* 5) 填 SSID/密码并启动 */
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    }
    /* 用 password 是否为空决定鉴权门槛：开放网络放宽到 OPEN，
     * 否则要求至少 WPA2-PSK，避免连到伪 AP/降级到 WEP。 */
    wc.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK
                                                          : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "init done, ssid=\"%s\"", ssid);
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if (!s_inited || !s_wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,   /* 不清位，便于后面再次查询 */
        pdFALSE,   /* 任一位置位就返回 */
        ticks);

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAIL_BIT)      return ESP_ERR_WIFI_NOT_CONNECT;
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    return s_ip.addr != 0;
}

void wifi_sta_get_ip_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 8) return;
    if (s_ip.addr == 0) {
        strncpy(buf, "0.0.0.0", buf_len);
        buf[buf_len - 1] = '\0';
        return;
    }
    snprintf(buf, buf_len, IPSTR, IP2STR(&s_ip));
}
