/*
 * ESP32 远程 OTA 模块 (基于 esp_https_ota 高级 API + 明文 HTTP)
 *
 * 工作流:
 *   1) ota_check_and_update(host, port):
 *        GET http://host:port/ota/manifest  -> {"version":"1.0.1","url":"..."}
 *        用 semver 比较 manifest.version 与当前运行版本; 不更新则返回。
 *   2) 若有新版本, esp_https_ota_begin() 打开固件流, get_img_desc() 读到
 *      新固件内嵌版本, 再次确认 != 当前版本 (防止服务端 manifest 与固件不一致
 *      导致的反复刷写), 然后 perform() 循环写入另一个 OTA 槽并打印进度。
 *   3) finish() 校验并把 boot 分区切到新槽, esp_restart() 重启。
 *
 * 回滚:
 *   - 开启了 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE。新固件首次启动时处于
 *     PENDING_VERIFY 状态; 应用需在健康检查通过后调 ota_confirm_running_app()
 *     标记为 valid, 否则下次重启 bootloader 自动回退旧槽。
 *
 * 安全提示: 明文 HTTP 无加密、无服务器身份校验, 仅用于学习。生产环境应改用
 *           HTTPS + 服务器证书 (把 cert_pem 填进 http_config)。
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

#include "ota.h"
#include "http_client.h"

static const char *TAG = "ota";

/* manifest 端点路径 (固件 url 由 manifest 内的 "url" 字段给出) */
#define OTA_MANIFEST_PATH   "/ota/manifest"

/*
 * 语义化版本比较: 解析形如 "MAJOR.MINOR.PATCH" 的字符串 (缺失位视为 0)。
 * 返回 >0 表示 a>b, <0 表示 a<b, 0 表示相等。非数字字符按解析终止处理。
 */
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

void ota_get_running_version(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    const esp_app_desc_t *desc = esp_app_get_description();
    strncpy(buf, desc->version, buflen - 1);
    buf[buflen - 1] = '\0';
}

esp_err_t ota_confirm_running_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return ESP_FAIL;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "首次运行新固件, 健康检查通过 -> 标记 valid, 取消回滚");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    /* 已经是 valid / 无回滚需求, 无需处理 */
    return ESP_OK;
}

/*
 * 从 manifest JSON 里取出字符串字段 key 的值, 写进 out。
 * 极简解析: 只处理 "key" : "value" 形式 (value 为字符串, 无转义)。
 * manifest 结构固定 ({"version":"..","url":"..","size":N}), 不引入 cJSON。
 * 成功返回 ESP_OK。
 */
static esp_err_t json_get_string(const char *json, const char *key,
                                 char *out, size_t out_len)
{
    /* 找 "key" */
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return ESP_ERR_INVALID_ARG;

    const char *p = strstr(json, pat);
    if (!p) return ESP_FAIL;
    p += n;

    /* 跳过空白与冒号 */
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return ESP_FAIL;   /* 期望字符串值 */
    p++;

    /* 拷贝到下一个未转义的引号 */
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    if (*p != '"') return ESP_FAIL;   /* 没正常闭合 */
    out[i] = '\0';
    return ESP_OK;
}

/* 从 manifest JSON 里取出 version 与 url。成功返回 ESP_OK。 */
static esp_err_t parse_manifest(const char *json,
                                char *ver, size_t ver_len,
                                char *url, size_t url_len)
{
    if (json_get_string(json, "version", ver, ver_len) != ESP_OK) {
        ESP_LOGE(TAG, "manifest 缺少 version 字段");
        return ESP_FAIL;
    }
    if (json_get_string(json, "url", url, url_len) != ESP_OK) {
        ESP_LOGE(TAG, "manifest 缺少 url 字段");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 执行一次完整的固件下载 + 写入 + 切槽。成功后调用方应重启。 */
static esp_err_t do_ota(const char *firmware_url, const char *running_ver)
{
    esp_http_client_config_t http_cfg = {
        .url = firmware_url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 读新固件内嵌的 app 描述 (含版本), 与当前版本再核对一次 */
    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
        ESP_LOGI(TAG, "新固件版本=%s (当前=%s)", new_desc.version, running_ver);
        if (semver_cmp(new_desc.version, running_ver) == 0) {
            ESP_LOGW(TAG, "固件内嵌版本与当前相同, 放弃更新 (防反复刷写)");
            esp_https_ota_abort(handle);
            return ESP_ERR_INVALID_VERSION;
        }
    }

    int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "开始下载固件, 大小=%d bytes", image_size);

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int read = esp_https_ota_get_image_len_read(handle);
        if (image_size > 0) {
            int pct = (int)((int64_t)read * 100 / image_size);
            if (pct != last_pct && pct % 10 == 0) {
                ESP_LOGI(TAG, "下载进度 %d%% (%d/%d)", pct, read, image_size);
                last_pct = pct;
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "下载/写入失败: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "数据不完整, 中止");
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        /* ESP_ERR_OTA_VALIDATE_FAILED: 镜像校验失败 (损坏/非法) */
        ESP_LOGE(TAG, "esp_https_ota_finish 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA 写入完成, boot 分区已切换, 即将重启");
    return ESP_OK;
}

esp_err_t ota_check_and_update(const char *host, uint16_t port)
{
    if (!host || host[0] == '\0') return ESP_ERR_INVALID_ARG;

    char running_ver[32];
    ota_get_running_version(running_ver, sizeof(running_ver));

    /* 1) 拉 manifest */
    char json[512];
    int status = 0;
    esp_err_t err = http_client_get_text_hp(host, port, OTA_MANIFEST_PATH,
                                            json, sizeof(json), &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "拉取 manifest 失败 (status=%d): %s", status, esp_err_to_name(err));
        return err;
    }

    char remote_ver[32];
    char firmware_url[192];
    if (parse_manifest(json, remote_ver, sizeof(remote_ver),
                       firmware_url, sizeof(firmware_url)) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 2) 版本比较 */
    int cmp = semver_cmp(remote_ver, running_ver);
    ESP_LOGI(TAG, "版本检查: 本地=%s 远端=%s -> %s",
             running_ver, remote_ver,
             cmp > 0 ? "有新版本" : (cmp == 0 ? "已是最新" : "远端更旧, 忽略"));
    if (cmp <= 0) {
        return ESP_OK;   /* 无需更新 */
    }

    /* 3) 下载并更新 */
    ESP_LOGI(TAG, "从 %s 下载新固件", firmware_url);
    err = do_ota(firmware_url, running_ver);
    if (err == ESP_OK) {
        esp_restart();   /* 不返回 */
    }
    return err;
}
