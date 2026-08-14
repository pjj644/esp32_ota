# ESP32 本地 OTA 服务器

这个 Node.js 服务器运行在本地电脑上，为 ESP32 提供：

- `GET /hello` —— 心跳测试，返回 `Hello from Local PC @ ...`
- `GET /ota/manifest` —— 返回当前固件版本与下载 URL
- `GET /ota/firmware.bin` —— 流式发送固件二进制文件

## 前置条件

1. ESP32 与电脑连接 **同一个 WiFi / 局域网**。
2. 电脑上已安装 Node.js（>= 18）。
3. Windows 防火墙允许 Node.js 访问网络（首次运行会弹窗）。

## 快速开始

### 1. 查看并填写本地 IP

双击 `show_ip.bat`，找到当前电脑的局域网 IPv4（如 `10.167.197.162`）。

然后打开 `../main/main.c`，把 `LOCAL_HOST` 改成这个 IP：

```c
#define LOCAL_HOST  "192.168.1.11"
```

如果电脑的 IP 变了，需要重新修改这里并重新烧录 ESP32。

### 2. 确认 WiFi 凭证

打开 `../main/wifi.h`，确认 `WIFI_SSID` 和 `WIFI_PASS` 是 ESP32 与电脑共同连接的 WiFi：

```c
#define WIFI_SSID  "YOUR_LOCAL_SSID"
#define WIFI_PASS  "YOUR_LOCAL_PASS"
```

### 3. 启动服务器

双击 `start_server.bat`，或在 PowerShell 中执行：

```powershell
cd D:\esp32\esp_project\local_test\server
node server.js
```

成功后会看到：

```
[esp32-test-server] listening on http://0.0.0.0:8888
```

### 4. 编译并烧录 ESP32

```powershell
cd D:\esp32\esp_project\local_test
idf.py build
idf.py -p COM? flash monitor
```

烧录后，ESP32 会连接 WiFi，然后每 10 秒访问一次 `/hello`，每 60 秒检查一次 OTA。

## 发布新固件（触发 OTA）

1. 在 `../sdkconfig.defaults` 中提升版本号，例如从 `1.0.1` 改为 `1.0.2`：

   ```
   CONFIG_APP_PROJECT_VER="1.0.2"
   ```

2. 重新编译：

   ```powershell
   idf.py build
   ```

3. 双击 `deploy_firmware.bat`，把 `build/local_test.bin` 复制到 `firmware/firmware.bin`。

4. 手动更新 `firmware/version.json`：

   ```json
   {"version":"1.0.2"}
   ```

   **注意**：`version.json` 里的版本号必须和 `sdkconfig.defaults` 里的 `CONFIG_APP_PROJECT_VER` 一致。

5. 等待 ESP32 下次 OTA 检查（最多 60 秒），就会下载新固件并自动重启。

## 手动验证服务器

在 PowerShell 中执行：

```powershell
curl http://10.167.197.162:8888/hello
curl http://10.167.197.162:8888/ota/manifest
```

请把 `10.167.197.162` 替换为当前电脑的局域网 IP。

## 常见问题

| 现象 | 排查 |
|---|---|
| ESP32 无法访问 `/hello` | 检查 `main.c` 里的 `LOCAL_HOST` 是否填写正确；检查 Windows 防火墙是否放行 Node.js；确认 ESP32 与电脑在同一网络。 |
| manifest 能拿到，但下载固件失败 | 服务器返回的 `url` 来自请求的 `Host` 头，确认 ESP32 能解析/访问该 IP。 |
| 固件反复刷写 | 检查 `sdkconfig.defaults` 里的 `CONFIG_APP_PROJECT_VER` 是否真的大于当前运行版本。 |
| 下载进度卡住 | 可能是固件文件不存在；确认已运行 `deploy_firmware.bat`。 |
