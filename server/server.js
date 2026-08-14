/*
 * ESP32 网络测试服务 (Node 内置 http/fs, 0 依赖)
 *
 * 端点:
 *   GET /hello         -> 200, 返回 "Hello from Local PC @ <UTC ISO-8601>\n"
 *   GET /ota/manifest  -> 200, JSON {version, url, size}  (读 firmware/version.json)
 *   GET /ota/firmware.bin -> 200, application/octet-stream  (流式发送 ESP32 固件)
 *   GET /ota/stm32_manifest -> 200, JSON {version, url, size} (读 firmware/stm32_version.json)
 *   GET /ota/stm32.bin  -> 200, application/octet-stream  (流式发送 STM32 固件)
 *   其他               -> 404
 *
 * OTA 目录布局 (与 server.js 同级):
 *   firmware/
 *     ├── version.json        {"version":"1.0.1"}   <- 发布脚本更新 (ESP32)
 *     ├── firmware.bin        idf.py build 产物 local_test.bin 的副本 (ESP32)
 *     ├── stm32_version.json  {"version":"1.0.0"}   <- 发布脚本更新 (STM32)
 *     └── stm32.bin           STM32 编译产物 (arm-none-eabi-objcopy 生成) 的副本
 *
 * manifest 里的 url 用请求的 Host 动态拼出, 免得写死 IP。
 *
 * 本地使用:
 *   1. 确保 ESP32 与电脑连接同一 WiFi。
 *   2. 把 main/main.c 里的 LOCAL_HOST 改成当前电脑的局域网 IP。
 *   3. 在此目录执行: node server.js   (或 npm start)
 *   4. 首次运行请在 Windows 防火墙提示中允许 Node.js 访问网络。
 *   5. 发布新固件: 运行 deploy_firmware.bat (需先 idf.py build)。
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 8888;
const HOST = '0.0.0.0';

const FW_DIR = path.join(__dirname, 'firmware');
const FW_BIN = path.join(FW_DIR, 'firmware.bin');
const FW_VER = path.join(FW_DIR, 'version.json');
const STM32_BIN = path.join(FW_DIR, 'stm32.bin');
const STM32_VER = path.join(FW_DIR, 'stm32_version.json');

function sendText(res, code, body) {
  res.writeHead(code, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Connection': 'close',
  });
  res.end(body);
}

function sendJson(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Connection': 'close',
  });
  res.end(body);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const log = (code) =>
    console.log(`${new Date().toISOString()} ${req.method} ${req.url} -> ${code} from ${req.socket.remoteAddress}`);

  // GET /hello
  if (req.method === 'GET' && url.pathname === '/hello') {
    sendText(res, 200, `Hello from Local PC @ ${new Date().toISOString()}\n`);
    log(200);
    return;
  }

  // GET /ota/manifest -> 读 version.json + firmware.bin 大小, 拼出固件下载 url
  if (req.method === 'GET' && url.pathname === '/ota/manifest') {
    let ver;
    try {
      ver = JSON.parse(fs.readFileSync(FW_VER, 'utf8'));
    } catch (e) {
      sendJson(res, 500, { error: 'no version.json', detail: String(e) });
      log(500);
      return;
    }
    let size = 0;
    try { size = fs.statSync(FW_BIN).size; } catch (e) { /* 固件还没上传 */ }

    const host = req.headers.host || `${req.socket.localAddress}:${PORT}`;
    sendJson(res, 200, {
      version: ver.version,
      url: `http://${host}/ota/firmware.bin`,
      size,
    });
    log(200);
    return;
  }

  // GET /ota/firmware.bin -> 流式发送固件
  if (req.method === 'GET' && url.pathname === '/ota/firmware.bin') {
    fs.stat(FW_BIN, (err, st) => {
      if (err) {
        sendText(res, 404, 'firmware not found\n');
        log(404);
        return;
      }
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': st.size,
        'Connection': 'close',
      });
      const stream = fs.createReadStream(FW_BIN);
      stream.pipe(res);
      stream.on('error', (e) => {
        console.error('firmware stream error:', e);
        res.destroy();
      });
      log(200);
    });
    return;
  }

  // GET /ota/stm32_manifest -> 读 stm32_version.json + stm32.bin 大小, 拼出下载 url
  if (req.method === 'GET' && url.pathname === '/ota/stm32_manifest') {
    let ver;
    try {
      ver = JSON.parse(fs.readFileSync(STM32_VER, 'utf8'));
    } catch (e) {
      sendJson(res, 500, { error: 'no stm32_version.json', detail: String(e) });
      log(500);
      return;
    }
    let size = 0;
    try { size = fs.statSync(STM32_BIN).size; } catch (e) { /* 固件还没上传 */ }

    const host = req.headers.host || `${req.socket.localAddress}:${PORT}`;
    sendJson(res, 200, {
      version: ver.version,
      url: `http://${host}/ota/stm32.bin`,
      size,
    });
    log(200);
    return;
  }

  // GET /ota/stm32.bin -> 流式发送 STM32 固件
  if (req.method === 'GET' && url.pathname === '/ota/stm32.bin') {
    fs.stat(STM32_BIN, (err, st) => {
      if (err) {
        sendText(res, 404, 'stm32 firmware not found\n');
        log(404);
        return;
      }
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': st.size,
        'Connection': 'close',
      });
      const stream = fs.createReadStream(STM32_BIN);
      stream.pipe(res);
      stream.on('error', (e) => {
        console.error('stm32 firmware stream error:', e);
        res.destroy();
      });
      log(200);
    });
    return;
  }

  // 404 兜底
  sendText(res, 404, 'not found\n');
  log(404);
});

server.listen(PORT, HOST, () => {
  console.log(`[esp32-test-server] listening on http://${HOST}:${PORT}`);
  console.log(`[esp32-test-server] firmware dir: ${FW_DIR}`);
  console.log(`[esp32-test-server] try:  curl http://<your-pc-ip>:${PORT}/ota/manifest`);
});

// 捕获致命错误, 保持服务运行
process.on('uncaughtException', (err) => {
  console.error('uncaughtException:', err);
});
process.on('unhandledRejection', (err) => {
  console.error('unhandledRejection:', err);
});
