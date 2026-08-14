/*
 * ESP32 OTA 模块公开接口 (本地/远程服务器通用)。
 *
 * 依赖: 复用 http_client (拉 manifest) + esp_https_ota (下载固件)。
 * 需要自定义双 OTA 分区表 (partitions.csv) 与 sdkconfig.defaults 中的
 * CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP / CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE。
 */
#ifndef OTA_H
#define OTA_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 把当前运行固件的版本号 (CONFIG_APP_PROJECT_VER) 写入 buf。
 * buf 建议 >= 32 字节。
 */
void ota_get_running_version(char *buf, size_t buflen);

/**
 * 确认当前运行的固件健康 (取消回滚)。
 *
 * 开启回滚保护后, 新刷入的固件首次启动处于 PENDING_VERIFY 状态。
 * 应用应在自检通过 (如 WiFi 连上、关键服务可达) 后调用本函数;
 * 否则下次重启 bootloader 会自动回退到旧固件。
 *
 * 幂等: 非 PENDING_VERIFY 状态下直接返回 ESP_OK。
 *
 * @return ESP_OK 成功 (已标记 valid 或无需标记)
 */
esp_err_t ota_confirm_running_app(void);

/**
 * 向 host:port 查询 manifest 并在有新版本时执行 OTA。
 *
 * 若成功写入新固件, 本函数内部会 esp_restart() 且不返回。
 * 无新版本或失败时返回, 由调用方决定下次何时再查。
 *
 * @param host  服务器主机 (如本地电脑 "10.167.197.162")
 * @param port  端口 (如 8888); 传 0 省略端口
 * @return ESP_OK              已是最新版本 (无需更新)
 *         ESP_ERR_INVALID_ARG host 为空
 *         其他                 拉取/解析/下载失败 (见日志)
 */
esp_err_t ota_check_and_update(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
