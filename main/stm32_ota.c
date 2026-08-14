/*
 * ESP32 -> STM32F103 OTA: 通过芯片 ROM 系统 Bootloader (AN3155) 刷写固件
 *
 * 原理: BOOT1=0 / BOOT0=1 后复位, STM32F103 进入 USART1 系统 Bootloader
 *       (@115200 8N1), 按 AN3155 协议接收命令:
 *         0x7F 同步     (Bootloader 回 0x79 = ACK / 0x1F = NACK)
 *         0x00 Get      (读 Bootloader 版本与支持命令表)
 *         0x43 Erase    (F103 中容量: 每页 1KB, 逐页擦除)
 *         0x31 Write    (每次 <= 256B, 地址 4 字节对齐, 分块等待 ACK)
 *         0x21 Go       (跳转到应用起始地址 0x08000000)
 *       命令帧校验 = 前面所有字节的 XOR。
 *
 * 版本判断: ESP32 的 NVS 记录上次成功刷写的版本 (namespace "stm32_ota",
 *           key "last_ver"), manifest 版本更大才刷, 防止反复刷写。
 *
 * 安全提示: 与 ESP32 OTA 一样基于明文 HTTP, 仅限学习/局域网使用。
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "stm32_ota.h"
#include "http_client.h"

static const char *TAG = "stm32_ota";

/* STM32 应用区起始地址与 F103 中容量页大小 (1KB/页) */
#define STM32_APP_BASE      0x08000000UL
#define STM32_PAGE_SIZE     1024
/* stm32.bin 最大尺寸 (F103C8T6 = 64KB Flash), 下载前做上限检查 */
#define STM32_IMAGE_MAX     (64 * 1024)
/* 下载时要求剩余堆 >= 固件大小 + 该余量, 避免内存不足 */
#define STM32_HEAP_RESERVE  (40 * 1024)

/* AN3155 协议常数 */
#define STM32_ACK           0x79UL
#define STM32_NACK          0x1FUL
#define CMD_GET             0x00UL
#define CMD_ERASE           0x43UL
#define CMD_WRITE           0x31UL
#define CMD_GO              0x21UL
/* 每块最大写入字节数 (F1 限制) */
#define WRITE_CHUNK_MAX     256

/* 时序 (ms) */
#define BOOT0_SETUP_MS      20
#define NRST_PULSE_MS       100
#define BOOTLOADER_WAKE_MS  200
#define SYNC_TIMEOUT_MS     500
#define ACK_TIMEOUT_MS      2000
#define ERASE_TIMEOUT_MS    15000

/* ------------------------------------------------------------------ */
/* 简单工具函数 (与 ota.c 同款实现, 保持独立)                            */

static int semver_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        long va = strtol(a, (char **)&a, 10);
        long vb = strtol(b, (char **)&b, 10);
        if (va != vb) return (va < vb) ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* 极简 JSON 字符串字段提取: 只支持 "key":"value" 形式 (无转义)。
 * 用于 {"version":"..","url":".."} 这类固定结构。 */
static esp_err_t json_get_string(const char *json, const char *key,
                                 char *out, size_t out_len)
{
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return ESP_ERR_INVALID_ARG;

    const char *p = strstr(json, pat);
    if (!p) return ESP_FAIL;
    p += n;

    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return ESP_FAIL;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    if (*p != '"') return ESP_FAIL;
    out[i] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* NVS: 记录/读取上次成功刷写的 STM32 版本                              */

static esp_err_t nvs_get_last_ver(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open("stm32_ota", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* 首次运行, 视为 "0.0.0" */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(read) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_get_str(h, "last_ver", buf, &buflen);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        err = ESP_OK;
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_last_ver(const char *ver)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("stm32_ota", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, "last_ver", ver);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/* 下载 stm32.bin 到堆内存                                             */

static esp_err_t download_image(const char *url, uint8_t **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);
    if (code < 200 || code >= 300) {
        ESP_LOGE(TAG, "GET %s -> %d (non-2xx)", url, code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length <= 0 || content_length > STM32_IMAGE_MAX) {
        ESP_LOGE(TAG, "非法固件大小: %d (上限 %d)", content_length, STM32_IMAGE_MAX);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((uint32_t)content_length + STM32_HEAP_RESERVE > esp_get_free_heap_size()) {
        ESP_LOGE(TAG, "堆内存不足: 固件 %d B, 空闲 %u B",
                 content_length, esp_get_free_heap_size());
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *buf = malloc(content_length);
    if (!buf) {
        ESP_LOGE(TAG, "malloc %d B failed", content_length);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < content_length) {
        int n = esp_http_client_read(client, (char *)buf + total, content_length - total);
        if (n < 0) {
            ESP_LOGE(TAG, "下载中断 (已读 %d/%d)", total, content_length);
            free(buf);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            ESP_LOGE(TAG, "服务端提前结束 (已读 %d/%d)", total, content_length);
            free(buf);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total += n;
    }

    esp_http_client_cleanup(client);
    *out_buf = buf;
    *out_len = (size_t)content_length;
    ESP_LOGI(TAG, "下载完成: %d bytes", content_length);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 硬件控制: UART2 + BOOT0/NRST                                        */

static void pins_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << STM32_BOOT0_GPIO) | (1ULL << STM32_NRST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    /* 默认: BOOT0=0 (应用启动), NRST=1 (不复位) */
    gpio_set_level(STM32_BOOT0_GPIO, 0);
    gpio_set_level(STM32_NRST_GPIO, 1);
}

static void pins_drive_bootloader(void)
{
    /* BOOT0=1, 低脉冲 NRST -> STM32 进入系统 Bootloader */
    gpio_set_level(STM32_BOOT0_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
}

static void pins_release_and_reboot(void)
{
    /* BOOT0=0, 再复位一次 -> STM32 从应用区启动 */
    gpio_set_level(STM32_BOOT0_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
}

static esp_err_t uart2_init(void)
{
    uart_config_t cfg = {
        .baud_rate = STM32_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(STM32_UART_PORT, 1024, 1024, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install: %s (可能已安装, 忽略)", esp_err_to_name(err));
    }
    err = uart_param_config(STM32_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(STM32_UART_PORT, STM32_UART_TX_PIN, STM32_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return err;
    }
    uart_flush_input(STM32_UART_PORT);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* AN3155 Bootloader 协议                                              */

/* 发送一帧 [cmd][params...][XOR 校验] 并等待 ACK。
 * xor_sum 与 param_len: 校验和覆盖 cmd + 所有参数。 */
static esp_err_t boot_send_cmd(uint8_t cmd, uint8_t *params, size_t param_len,
                               uint32_t ack_timeout_ms)
{
    uint8_t frame[WRITE_CHUNK_MAX + 8];
    size_t n = 0;
    uint8_t xor_sum = cmd;

    frame[n++] = cmd;
    for (size_t i = 0; i < param_len; i++) {
        frame[n++] = params[i];
        xor_sum ^= params[i];
    }
    frame[n++] = xor_sum;

    int written = uart_write_bytes(STM32_UART_PORT, frame, n);
    if (written != (int)n) {
        ESP_LOGE(TAG, "uart_write_bytes: %d/%d", written, (int)n);
        return ESP_FAIL;
    }

    uint8_t ack = 0;
    if (uart_read_bytes(STM32_UART_PORT, &ack, 1, pdMS_TO_TICKS(ack_timeout_ms)) != 1) {
        ESP_LOGW(TAG, "cmd 0x%02X 无应答 (超时 %lu ms)", cmd, (unsigned long)ack_timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    if (ack == STM32_NACK) {
        ESP_LOGW(TAG, "cmd 0x%02X 被 NACK", cmd);
        return ESP_FAIL;
    }
    if (ack != STM32_ACK) {
        ESP_LOGW(TAG, "cmd 0x%02X 应答异常: 0x%02X", cmd, ack);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 同步: 发 0x7F 直到收到 ACK (最多重试 3 次) */
static esp_err_t boot_sync(void)
{
    for (int i = 0; i < 3; i++) {
        uint8_t rx = 0;
        uint8_t sync = 0x7F;
        uart_write_bytes(STM32_UART_PORT, &sync, 1);
        if (uart_read_bytes(STM32_UART_PORT, &rx, 1, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) == 1) {
            if (rx == STM32_ACK) {
                ESP_LOGI(TAG, "Bootloader 同步成功 (第 %d 次)", i + 1);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "同步应答异常 0x%02X, 重试", rx);
        } else {
            ESP_LOGW(TAG, "同步无应答, 重试");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

/* Get (0x00): 打印 Bootloader 版本与支持命令表 (仅调试定位用) */
static esp_err_t boot_get_info(void)
{
    uint8_t frame[2] = { CMD_GET, CMD_GET };   /* 命令 + 校验 */
    uart_write_bytes(STM32_UART_PORT, frame, sizeof(frame));

    uint8_t buf[12];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_TIMEOUT_MS);
    while (total < (int)sizeof(buf) && xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(STM32_UART_PORT, buf + total, sizeof(buf) - total,
                                pdMS_TO_TICKS(200));
        if (n > 0) total += n;
    }
    if (total < 3) {
        ESP_LOGW(TAG, "Get 应答不完整: %d bytes", total);
        return ESP_FAIL;
    }
    if (buf[0] != STM32_ACK) {
        ESP_LOGW(TAG, "Get 首字节异常 0x%02X", buf[0]);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Bootloader 版本: 0x%02X", buf[1]);
    ESP_LOGI(TAG, "支持命令表 (hex): %s", (char *)&buf[2]);
    return ESP_OK;
}

/* 擦除: 逐页擦除 (F103 中容量 1KB/页), 擦 image_size 覆盖到的页数 */
static esp_err_t boot_erase_pages(size_t image_size)
{
    size_t pages = (image_size + STM32_PAGE_SIZE - 1) / STM32_PAGE_SIZE;
    if (pages == 0) pages = 1;

    uint8_t params[1 + 64];   /* N-1 + 页号列表, F103C8 最多 64 页 */
    params[0] = (uint8_t)(pages - 1);
    for (size_t i = 0; i < pages; i++) {
        params[1 + i] = (uint8_t)i;
    }
    ESP_LOGI(TAG, "擦除 %d 页 (每页 1KB)...", (int)pages);
    return boot_send_cmd(CMD_ERASE, params, 1 + pages, ERASE_TIMEOUT_MS);
}

/* 写入整片镜像: 按 256B 分块, 每块等 ACK; 末块补齐到 4 字节 (F1 写命令要求) */
static esp_err_t boot_write_image(uint32_t base_addr, const uint8_t *data, size_t size)
{
    ESP_LOGI(TAG, "写入 %d bytes 到 0x%08X ...", (int)size, (unsigned)base_addr);

    size_t offset = 0;
    int last_pct = -1;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > WRITE_CHUNK_MAX) chunk = WRITE_CHUNK_MAX;
        /* F1: 块大小须为 4 的倍数 (写入以字为单位), 末块用 0xFF 补齐 */
        if (chunk % 4 != 0) {
            chunk = (chunk + 3) & ~(size_t)3;
        }

        uint8_t frame[1 + 3 + 1 + 1 + WRITE_CHUNK_MAX];
        size_t n = 0;
        uint8_t xor_sum = 0;

        frame[n++] = CMD_WRITE;                     xor_sum ^= CMD_WRITE;
        uint32_t addr = base_addr + offset;
        frame[n++] = (uint8_t)(addr >> 16);         xor_sum ^= frame[n - 1];
        frame[n++] = (uint8_t)(addr >> 8);          xor_sum ^= frame[n - 1];
        frame[n++] = (uint8_t)addr;                 xor_sum ^= frame[n - 1];
        frame[n++] = xor_sum;                       xor_sum = 0;   /* 独立校验段1 */

        frame[n++] = (uint8_t)(chunk - 1);          xor_sum ^= frame[n - 1];
        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = (offset + i < size) ? data[offset + i] : 0xFF;
            frame[n++] = b;
            xor_sum ^= b;
        }
        frame[n++] = xor_sum;                       /* 校验段2: 覆盖本块整帧 */

        int written = uart_write_bytes(STM32_UART_PORT, frame, n);
        if (written != (int)n) {
            ESP_LOGE(TAG, "写块失败 @0x%08X", (unsigned)addr);
            return ESP_FAIL;
        }

        uint8_t ack = 0;
        if (uart_read_bytes(STM32_UART_PORT, &ack, 1, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) != 1) {
            ESP_LOGE(TAG, "写块 ACK 超时 @0x%08X", (unsigned)addr);
            return ESP_ERR_TIMEOUT;
        }
        if (ack != STM32_ACK) {
            ESP_LOGE(TAG, "写块被拒绝 (0x%02X) @0x%08X", ack, (unsigned)addr);
            return ESP_FAIL;
        }

        offset += chunk;
        if (size > 0) {
            int pct = (int)(offset * 100 / size);
            if (pct != last_pct && (pct % 10 == 0 || pct == 100)) {
                ESP_LOGI(TAG, "写入进度 %d%% (%d/%d)", pct, (int)offset, (int)size);
                last_pct = pct;
            }
        }
    }
    return ESP_OK;
}

/* Go (0x21): 跳到应用地址 (不回 ACK, 直接执行) */
static esp_err_t boot_go(uint32_t addr)
{
    uint8_t frame[4];
    frame[0] = CMD_GO;
    frame[1] = (uint8_t)(addr >> 16);
    frame[2] = (uint8_t)(addr >> 8);
    frame[3] = (uint8_t)addr;
    uint8_t xor_sum = frame[0] ^ frame[1] ^ frame[2] ^ frame[3];

    uint8_t send[5] = { frame[0], frame[1], frame[2], frame[3], xor_sum };
    int written = uart_write_bytes(STM32_UART_PORT, send, sizeof(send));
    if (written != (int)sizeof(send)) {
        ESP_LOGE(TAG, "Go 发送失败");
        return ESP_FAIL;
    }
    /* F1 跳转后不回 ACK, 直接放行; 稍等让应用启动 */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Go 0x%08X 已发出", (unsigned)addr);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                            */

esp_err_t stm32_ota_check_and_update(const char *host, uint16_t port)
{
    if (!host || host[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) 拉 manifest */
    char json[512];
    int status = 0;
    esp_err_t err = http_client_get_text_hp(host, port, "/ota/stm32_manifest",
                                            json, sizeof(json), &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "拉取 stm32 manifest 失败 (status=%d): %s", status, esp_err_to_name(err));
        return err;
    }

    char remote_ver[32];
    char url[192];
    if (json_get_string(json, "version", remote_ver, sizeof(remote_ver)) != ESP_OK ||
        json_get_string(json, "url", url, sizeof(url)) != ESP_OK) {
        ESP_LOGE(TAG, "manifest 字段缺失: %s", json);
        return ESP_FAIL;
    }

    /* 2) 与 NVS 记录的上次版本比较 */
    char last_ver[32];
    if (nvs_get_last_ver(last_ver, sizeof(last_ver)) != ESP_OK) {
        last_ver[0] = '\0';
    }
    if (last_ver[0] == '\0') strcpy(last_ver, "0.0.0");
    ESP_LOGI(TAG, "版本检查: NVS=%s 远端=%s", last_ver, remote_ver);
    if (semver_cmp(remote_ver, last_ver) <= 0) {
        ESP_LOGI(TAG, "STM32 已是最新, 跳过");
        return ESP_OK;
    }

    /* 3) 下载 stm32.bin */
    uint8_t *image = NULL;
    size_t image_size = 0;
    err = download_image(url, &image, &image_size);
    if (err != ESP_OK) {
        return err;
    }

    /* 4) 初始化串口与引脚, 进入 Bootloader */
    pins_init();
    uart2_init();
    pins_drive_bootloader();

    err = boot_sync();
    if (err == ESP_OK) {
        boot_get_info();   /* 仅打印, 失败不中断 */
    }

    /* 5) 擦除 -> 写入 -> Go */
    if (err == ESP_OK) {
        err = boot_erase_pages(image_size);
    }
    if (err == ESP_OK) {
        err = boot_write_image(STM32_APP_BASE, image, image_size);
    }
    if (err == ESP_OK) {
        err = boot_go(STM32_APP_BASE);
    }

    /* 6) 无论成败, 释放 BOOT0 并复位 (半写固件也能下次重刷) */
    pins_release_and_reboot();
    vTaskDelay(pdMS_TO_TICKS(500));
    free(image);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STM32 刷写失败: %s (NVS 版本未更新, 下轮自动重试)",
                 esp_err_to_name(err));
        return err;
    }

    /* 7) 记录版本 */
    err = nvs_set_last_ver(remote_ver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 记录版本失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "STM32 OTA 完成: v%s 已写入并复位", remote_ver);
    return ESP_OK;
}