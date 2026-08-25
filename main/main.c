/*
 * ESP32-WROOM-32 自检 + WiFi STA 连接测试 + 本地 HTTP 测试
 *  - 打印芯片型号 / 核数 / Flash / MAC / IDF 版本
 *  - GPIO2 板载 LED 心跳闪烁
 *  - 启动 WiFi STA，连上后周期打印 IP 与剩余堆内存
 *  - 后台任务每 10s 调一次 http://<本地电脑>:8888/hello
 *  - 双 OTA (ESP32 + STM32) 由 ota_manager 一键托管
 */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "led.h"
#include "wifi.h"
#include "http_client.h"
#include "ota.h"
#include "ota_manager.h"

static const char *TAG = "self_test";

/* 本地 OTA / HTTP 测试服务器 —— IP 变化时改这里即可 (同步 config/paths.ps1) */
#define LOCAL_HOST           "192.168.1.11"
#define LOCAL_PORT           8888
#define LOCAL_PATH           "/hello"
#define HTTP_TEST_PERIOD_MS  10000

static void http_test_task(void *pvParameters);
static void print_chip_info(void);

void app_main(void)
{
    print_chip_info();
    led_init();

    ESP_LOGI(TAG, "connecting to WiFi SSID=\"%s\"", WIFI_SSID);
    esp_err_t err = wifi_sta_init(WIFI_SSID, WIFI_PASS);
    if (err == ESP_OK) {
        err = wifi_sta_wait_connected(15000);
    }

    char ip[16];
    if (err == ESP_OK) {
        wifi_sta_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi connected, IP=%s", ip);

        xTaskCreate(http_test_task, "http_test", 4096, NULL, 5, NULL);
        /* 一行启动双 OTA: ESP32 + STM32 各自后台轮询, 周期/延时走默认值 (60s/10s) */
        err = ota_manager_start_simple(LOCAL_HOST, LOCAL_PORT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA 管理器启动失败: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "WiFi unavailable: %s", esp_err_to_name(err));
    }

    uint32_t tick = 0;
    while (1) {
        int level = led_toggle();
        wifi_sta_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "[heartbeat #%" PRIu32 "] LED=%d  IP=%s  free_heap=%" PRIu32,
                 tick++, level, ip, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void print_chip_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "================ ESP32-WROOM-32 SELF TEST ================");
    ESP_LOGI(TAG, "*** OTA TEST BUILD ***");
    ESP_LOGI(TAG, "IDF version : %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip        : %s rev v%d.%d, %d core(s)",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "Features    : WiFi%s%s%s",
             (chip.features & CHIP_FEATURE_BT)        ? " / BT"        : "",
             (chip.features & CHIP_FEATURE_BLE)       ? " / BLE"       : "",
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? " / EMB-FLASH" : "");
    ESP_LOGI(TAG, "Flash       : %" PRIu32 " MB %s",
             flash_size / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "(embedded)" : "(external)");
    ESP_LOGI(TAG, "MAC (STA)   : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Free heap   : %" PRIu32 " bytes", esp_get_free_heap_size());
    char fw_ver[32];
    ota_get_running_version(fw_ver, sizeof(fw_ver));
    ESP_LOGI(TAG, "Firmware    : v%s", fw_ver);
    ESP_LOGI(TAG, "==========================================================");
}

/* http_test_task: 每 HTTP_TEST_PERIOD_MS 调一次 LOCAL_HOST:LOCAL_PORT/LOCAL_PATH */
static void http_test_task(void *pvParameters)
{
    (void)pvParameters;
    char buf[256];
    int  status = 0;

    while (1) {
        esp_err_t err = http_client_get_text_hp(
            LOCAL_HOST, LOCAL_PORT, LOCAL_PATH, buf, sizeof(buf), &status);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "<- 200: %s", buf);
        } else {
            ESP_LOGE(TAG, "local server unreachable: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(HTTP_TEST_PERIOD_MS));
    }
}
