# Siungo-ESP32

烧鹅的 ESP32 开发项目仓库。每个子目录一个独立 PlatformIO 项目，各自带 README，可单独编译、单独烧录。

## 项目

| 目录 | 一句话 | 目标板 |
|---|---|---|
| [blink3](blink3/) | 三色定时闪烁最小示例 —— 红 1s / 蓝 1min / 绿 1h，全非阻塞。**默认点板载 WS2812 全彩灯**，一个宏切回外接三色 LED | ESP32-S3-DevKitC-1 N16R8 |
| [pin-probe](pin-probe/) | 板载 WS2812 通路自检（GPIO 21 与 GPIO 48 轮流点亮，判定板载灯到底在哪、以及板上有没有这颗灯） | ESP32-S3-DevKitC-1 N16R8 |
| [uptime-oled](uptime-oled/) | 128x64 I2C OLED（**SSD1306**）显示通电时间，每 1s 刷新。SCL→GPIO4、SDA→GPIO5，零第三方依赖裸驱；附 `tools/i2c-scan`、`tools/bus-probe` 两个硬件诊断小工程 | ESP32-S3-DevKitC-1 N16R8 |
| [dht11-oled](dht11-oled/) | **DHT11 温湿度实时监测**，每 1s 刷新屏幕。DATA→**GPIO7**、OLED SCL→GPIO4 / SDA→GPIO5；零第三方依赖自写时序；弃用 Adafruit DHT 库的原因见其 README | ESP32-S3-DevKitC-1 N16R8 |
| [clock-oled](clock-oled/) | **联网时钟**：NTP 对时后显示时间与日期，每 1s 刷新，大号 HH:MM + 日期星期。每小时自动重新对时压漂移。OLED SCL→GPIO4 / SDA→GPIO5；**需先建 `src/secrets.h` 填 WiFi** | ESP32-S3-DevKitC-1 N16R8 |
| [pet-oled3](pet-oled3/) | **养成类掌上宠物**（策划阶段，尚未动工）：三块 SSD1306 分饰角色/数值/日志，经 **PCA9548A** 解决同址冲突，4x4 键盘出三个主动作，旋转编码器调参，蜂鸣器出音效 | ESP32-S3-DevKitC-1 N16R8 |
| [status-oled3](status-oled3/) | **桌面状态显示器**（策划阶段，尚未动工）：三块 SSD1306 分饰 ESP32 状态/阿里云服务器状态/邮箱未读数，经 **PCA9548A** 解决同址冲突；两个轻触按钮切换功能，有源蜂鸣器做通知提醒 | ESP32-S3-DevKitC-1 N16R8 |

硬件与工具链参数（含实测数据）见 **[HARDWARE.md](HARDWARE.md)** —— 器件规格、接线、驱动接口与踩坑记录。

`blink3` 的外接三灯模式与 `pin-probe`、`uptime-oled` 只用标准 Arduino API；`blink3` 的板载模式用 Adafruit NeoPixel。`platformio.ini` 互相保持一致。

`clock-oled` 用 Arduino core 自带的 `WiFi`（无外部库），OLED 驱动与 `uptime-oled` 同一份裸驱。

## 已废止的项目

| 目录 | 一句话 | 状态 |
|---|---|---|
| [lcd1602-uptime](lcd1602-uptime/) | 16x2 字符 LCD（LCD1602 + PCF8574 I2C 背包）显示通电时间 | **已废止，未上架** |

> 代码与调试记录保留下来，供以后 1602 再上场时参考（驱动、时序、诊断工具都是完整可用的），
> 但**硬件链路一直没有调通**，因此不计入本仓库的可用项目。
>
> 2026-10-14 实测：背光亮（供电到位）、SDA/SCL 纯输入读数均为 HIGH（外部上拉在线）、
> `i2c-scan` 扫 0x08~0x77 全段**零应答且零 ERR**（总线电气正常但器件无响应）。
> 当晚追加的**终极测试推翻了"硬件坏"这个初判**：把背包整块拔下来后，
> 背光**仍然亮**、两线**仍然高**——而背光由背包上的 PCF8574 驱动，
> 器件拔掉后不可能还亮。所以真正的原因在更前面一步：
> **A 排针从未真正插进面包板，或 K 背包与屏体的 16 针并口虚接**，
> 电流根本没经过 PCF8574。**"背光亮 = 供电到位"在本机不成立。**
> 下次要做的第一步是万用表量背包排针与屏体的通断，不是改固件。
> 详见 [`HARDWARE.md`](HARDWARE.md) §3.5。

## 板载 WS2812 在 GPIO 48（已实机验证）

这是本仓库最容易踩的坑，结论先放这里：

- **板上焊好的那颗全彩灯，数据脚是 GPIO 48**（Arduino core 的 `PIN_NEOPIXEL`）。
- **只有 ESP32-S3-DevKitC-1 v1.0 有板载灯**，v1.1 起官方取消了。板子上没这颗灯，代码再对也不亮。
- 你**自己外接**的 WS2812 灯珠接在任意 IO 上（本仓库曾用 GPIO 21），两者不是一回事。
- 引脚**不要**写成 `LED_BUILTIN` / `RGB_BUILTIN`。core 2.0.17 里它们等于 `SOC_GPIO_PIN_COUNT + PIN_NEOPIXEL`（= 96），
  再被 `esp32-hal-rgb-led.c` 减 `SOC_GPIO_PIN_COUNT` 还原——属于"赌编译期常量==运行时常量"的写法。传字面量最稳。

> 2026-10-14 已在实机上确认：`blink3` 用 GPIO 48 驱动板载灯，上电白闪两下自检通过，
> 之后每秒红闪、每 60 秒蓝闪，时序与设计一致。

拿不准手上这块板到底是哪种情况，烧 `pin-probe/`，两段一试便知。

## 通用约定

- **开发环境**：VS Code + PlatformIO（Arduino 框架）。没有装 PlatformIO IDE 也能用命令行：
  ```bash
  cd <项目目录>
  pio run              # 编译
  pio run -t upload    # 烧录
  pio device monitor   # 串口监视，115200
  ```
- **板子配置**：16MB Flash + 8MB OPI PSRAM，`-DARDUINO_USB_CDC_ON_BOOT=1`，串口走板子原生 USB。
- **凭据**：WiFi 名密码写在 `src/secrets.h`，已进 `.gitignore`；目录里只留 `src/secrets.h.example` 模板。**永远不要把 secrets.h 提交上来。**
- **构建产物**：`.pio/` 已忽略，不入库。
- **安全**：S3 的 IO 是 3.3V 电平。把 GPIO 直接短到 GND 或 3V3 会烧 IO；调试时先接线后上电。

## 从哪个开始

- 只想点个灯 → `blink3`（README 里有两种模式的说明、接线图、限流电阻取值、`TIME_SCALE` 台架验证法）
- **板载灯不亮** → `pin-probe`（1 分钟判定：是 GPIO 号问题，还是板上根本没这颗灯）
- 要用 OLED 显示运行时长 → `uptime-oled`（屏幕探测不到时会报清楚是接线还是地址问题，并附硬件诊断工具）
- **要显示时间与日期** → `clock-oled`（先建 `src/secrets.h` 填 WiFi，否则编译不过）
- **要测温湿度** → `dht11-oled`（DATA 接 GPIO7；屏幕会直接显示是"没接传感器"还是"线接错了"）
- **要做三块屏的项目 / 要玩 I2C 多路复用** → `pet-oled3`（**只有文档，尚无代码**；先读它的 `docs/策划案.md`，动手第一步是 M0 bringup）
- **要看服务器 / 邮箱 / 设备自身状态** → `status-oled3`（**只有文档，尚无代码**；三屏 + 按钮 + 蜂鸣器 + 聚合端点，先读它的 [`docs/adr/`](status-oled3/docs/adr/)，动手第一步是 M0 bringup）

## 许可

[MIT](LICENSE) © 2026 siungolai

各子项目引用的第三方库遵循其自身协议 —— 目前只有 `blink3` 用到 [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)（BSD-3-Clause，由 PlatformIO 按需拉取，未 vendor 进本仓库）。
