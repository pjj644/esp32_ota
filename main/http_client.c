/*
 * ESP32 简易 HTTP GET 客户端封装 (基于 esp_http_client)
 *
 *  - 内部使用 esp_http_client_init + esp_http_client_perform + esp_http_client_read_response
 *  - 5s 超时, 单次 GET
 *  - 2xx 才返回 ESP_OK, 其余统一 ESP_FAIL
 *  - 不维护任何静态状态, 可重入
 */
#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "http_client.h"

static const char *TAG = "http_client";

/* 单次请求超时 */
#define HTTP_CLIENT_TIMEOUT_MS  5000

/* 内部: 执行一次 GET, 把响应正文写进 buf (NUL 终止), 状态码写进 *status */
static esp_err_t perform_get(const char *url, char *buf, size_t buflen, int *status)
{
    if (url == NULL || buf == NULL || buflen == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (status) *status = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_CLIENT_TIMEOUT_MS,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    /* 手动控制 open -> fetch_headers -> read_response, 以便把 body 读进调用方 buffer。
     * 注意: 不能用 esp_http_client_perform(), 那个 API 内部已经把 body 消耗完,
     *        之后再调 esp_http_client_read() 立即返回 0, 看起来像 "0 bytes"。 */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (err == ESP_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    (void)content_length;
    int code = esp_http_client_get_status_code(client);
    if (status) *status = code;

    if (code < 200 || code >= 300) {
        ESP_LOGW(TAG, "GET %s -> %d (non-2xx)", url, code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 读 body. esp_http_client_read_response 会读所有数据直到 content_length 或 EOF,
     * 不需要我们再 while 循环。 */
    int n = esp_http_client_read_response(client, buf, (int)buflen - 1);
    int total = 0;
    if (n < 0) {
        ESP_LOGE(TAG, "read_response error: %d", n);
        err = ESP_FAIL;
    } else {
        total = n;
    }
    buf[total] = '\0';

    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "GET %s -> %d, %d bytes", url, code, total);
    return ESP_OK;
}

esp_err_t http_client_get_text(const char *url,
                               char *resp_buf, size_t resp_buf_len,
                               int *out_http_status)
{
    if (url == NULL) {
        ESP_LOGE(TAG, "url is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (resp_buf == NULL || resp_buf_len == 0) {
        ESP_LOGE(TAG, "resp_buf invalid");
        return ESP_ERR_INVALID_ARG;
    }
    return perform_get(url, resp_buf, resp_buf_len, out_http_status);
}

esp_err_t http_client_get_text_hp(const char *host, uint16_t port,
                                  const char *path,
                                  char *resp_buf, size_t resp_buf_len,
                                  int *out_http_status)
{
    if (host == NULL || host[0] == '\0') {
        ESP_LOGE(TAG, "host invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (resp_buf == NULL || resp_buf_len == 0) {
        ESP_LOGE(TAG, "resp_buf invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || path[0] == '\0') path = "/";

    char url[192];
    if (port == 0) {
        snprintf(url, sizeof(url), "http://%s%s", host, path);
    } else {
        snprintf(url, sizeof(url), "http://%s:%u%s", host, (unsigned)port, path);
    }
    return perform_get(url, resp_buf, resp_buf_len, out_http_status);
}
