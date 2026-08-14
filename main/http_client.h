/*
 * ESP32 简易 HTTP GET 客户端封装 (基于 esp_http_client)
 *
 *  - 同步阻塞调用, 单次 GET, 不带 body, 不带自定义 header
 *  - 响应正文写入调用方提供的缓冲区; 最多写入 resp_buf_len - 1 字节 + '\0'
 *  - 失败时缓冲区首字节写 '\0'
 *
 * 用法示例:
 *     char buf[256];
 *     int status = 0;
 *     esp_err_t err = http_client_get_text_hp("10.167.197.162", 8888, "/hello",
 *                                             buf, sizeof(buf), &status);
 *     if (err == ESP_OK) {
 *         ESP_LOGI(TAG, "status=%d body=%s", status, buf);
 *     } else {
 *         ESP_LOGE(TAG, "GET failed: %s", esp_err_to_name(err));
 *     }
 */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GET 一个完整 URL 并把响应正文写入 resp_buf。
 *
 * @param url             完整 URL, e.g. "http://10.167.197.162:8888/hello"
 * @param resp_buf        响应正文缓冲区 (调用方持有生命周期)
 * @param resp_buf_len    缓冲区大小, 至少 1
 * @param out_http_status 可选, 非 NULL 时接收 HTTP 状态码; 失败时为 0
 *
 * @return ESP_OK               2xx 响应, 正文已写入 resp_buf
 *         ESP_ERR_INVALID_ARG  url/resp_buf 为空
 *         ESP_ERR_TIMEOUT      传输超时
 *         ESP_FAIL             其他错误 (DNS / 连接 / 读 / 状态 >= 300)
 */
esp_err_t http_client_get_text(const char *url,
                               char *resp_buf, size_t resp_buf_len,
                               int *out_http_status);

/**
 * 同 http_client_get_text, 但参数拆成 host / port / path,
 * 库内用 snprintf 拼成 "http://<host>:<port><path>"。
 * port 传 0 时省略 ":port"; path 必须以 '/' 开头, 缺省传 "/"。
 */
esp_err_t http_client_get_text_hp(const char *host, uint16_t port,
                                  const char *path,
                                  char *resp_buf, size_t resp_buf_len,
                                  int *out_http_status);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
