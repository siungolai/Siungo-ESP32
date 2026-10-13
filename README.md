# Siungo-ESP32

烧鹅的 ESP32 开发项目仓库。每个子目录一个独立 PlatformIO 项目，各自带 README，可单独编译、单独烧录。

## 项目

| 目录 | 一句话 | 目标板 |
|---|---|---|
| [blink3](blink3/) | 三色 LED 定时闪烁最小示例 —— 红 1s / 蓝 1min / 绿 1h，全非阻塞 | ESP32-S3-DevKitC-1 N16R8 |
| [blink3-test](blink3-test/) | 共阳 / 共阴硬件判定测试（A 段全 LOW、B 段全 HIGH 交替，看哪段灯全亮） | ESP32-S3-DevKitC-1 N16R8 |
| [web-led](web-led/) | 板载 WS2812 网页控制台灯 —— 局域网开关 + 调光，品牌暖橙 #C2410C，日/夜模式 | ESP32-S3-DevKitC-1 N16R8 |

三个项目都只用标准 Arduino API 或单一知名库（web-led 用 Adafruit NeoPixel），`platformio.ini` 互相保持一致。

## 通用约定

- **开发环境**：VS Code + PlatformIO（Arduino 框架）。没有装 PlatformIO IDE 也能用命令行：
  ```bash
  cd <项目目录>
  pio run              # 编译
  pio run -t upload    # 烧录
  pio device monitor   # 串口监视，115200
  ```
- **板子配置**：16MB Flash + 8MB OPI PSRAM，`-DARDUINO_USB_CDC_ON_BOOT=1`，串口走板子原生 USB。
- **凭据**：WiFi 名密码写在 `src/secrets.h`，已进 `.gitignore`；仓库只留 `src/secrets.h.example` 模板。**永远不要把 secrets.h 提交上来。**
- **构建产物**：`.pio/` 已忽略，不入库。

## 从哪个开始

- 只想点个灯 → `blink3`（README 里有接线图、限流电阻取值、`TIME_SCALE` 台架验证法）
- 灯不亮 / 不知道手里的 RGB 模块是共阳还是共阴 → `blink3-test`（1 分钟出结论）
- 想联网控制 → `web-led`（烧录后浏览器开板子打印的 IP）
