/*
 * ESP32 -> STM32F103 OTA: 通过芯片 ROM 系统 Bootloader (AN3155) 刷写固件
 *
 * 原理: BOOT1=0 / BOOT0=1 后复位, STM32F103 进入 USART1 系统 Bootloader
 *       (@115200 8E1 = 8 数据位 + 偶校验 + 1 停止位), 按 AN3155 协议接收命令:
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
        /* AN3155: USART 系统 Bootloader 使用 8 数据位 + 偶校验 + 1 停止位 (8E1) */
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
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
/* 实测帧式 (Get 可正常应答):                                           */
/*   命令头   = [cmd][~cmd]          (1's 补码, 非 XOR, 实测确认)       */
/*   数据段   = [data...][XOR 校验]  (XOR 覆盖数据段全部字节)            */
/*   地址     = 4 字节大端 (实测该克隆 Bootloader 需 4B 地址段)          */

/* 发送命令头 [cmd][~cmd] 并等待 ACK */
static esp_err_t boot_send_cmd(uint8_t cmd, uint32_t ack_timeout_ms)
{
    uint8_t frame[2] = { cmd, (uint8_t)~cmd };
    int written = uart_write_bytes(STM32_UART_PORT, frame, sizeof(frame));
    if (written != (int)sizeof(frame)) {
        ESP_LOGE(TAG, "cmd 0x%02X 发送失败 (%d/%d)", cmd, written, (int)sizeof(frame));
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

/* 发送数据段 [data...][XOR] 并等待 ACK (data 可含地址/页表/固件数据) */
static esp_err_t boot_send_payload(const uint8_t *data, size_t len, uint32_t ack_timeout_ms)
{
    /* 最大帧 = 1(N) + 256(数据) + 1(校验) = 258 */
    uint8_t frame[WRITE_CHUNK_MAX + 2];
    if (len > WRITE_CHUNK_MAX + 1) return ESP_ERR_INVALID_SIZE;

    uint8_t xor_sum = 0;
    for (size_t i = 0; i < len; i++) {
        frame[i] = data[i];
        xor_sum ^= data[i];
    }
    frame[len] = xor_sum;

    int written = uart_write_bytes(STM32_UART_PORT, frame, len + 1);
    if (written != (int)(len + 1)) {
        ESP_LOGE(TAG, "payload 发送失败 (%d/%d)", written, (int)(len + 1));
        return ESP_FAIL;
    }
    uint8_t ack = 0;
    if (uart_read_bytes(STM32_UART_PORT, &ack, 1, pdMS_TO_TICKS(ack_timeout_ms)) != 1) {
        ESP_LOGW(TAG, "payload(%d B) 无应答 (超时 %lu ms)", (int)len, (unsigned long)ack_timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    if (ack != STM32_ACK) {
        ESP_LOGW(TAG, "payload(%d B) 被拒 (0x%02X)", (int)len, ack);
        return (ack == STM32_NACK) ? ESP_FAIL : ESP_FAIL;
    }
    return ESP_OK;
}

/* 同步: 发 0x7F, 跳过回显, 直到收到 ACK (最多 3 次) */
static esp_err_t boot_sync(void)
{
    uint8_t sync = 0x7F;
    for (int i = 0; i < 3; i++) {
        uart_flush_input(STM32_UART_PORT);
        uart_write_bytes(STM32_UART_PORT, &sync, 1);
        uint8_t buf[8];
        int total = 0;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYNC_TIMEOUT_MS);
        while (total < (int)sizeof(buf) && xTaskGetTickCount() < deadline) {
            int n = uart_read_bytes(STM32_UART_PORT, buf + total, sizeof(buf) - total,
                                    pdMS_TO_TICKS(50));
            if (n > 0) {
                for (int j = total; j < total + n; j++) {
                    if (buf[j] == STM32_ACK) {
                        ESP_LOGI(TAG, "Bootloader 同步成功 (第 %d 次)", i + 1);
                        return ESP_OK;
                    }
                }
                total += n;
            }
        }
        ESP_LOGW(TAG, "同步无 ACK (第 %d 次)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

/* Get (0x00): 打印 Bootloader 版本与支持命令表 (调试用) */
static esp_err_t boot_get_info(void)
{
    uint8_t frame[2] = { CMD_GET, (uint8_t)~CMD_GET };
    uart_write_bytes(STM32_UART_PORT, frame, sizeof(frame));

    uint8_t buf[16];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ACK_TIMEOUT_MS);
    while (total < (int)sizeof(buf) && xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(STM32_UART_PORT, buf + total, sizeof(buf) - total,
                                pdMS_TO_TICKS(200));
        if (n > 0) total += n;
    }
    if (total < 3 || buf[0] != STM32_ACK) {
        ESP_LOGW(TAG, "Get 应答异常 (%d bytes, 首字节 0x%02X)", total, total ? buf[0] : 0);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Bootloader 版本: 0x%02X", buf[2]);
    char hex[64]; size_t hn = 0;
    for (int i = 0; i < total; i++) {
        int r = snprintf(hex + hn, sizeof(hex) - hn, "%02X ", buf[i]);
        if (r <= 0 || hn + (size_t)r >= sizeof(hex)) break;
        hn += (size_t)r;
    }
    ESP_LOGI(TAG, "Get 原始响应: %s", hex);
    return ESP_OK;
}

/* 擦除: 逐页擦除 (F103 中容量 1KB/页), 擦 image_size 覆盖到的页数。
 * 帧: [43 BC] ->ACK-> [页码量-1][页码列表][XOR] ->ACK */
static esp_err_t boot_erase_pages(size_t image_size)
{
    size_t pages = (image_size + STM32_PAGE_SIZE - 1) / STM32_PAGE_SIZE;
    if (pages == 0) pages = 1;

    esp_err_t err = boot_send_cmd(CMD_ERASE, ACK_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    uint8_t payload[1 + 64];   /* N-1 + 页号列表, F103C8 最多 64 页 */
    payload[0] = (uint8_t)(pages - 1);
    for (size_t i = 0; i < pages; i++) {
        payload[1 + i] = (uint8_t)i;
    }
    ESP_LOGI(TAG, "擦除 %d 页 (每页 1KB)...", (int)pages);
    return boot_send_payload(payload, 1 + pages, ERASE_TIMEOUT_MS);
}

/* 写入整片镜像: 按 256B 分块, 每块:
 * [31 CE] ->ACK-> [地址3B][XOR] ->ACK-> [N-1][数据][XOR] ->ACK;
 * 末块补齐到 4 字节 (F1 写命令要求以字写入) */
static esp_err_t boot_write_image(uint32_t base_addr, const uint8_t *data, size_t size)
{
    ESP_LOGI(TAG, "写入 %d bytes 到 0x%08X ...", (int)size, (unsigned)base_addr);

    size_t offset = 0;
    int last_pct = -1;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > WRITE_CHUNK_MAX) chunk = WRITE_CHUNK_MAX;
        if (chunk % 4 != 0) {
            chunk = (chunk + 3) & ~(size_t)3;
        }

        esp_err_t err = boot_send_cmd(CMD_WRITE, ACK_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写块命令头失败 @0x%08X", (unsigned)(base_addr + offset));
            return err;
        }

        uint32_t addr = base_addr + offset;
        uint8_t addr_b[4] = {
            (uint8_t)(addr >> 24),
            (uint8_t)(addr >> 16),
            (uint8_t)(addr >> 8),
            (uint8_t)addr,
        };
        err = boot_send_payload(addr_b, sizeof(addr_b), ACK_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写块地址段失败 @0x%08X", (unsigned)addr);
            return err;
        }

        uint8_t payload[1 + WRITE_CHUNK_MAX];
        payload[0] = (uint8_t)(chunk - 1);
        for (size_t i = 0; i < chunk; i++) {
            payload[1 + i] = (offset + i < size) ? data[offset + i] : 0xFF;
        }
        err = boot_send_payload(payload, 1 + chunk, ACK_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写块数据段失败 @0x%08X", (unsigned)addr);
            return err;
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

/* Go (0x21): [21 DE] ->ACK-> [地址3B][XOR] ->ACK, 然后跳转 */
static esp_err_t boot_go(uint32_t addr)
{
    esp_err_t err = boot_send_cmd(CMD_GO, ACK_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    uint8_t addr_b[4] = {
        (uint8_t)(addr >> 24),
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)addr,
    };
    err = boot_send_payload(addr_b, sizeof(addr_b), ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Go 地址段失败");
        return err;
    }
    /* F1 回 ACK 后跳转执行 */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Go 0x%08X 已发出", (unsigned)addr);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 诊断探针 (临时): 定位 STM32 未进入 Bootloader 的原因                 */

static void probe_dump_rx(const char *label, uint32_t ms)
{
    uint8_t buf[384];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < deadline && total < (int)sizeof(buf) - 1) {
        int n = uart_read_bytes(STM32_UART_PORT, buf + total, sizeof(buf) - 1 - total,
                                pdMS_TO_TICKS(200));
        if (n > 0) total += n;
    }
    if (total == 0) {
        ESP_LOGI(TAG, "[probe] %s: 无数据 (%u ms)", label, (unsigned)ms);
        return;
    }
    ESP_LOGI(TAG, "[probe] %s: 收到 %d bytes:", label, total);
    char hex[96];
    size_t hex_n = 0;
    for (int i = 0; i < total; i++) {
        int r = snprintf(hex + hex_n, sizeof(hex) - hex_n, "%02X ", buf[i]);
        if (r <= 0 || hex_n + (size_t)r >= sizeof(hex)) {
            ESP_LOGI(TAG, "[probe]   %s", hex);
            hex_n = 0;
            i--;   /* 重新处理当前字节 */
            continue;
        }
        hex_n += (size_t)r;
        if ((i + 1) % 16 == 0) {
            ESP_LOGI(TAG, "[probe]   %s", hex);
            hex_n = 0;
        }
    }
    if (hex_n > 0) ESP_LOGI(TAG, "[probe]   %s", hex);
}

static void probe_pulse_nrst(void)
{
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
}

void stm32_ota_debug_probe(void)
{
    pins_init();
    uart2_init();
    uart_flush_input(STM32_UART_PORT);

    ESP_LOGI(TAG, "========== STM32 OTA 诊断探针 ==========");
    ESP_LOGI(TAG, "[probe] GPIO4(BOOT0)=%d GPIO5(NRST)=%d",
             gpio_get_level(STM32_BOOT0_GPIO), gpio_get_level(STM32_NRST_GPIO));

    /* D1: 空闲监听 —— STM32 应用是否在周期发送数据? */
    probe_dump_rx("D1 空闲监听 2s (BOOT0=0)", 2000);

    /* D2: 不复位 BOOT0, 仅复位 —— NRST 是否生效? 应用是否会重启并打印横幅? */
    probe_pulse_nrst();
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
    probe_dump_rx("D2 复位后监听 1s (BOOT0=0)", 1000);

    /* D3: BOOT0=1 + 复位 —— 进 Bootloader? 进则无数据; 没进则应用横幅再次出现 */
    gpio_set_level(STM32_BOOT0_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    probe_pulse_nrst();
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
    probe_dump_rx("D3 BOOT0=1 复位后监听 1s", 1000);

    /* D4: 发 0x7F 看应答 —— Bootloader 在等同步; 应用则无应答 */
    uint8_t sync = 0x7F;
    uart_write_bytes(STM32_UART_PORT, &sync, 1);
    uint8_t ack = 0;
    esp_err_t err = uart_read_bytes(STM32_UART_PORT, &ack, 1, pdMS_TO_TICKS(1000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[probe] D4 0x7F 应答: 0x%02X %s", ack,
                 ack == STM32_ACK ? "(ACK, Bootloader 就绪!)" :
                 ack == STM32_NACK ? "(NACK)" : "(未知)");
    } else {
        ESP_LOGI(TAG, "[probe] D4 0x7F 无应答");
    }

    /* 恢复: BOOT0=0 + 复位, 让应用回到正常状态 */
    gpio_set_level(STM32_BOOT0_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    probe_pulse_nrst();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "========== 诊断探针结束 ==========");
}

/* ------------------------------------------------------------------ */
/* 多波特率 Bootloader 探测 (临时调试用)                                */

/* 协议应答: 0x7F 同步, 成功回 0x79 (ACK), 拒绝回 0x1F (NACK)。
 * 候选波特率覆盖常见克隆芯片 (GD32 等) 的 Bootloader 兼容范围。 */
void stm32_ota_baud_probe(void)
{
    static const int bauds[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800 };
    static const int n_bauds = (int)(sizeof(bauds) / sizeof(bauds[0]));

    pins_init();
    uart2_init();

    ESP_LOGI(TAG, "========== STM32 多波特率 Bootloader 探测 ==========");
    for (int i = 0; i < n_bauds; i++) {
        int baud = bauds[i];

        /* 重新配置波特率 + 进 Bootloader */
        uart_set_baudrate(STM32_UART_PORT, baud);
        uart_flush_input(STM32_UART_PORT);
        gpio_set_level(STM32_BOOT0_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
        gpio_set_level(STM32_NRST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
        gpio_set_level(STM32_NRST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
        uart_flush_input(STM32_UART_PORT);

        /* 两阶段同步: 0x7F -> ACK -> 0x7F -> ACK (GD32 克隆芯片必须两阶段) */
        int phase = 0;   /* 0=无应答 1=阶段1完成 2=阶段2完成 */
        uint8_t resp = 0;
        for (int t = 0; t < 3; t++) {
            uint8_t sync = 0x7F;
            uart_write_bytes(STM32_UART_PORT, &sync, 1);
            if (uart_read_bytes(STM32_UART_PORT, &resp, 1, pdMS_TO_TICKS(500)) != 1) {
                continue;
            }
            if (resp == STM32_ACK) {
                phase++;
                if (phase >= 2) break;
            } else if (resp == STM32_NACK) {
                phase = -1;   /* 明确被拒 */
                break;
            } else {
                phase = -2;   /* 异常字节 */
                break;
            }
        }

        if (phase >= 2) {
            /* 同步完成! 发 Get (0x00) 确认协议可用并读回版本/命令表 */
            ESP_LOGI(TAG, "[baud %5d] **** 两阶段同步成功 ****", baud);
            uint8_t frame[2] = { CMD_GET, CMD_GET };
            uart_write_bytes(STM32_UART_PORT, frame, sizeof(frame));
            uint8_t rbuf[16];
            int total = 0;
            TickType_t dline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            while (total < (int)sizeof(rbuf) && xTaskGetTickCount() < dline) {
                int n = uart_read_bytes(STM32_UART_PORT, rbuf + total,
                                        sizeof(rbuf) - total, pdMS_TO_TICKS(200));
                if (n > 0) total += n;
            }
            ESP_LOGI(TAG, "[baud %5d] Get 应答 %d bytes:", baud, total);
            char hex[96]; size_t hn = 0;
            for (int k = 0; k < total; k++) {
                int r = snprintf(hex + hn, sizeof(hex) - hn, "%02X ", rbuf[k]);
                if (r <= 0 || hn + (size_t)r >= sizeof(hex)) { hn = 0; k--; continue; }
                hn += (size_t)r;
            }
            if (hn > 0) ESP_LOGI(TAG, "[baud %5d]   %s", baud, hex);
            break;   /* 找到可用波特率, 结束 */
        } else if (phase == -1) {
            ESP_LOGI(TAG, "[baud %5d] Bootloader 在, 但 NACK 同步", baud);
        } else if (phase == -2) {
            ESP_LOGI(TAG, "[baud %5d] 收到异常字节 0x%02X", baud, resp);
        } else {
            ESP_LOGI(TAG, "[baud %5d] 无应答 (阶段%d)", baud, phase);
        }
    }

    /* 恢复: BOOT0=0 + 复位, 波特率回到 115200 */
    uart_set_baudrate(STM32_UART_PORT, STM32_UART_BAUD);
    gpio_set_level(STM32_BOOT0_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "========== 多波特率探测结束 ==========");
}

/* ------------------------------------------------------------------ */
/* 精细协议探针 (临时调试用): 9600 下逐条命令 dump 原始响应              */

static void proto_rx_dump(uint32_t ms)
{
    uint8_t buf[64];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (total < (int)sizeof(buf) && xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(STM32_UART_PORT, buf + total, sizeof(buf) - total,
                                pdMS_TO_TICKS(100));
        if (n > 0) total += n;
    }
    if (total == 0) {
        ESP_LOGI(TAG, "[proto]   (无数据 %u ms)", (unsigned)ms);
        return;
    }
    char hex[192]; size_t hn = 0;
    for (int i = 0; i < total; i++) {
        int r = snprintf(hex + hn, sizeof(hex) - hn, "%02X ", buf[i]);
        if (r <= 0 || hn + (size_t)r >= sizeof(hex)) {
            ESP_LOGI(TAG, "[proto]   %s", hex); hn = 0; i--; continue;
        }
        hn += (size_t)r;
    }
    if (hn > 0) ESP_LOGI(TAG, "[proto]   %s", hex);
}

void stm32_ota_proto_probe(void)
{
    pins_init();
    uart2_init();
    /* 已知 9600 可同步, 直接锁 9600 */
    uart_set_baudrate(STM32_UART_PORT, 9600);

    /* 进 Bootloader */
    gpio_set_level(STM32_BOOT0_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
    uart_flush_input(STM32_UART_PORT);

    ESP_LOGI(TAG, "========== STM32 协议暴力扫描 v2 (9600) ==========");

    /* 进 Bootloader, 同步并消费 ACK 残留 */
    gpio_set_level(STM32_BOOT0_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
    uart_flush_input(STM32_UART_PORT);
    uint8_t b = 0x7F;
    uart_write_bytes(STM32_UART_PORT, &b, 1);
    {
        uint8_t s_buf[8]; int s_n = 0;
        TickType_t s_dl = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        while (s_n < (int)sizeof(s_buf) && xTaskGetTickCount() < s_dl) {
            int ng = uart_read_bytes(STM32_UART_PORT, s_buf + s_n, sizeof(s_buf) - s_n, pdMS_TO_TICKS(50));
            if (ng > 0) s_n += ng;
        }
        char s_hex[32]; size_t s_hn = 0;
        for (int i = 0; i < s_n; i++) {
            int r2 = snprintf(s_hex + s_hn, sizeof(s_hex) - s_hn, "%02X ", s_buf[i]);
            if (r2 > 0 && s_hn + (size_t)r2 < sizeof(s_hex)) s_hn += (size_t)r2;
        }
        ESP_LOGI(TAG, "[scan] 同步残留(已消费): %s", s_hn ? s_hex : "(无)");
    }

    /* 命令集合: Get / GetVersion / GetID / ReadMem / Go / WriteMem / Erase / 扩展擦除 */
    static const uint8_t cmds[] = { 0x00, 0x01, 0x02, 0x11, 0x21, 0x31, 0x43, 0x44 };
    static const char *fnames[] = { "XOR", "CMPL", "SINGLE", "NUL", "FF", "7F", "TRIPLE", "SYNC2" };
    int silence = 0;

    for (size_t ci = 0; ci < sizeof(cmds); ci++) {
        uint8_t c = cmds[ci];
        uint8_t fr[8][4];
        int    ln[8];
        fr[0][0] = c; fr[0][1] = c;                    ln[0] = 2;  /* XOR      c c    */
        fr[1][0] = c; fr[1][1] = (uint8_t)~c;          ln[1] = 2;  /* 补码     c ~c   */
        fr[2][0] = c;                                  ln[2] = 1;  /* 单字节   c      */
        fr[3][0] = c; fr[3][1] = 0x00;                 ln[3] = 2;  /* NUL      c 00   */
        fr[4][0] = c; fr[4][1] = 0xFF;                 ln[4] = 2;  /* FF       c FF   */
        fr[5][0] = c; fr[5][1] = 0x7F;                 ln[5] = 2;  /* 7F       c 7F   */
        fr[6][0] = c; fr[6][1] = c; fr[6][2] = c;      ln[6] = 3;  /* 三次     c c c  */
        fr[7][0] = 0x7F; fr[7][1] = c; fr[7][2] = c;   ln[7] = 3;  /* 双同步式 7F c c */

        for (int fi = 0; fi < 8; fi++) {
            if (silence >= 3) {
                ESP_LOGI(TAG, "[scan] 连续静默, 重新进入 bootloader");
                uart_flush_input(STM32_UART_PORT);
                gpio_set_level(STM32_NRST_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
                gpio_set_level(STM32_NRST_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(BOOTLOADER_WAKE_MS));
                uart_flush_input(STM32_UART_PORT);
                b = 0x7F;
                uart_write_bytes(STM32_UART_PORT, &b, 1);
                uint8_t tmp[32]; int trn = 0;
                TickType_t dl = xTaskGetTickCount() + pdMS_TO_TICKS(600);
                while (trn < (int)sizeof(tmp) && xTaskGetTickCount() < dl) {
                    int n = uart_read_bytes(STM32_UART_PORT, tmp + trn, sizeof(tmp) - trn, pdMS_TO_TICKS(50));
                    if (n > 0) trn += n;
                }
                ESP_LOGI(TAG, "[scan]   重新同步完成 (消费 %d bytes)", trn);
                silence = 0;
            }

            uart_flush_input(STM32_UART_PORT);
            uart_write_bytes(STM32_UART_PORT, fr[fi], ln[fi]);

            uint8_t r[16]; int rn = 0;
            bool got_ack = false;
            TickType_t dl = xTaskGetTickCount() + pdMS_TO_TICKS(300);
            while (rn < (int)sizeof(r) && xTaskGetTickCount() < dl) {
                int n = uart_read_bytes(STM32_UART_PORT, r + rn, sizeof(r) - rn, pdMS_TO_TICKS(40));
                if (n > 0) rn += n;
            }
            /* 严格判定: 响应以 0x79 开头且后面不是 0x1F 才算命令 ACK */
            if (rn >= 1 && r[0] == STM32_ACK && (rn == 1 || r[1] != 0x1F)) got_ack = true;
            char hex[64]; size_t hn = 0;
            for (int i = 0; i < rn; i++) {
                int r2 = snprintf(hex + hn, sizeof(hex) - hn, "%02X ", r[i]);
                if (r2 <= 0 || hn + (size_t)r2 >= sizeof(hex)) { hn = 0; i--; continue; }
                hn += (size_t)r2;
            }
            ESP_LOGI(TAG, "[scan] cmd=%02X %-6s len=%d -> %s%s",
                     c, fnames[fi], ln[fi], got_ack ? "!!! ACK !!!" : (rn ? "NACK/其他" : "静默"), hex);
            if (got_ack) {
                ESP_LOGI(TAG, "[scan] ======== 命中: cmd=%02X 帧式=%s 响应=%s ========", c, fnames[fi], hex);
                goto scan_done;
            }
            if (rn == 0) silence++; else silence = 0;
        }
    }
scan_done:

    /* 恢复: BOOT0=0 + 复位, 波特率回 115200 */
    uart_set_baudrate(STM32_UART_PORT, STM32_UART_BAUD);
    gpio_set_level(STM32_BOOT0_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT0_SETUP_MS));
    gpio_set_level(STM32_NRST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_set_level(STM32_NRST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "========== 协议暴力扫描结束 ==========");
}

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