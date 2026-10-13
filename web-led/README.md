# web-led · 网页控制台灯

只用 ESP32 本身的第一个项目：板载 WS2812 全彩灯 + 局域网网页控制（开关 / 调光），亮灯颜色为烧鹅品牌暖橙 #C2410C，界面用品牌色，带日/夜间模式。零外件。

目标板子：**ESP32-S3-DevKitC-1 N16R8**（板载 WS2812 全彩灯，GPIO 21）。

## 烧录步骤（Arduino IDE）

1. **装 ESP32 支持**：文件 → 首选项 → 「附加开发板管理网址」填入
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   然后 工具 → 开发板 → 开发板管理器 → 搜 `esp32` → 安装「esp32 by Espressif Systems」。
2. **装灯库**：工具 → 管理库 → 搜 `Adafruit NeoPixel` → 安装「Adafruit NeoPixel」。
3. **改凭据**：复制 `src/secrets.h.example` 为 `src/secrets.h`，填入你的 WiFi 名和密码（`secrets.h` 已被 .gitignore 忽略，不会进仓库）。
4. **选板子**：工具 → 开发板 → `ESP32S3 Dev Module`；Flash Size 选 `16MB`，PSRAM 选 `OPI PSRAM 8MB`（对应 N16R8）；USB CDC On Boot 选 `Enabled`。
5. **烧录**：选对端口 → 点「上传」。
6. **看 IP**：工具 → 串口监视器，波特率 115200，等它打印 `已连接！浏览器打开: http://192.168.x.x`。
7. **用手机/电脑浏览器打开该 IP** 即可控制台灯。

## 功能

- 网页开关 LED、0–100% 调光
- 亮灯颜色 = 品牌暖橙 #C2410C（改 `src/web-led.ino` 里的 `BRAND_R/G/B` 可换色）
- 板载 BOOT 键按下也能切换开关
- 日/夜间模式（记住在浏览器 localStorage）

## 常见情况

- **灯不亮**：确认是 N16R8 版（板上有全彩灯）；若换用其他 S3 板子，按注释改 `src/web-led.ino` 里的 `LED_PIN`。
- **想显示别的颜色**：改 `BRAND_R/G/B` 三个值（0~255）。
- **板子没有板载灯**：接 WS2812 灯珠到 GPIO 21（IN 接 GPIO 21，VCC 接 3.3V/GND 看灯珠规格），或接普通 LED + 220Ω 电阻到 GPIO 2 并改回 PWM 版本（找我换）。
