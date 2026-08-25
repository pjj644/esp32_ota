/*
 * OTA 管理器实现 —— 封装 ESP32 OTA + STM32 OTA 的任务创建与轮询逻辑。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "ota_manager.h"
#include "ota.h"
#include "stm32_ota.h"

static const char *TAG = "ota_mgr";

#define HOST_MAX_LEN  64

static char     s_host[HOST_MAX_LEN];
static uint16_t s_port;
static uint32_t s_esp_period_ms;
static uint32_t s_stm32_period_ms;
static uint32_t s_startup_delay_ms;

static TaskHandle_t s_esp_handle   = NULL;
static TaskHandle_t s_stm32_handle = NULL;
static bool         s_running      = false;

static void esp32_ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_startup_delay_ms));
    while (1) {
        ESP_LOGI(TAG, "OTA: 检查更新...");
        esp_err_t err = ota_check_and_update(s_host, s_port);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA: 本轮未更新 (%s)", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(s_esp_period_ms));
    }
}

static void stm32_ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_startup_delay_ms));
    while (1) {
        ESP_LOGI(TAG, "STM32 OTA: 检查更新...");
        esp_err_t err = stm32_ota_check_and_update(s_host, s_port);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STM32 OTA: 本轮未更新 (%s)", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(s_stm32_period_ms));
    }
}

bool ota_manager_is_running(void)
{
    return s_running;
}

void ota_manager_stop(void)
{
    if (s_esp_handle) {
        vTaskDelete(s_esp_handle);
        s_esp_handle = NULL;
    }
    if (s_stm32_handle) {
        vTaskDelete(s_stm32_handle);
        s_stm32_handle = NULL;
    }
    s_running = false;
    ESP_LOGI(TAG, "OTA 管理器已停止");
}

esp_err_t ota_manager_start(const ota_manager_config_t *config)
{
    if (!config || !config->host || config->host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "OTA 管理器已在运行, 忽略重复启动");
        return ESP_ERR_INVALID_STATE;
    }

    /* 参数归一化 + 拷贝 */
    strncpy(s_host, config->host, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port             = config->port;
    s_esp_period_ms    = config->esp_period_ms   ? config->esp_period_ms   : OTA_MANAGER_DEFAULT_PERIOD_MS;
    s_stm32_period_ms  = config->stm32_period_ms ? config->stm32_period_ms : OTA_MANAGER_DEFAULT_PERIOD_MS;
    s_startup_delay_ms = config->startup_delay_ms ? config->startup_delay_ms : OTA_MANAGER_DEFAULT_DELAY_MS;

    /* WiFi 已通即视为健康, 取消回滚 (幂等, 非 PENDING_VERIFY 时直接返回) */
    esp_err_t ret = ota_confirm_running_app();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "固件健康确认完成");
    } else {
        ESP_LOGW(TAG, "固件健康确认返回: %s (不影响 OTA 轮询)", esp_err_to_name(ret));
    }

    BaseType_t ok;
    ok = xTaskCreate(esp32_ota_task, "ota_check", 8192, NULL, 4, &s_esp_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 ESP32 OTA 任务失败");
        s_esp_handle = NULL;
        return ESP_FAIL;
    }
    ok = xTaskCreate(stm32_ota_task, "stm32_ota", 8192, NULL, 3, &s_stm32_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 STM32 OTA 任务失败");
        vTaskDelete(s_esp_handle);
        s_esp_handle = NULL;
        s_stm32_handle = NULL;
        return ESP_FAIL;
    }

    s_running = true;
    ESP_LOGI(TAG, "OTA 管理器已启动 host=%s:%u esp=%ums stm32=%ums delay=%ums",
             s_host, s_port, (unsigned)s_esp_period_ms,
             (unsigned)s_stm32_period_ms, (unsigned)s_startup_delay_ms);
    return ESP_OK;
}

esp_err_t ota_manager_start_simple(const char *host, uint16_t port)
{
    ota_manager_config_t cfg = {
        .host             = host,
        .port             = port,
        .esp_period_ms    = 0,
        .stm32_period_ms  = 0,
        .startup_delay_ms = 0,
    };
    return ota_manager_start(&cfg);
}
