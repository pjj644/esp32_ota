/*
 * ESP32-WROOM-32 自检 + WiFi STA 连接测试 + 本地 HTTP 测试
 *  - 打印芯片型号 / 核数 / Flash / MAC / IDF 版本
 *  - GPIO2 板载 LED 心跳闪烁
 *  - 启动 WiFi STA，连上后周期打印 IP 与剩余堆内存
 *  - 后台任务每 10s 调一次 http://<本地电脑>:8888/hello
 *
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
#include "stm32_ota.h"

static const char *TAG = "self_test";

/* 本地电脑测试/OTA 服务 —— 若 IP 变化, 请改成当前电脑的局域网 IP */
#define LOCAL_HOST           "192.168.1.11"
#define LOCAL_PORT           8888
#define LOCAL_PATH           "/hello"
#define HTTP_TEST_PERIOD_MS  10000

/* OTA: 每 60s 向本地电脑查询一次 manifest, 有新版本则自动更新并重启 */
#define OTA_CHECK_PERIOD_MS  60000

/* STM32 OTA: 与 ESP32 OTA 同周期检查, 由 stm32_ota_task 调 stm32_ota_check_and_update() */
#define STM32_OTA_CHECK_PERIOD_MS  60000

static void http_test_task(void *pvParameters);
static void ota_task(void *pvParameters);
static void stm32_ota_task(void *pvParameters);
static void stm32_dbg_task(void *pvParameters);

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

void app_main(void)
{
    print_chip_info();
    led_init();

    /* 启动 WiFi，最多等 15 秒看是否拿到 IP */
    ESP_LOGI(TAG, "connecting to WiFi SSID=\"%s\"", WIFI_SSID);
    esp_err_t err = wifi_sta_init(WIFI_SSID, WIFI_PASS);
    if (err == ESP_OK) {
        err = wifi_sta_wait_connected(15000);
    }

    char ip[16];
    if (err == ESP_OK) {
        wifi_sta_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi connected, IP=%s", ip);

        /* WiFi 通了即视为健康检查通过: 确认当前固件 valid, 取消回滚。
         * (若本次是 OTA 后首启, 这一步阻止 bootloader 下次回退到旧固件。) */
        ota_confirm_running_app();

        /* HTTP 测试独立成任务, 不阻塞 2s 心跳 */
        xTaskCreate(http_test_task, "http_test", 4096, NULL, 5, NULL);
        /* OTA 检查任务: 栈要大 (TLS/缓冲), 用 8KB */
        xTaskCreate(ota_task, "ota_check", 8192, NULL, 4, NULL);
        /* STM32 OTA 检查任务: 下载整块固件到堆 + UART 刷写, 栈 8KB */
        xTaskCreate(stm32_ota_task, "stm32_ota", 8192, NULL, 3, NULL);
        /* 临时调试: 非刷写期监听 STM32 USART1(115200) 应用输出 */
        xTaskCreate(stm32_dbg_task, "stm32_dbg", 2048, NULL, 1, NULL);
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

/*
 * http_test_task: 独立 FreeRTOS 任务, 每 HTTP_TEST_PERIOD_MS 调一次
 * 本地电脑:8888/hello。失败仅打日志, 不影响心跳任务。
 */
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

/*
 * ota_task: 每 OTA_CHECK_PERIOD_MS 向本地电脑查一次固件 manifest。
 * 若有新版本, ota_check_and_update() 内部会下载、写另一槽、重启 (不返回)。
 * 无新版本或失败仅打日志, 不影响心跳。
 */
static void ota_task(void *pvParameters)
{
    (void)pvParameters;
    /* 开机先等一会, 让 http_test / 心跳先稳定, 也给本地服务端上线留时间 */
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        ESP_LOGI(TAG, "OTA: 检查更新...");
        esp_err_t err = ota_check_and_update(LOCAL_HOST, LOCAL_PORT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA: 本轮未更新 (%s)", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_MS));
    }
}

/*
 * stm32_ota_task: 每 STM32_OTA_CHECK_PERIOD_MS 向本地电脑查一次 STM32 固件 manifest。
 * 有新版本时 stm32_ota_check_and_update() 内部会: 下载 -> 进 Bootloader -> 擦除/写入 -> 复位。
 * 失败仅打日志 (NVS 版本不更新, 下轮重试), 不影响其他任务。
 */
static void stm32_ota_task(void *pvParameters)
{
    (void)pvParameters;
    /* 开机先等一会, 给 ESP32 自身 OTA 和本地服务端留时间 */
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        ESP_LOGI(TAG, "STM32 OTA: 检查更新...");
        esp_err_t err = stm32_ota_check_and_update(LOCAL_HOST, LOCAL_PORT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STM32 OTA: 本轮未更新 (%s)", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(STM32_OTA_CHECK_PERIOD_MS));
    }
}

/*
 * stm32_dbg_task (临时调试): STM32 应用 USART1 输出 115200 8N1, 而刷写用的
 * UART2 是 9600 8E1。本任务在非刷写期把 UART2 重配为 115200 8N1, 把 STM32
 * 的启动横幅 / OLED 状态消息读进 ESP32 日志, 用于诊断 OLED 不显示。
 * 定位完成确认正常后应删除本任务。
 */
static void stm32_dbg_task(void *pvParameters)
{
    (void)pvParameters;
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uint8_t buf[160];

    while (1) {
        if (stm32_ota_is_busy()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        esp_err_t err = uart_driver_install(STM32_UART_PORT, 1024, 1024, 0, NULL, 0);
        /* 已安装时可能返回 ESP_ERR_INVALID_STATE 或 ESP_FAIL, 均视为正常, 继续用 */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
            ESP_LOGW("stm32_dbg", "install: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        uart_param_config(STM32_UART_PORT, &cfg);
        uart_set_pin(STM32_UART_PORT, STM32_UART_TX_PIN, STM32_UART_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_flush_input(STM32_UART_PORT);

        int n = uart_read_bytes(STM32_UART_PORT, buf, sizeof(buf) - 1,
                                pdMS_TO_TICKS(2500));
        if (n > 0) {
            buf[n] = '\0';
            ESP_LOGI("stm32_dbg", "STM32 says: %.*s", n, (char *)buf);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
