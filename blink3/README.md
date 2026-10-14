# blink3 · 三色定时闪烁最小示例

一颗灯，三种周期，全部非阻塞：

| 灯 | 周期 | 每次亮多久 |
|---|---|---|
| 红 | 1 秒 | 100 ms |
| 蓝 | 1 分钟 | 100 ms |
| 绿 | 1 小时 | 100 ms |

到点亮、亮够即灭，三个灯互不干扰，`loop()` 里没有任何 `delay()`（只有开机自检用了一次短 `delay`，可用 `ONBOARD_SELFTEST 0` 关掉）。

## 两种模式（`ENABLE_ONBOARD_RGB` 开关）

| 模式 | 值 | 硬件 | 点亮方式 | 依赖 |
|---|---|---|---|---|
| **板载 WS2812**（默认） | `1` | 板上那一颗全彩灯（GPIO 48） | 一颗灯按 红/蓝/绿 依次换色 | Adafruit NeoPixel |
| **外接三色 LED** | `0` | 分立 RGB，共阳接法，GPIO 4/5/6 | 三路各自开关 | 零依赖 |

改 `src/main.cpp` 顶部一行即可切换：

```c
#define ENABLE_ONBOARD_RGB 1    // 1 = 板载灯；0 = 外接三灯
```

## 假设（与代码一致，改代码时同步改这里）

- **开发板**：ESP32-S3-DevKitC-1（N16R8，16MB Flash + 8MB PSRAM）。同板只跑本项目；外接三灯模式只用了标准 Arduino API，换 Arduino Uno 时只需改引脚常量，其余原样。
- **开发环境**：VS Code + PlatformIO（Arduino 框架），`platformio.ini` 与本仓库其他 ESP32-S3 项目保持一致。
- **板载灯**：WS2812 单颗，数据脚 **GPIO 48**（`PIN_NEOPIXEL`）。**只有 v1.0 有这颗灯，v1.1 起取消了**——详见「常见问题」。
- **外接 LED 接法**（仅 `ENABLE_ONBOARD_RGB 0`）：共阳 RGB 模块，**公共脚（长脚）→ 3V3，各色阴极 → 限流电阻 → GPIO**，低电平点亮（active LOW）。若手头是**分立 LED 共阴接法**（阴极接 GND），把 `setLamp()` 里的 `on ? LOW : HIGH` 对调即可。

## 板载灯模式：接线为零，但要认识两个坑

### 坑一：板载灯不一定存在

ESP32-S3-DevKitC-1 的 **v1.0** 板上有 1 颗 WS2812；**v1.1 起官方把这个灯去掉了**。
板子上如果没有那颗 5050 全彩灯，代码写得再对也不会亮。

### 坑二：引脚数据脚是 48，不是 21

板载 WS2812 的数据脚是 **GPIO 48**（Arduino core 的 `PIN_NEOPIXEL`）。

> 注：**板载灯在 48，外接灯珠按你的实际接线。**
> 两者不是一回事：**焊在板上的那颗灯是 48，你手接的灯珠接你选的那个脚**。
> 到底哪边亮，烧 `pin-probe/` 一试便知。

```c
// src/main.cpp
#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 48
#endif
static const uint8_t PIN_ONBOARD = (uint8_t)PIN_NEOPIXEL;
```

**不要**把引脚写成核心的 `LED_BUILTIN` / `RGB_BUILTIN`。core 2.0.17 的 S3 变体里：

```c
static const uint8_t LED_BUILTIN = SOC_GPIO_PIN_COUNT + PIN_NEOPIXEL;   // 48 + 48 = 96
#define RGB_BUILTIN LED_BUILTIN                                         // 96
```

而 `cores/esp32/esp32-hal-rgb-led.c` 会做：

```c
if (pin == RGB_BUILTIN) { _pin = RGB_BUILTIN - SOC_GPIO_PIN_COUNT; }    // 96 - 48 = 48
```

数值上碰巧算回 48，但那是在赌「编译期常量 == 芯片运行时常量」；换变体就可能错位。
本项目一律传字面量，完全绕开那行改写。

## 用电安全

S3 的 IO 是 **3.3V 电平**。把 GPIO 直接短到 GND 或 3V3 会烧 IO。
调试接线时先接线后上电，别在通电状态下捏着杜邦线乱捅。

## 接线（外接三色 LED 模式）

| 颜色 | 公共端（长脚） | 阴极（短脚） | 限流电阻 |
|---|---|---|---|
| 红 | 3V3 | 经 220 Ω 接 GPIO 4 | 220 Ω |
| 蓝 | 3V3 | 经 100 Ω 接 GPIO 5 | 100 Ω |
| 绿 | 3V3 | 经 100 Ω 接 GPIO 6 | 100 Ω |

电阻取值按 `(3.3 V − Vf) ÷ 5 mA` 估算（红 Vf≈2.0 V，蓝/绿 Vf≈3.0 V），单 GPIO 电流约 3–6 mA，远在安全范围内。蓝/绿若偏暗，把 100 Ω 换成 68 Ω 或 47 Ω。

```
3V3 ──►|──[220Ω]── GPIO4     红
3V3 ──►|──[100Ω]── GPIO5     蓝
3V3 ──►|──[100Ω]── GPIO6     绿
```

## 可调参数（src/main.cpp 顶部）

| 常量 | 默认值 | 说明 |
|---|---|---|
| `ENABLE_ONBOARD_RGB` | 1 | 1 = 板载 WS2812；0 = 外接三色 LED |
| `PIN_ONBOARD` | 48 | 板载 WS2812 数据脚（取 `PIN_NEOPIXEL`） |
| `ONBOARD_BRIGHTNESS` | 64 | 板载灯亮度，0–255。板载灯直怼 3.3V 很刺眼 |
| `ONBOARD_SELFTEST` | 1 | 开机先白闪两下自检；0 = 关掉 |
| `PIN_LED_RED / BLUE / GREEN` | 4 / 5 / 6 | 外接模式三个 GPIO |
| `PERIOD_RED_MS` | 1000 | 红闪周期 |
| `PERIOD_BLUE_MS` | 60000 | 蓝闪周期 |
| `PERIOD_GREEN_MS` | 3600000 | 绿闪周期 |
| `ON_DURATION_MS` | 100 | 每次亮灯时长 |
| `TIME_SCALE` | 1 | 时间压缩倍数，仅用于台架验证，见下 |

## 编译烧录（PlatformIO）

```bash
cd blink3
pio run                  # 编译
pio run -t upload        # 烧录（板子插着 USB）
pio device monitor       # 看串口日志，波特率 115200
```

VS Code 装了 PlatformIO IDE 插件后，用底部状态栏的 →（上传）、⌂（监视器）亦可。
外接三灯模式（`ENABLE_ONBOARD_RGB 0`）只含标准 Arduino API，可整段复制进 Arduino IDE 当 .ino 用
（开发板选 `ESP32S3 Dev Module`，Flash Size 16MB、PSRAM OPI 8MB）；
板载模式需要额外装「Adafruit NeoPixel」库。

## 验证方法

### 板载灯模式

1. **先把自检当第一道关**：上电瞬间板载灯应**白闪两下**（亮 150 ms 灭 150 ms）。
   - 白闪出现 → 板载灯通路完全正常，接着看下面的周期闪烁。
   - 白闪不出现 → 先别怀疑代码。见「常见问题」，直接烧 `pin-probe/` 判定是 GPIO 号问题还是板上没这颗灯。
2. 串口应打印 `blink3: 板载 WS2812 @ GPIO 48 , RED=1s / BLUE=1min / GREEN=1h, non-blocking`。
3. 红灯：每秒一次 100 ms 短亮，数 10 秒应有 10 次（板载灯同一时刻只能显示一种颜色，所以是**依次**出现红/蓝/绿）。
4. 蓝灯：第 60 秒、第 120 秒各闪一次——等 2 分钟即可确认分钟级逻辑。

> **2026-10-14 实机验证通过**（ESP32-S3-DevKitC-1，GPIO 48）：
> 上电白闪两下 → 每秒红闪 → `BLUE ON` 落在 `t=120701 ms` 与 `t=180701 ms`，
> 恰好相隔 60000 ms；`RED ON` 间隔稳定 1000 ms。红蓝偶尔同毫秒出现是因为 1s 与 60s 的最小公倍数是 60s，
> 撞点属设计使然。

### 外接三灯模式

同一套时序，只是三种颜色由三路 GPIO 分别驱动、可以同时亮。

### 小时级怎么快速验（`TIME_SCALE = 60`）

干等一小时不现实。把 `#define TIME_SCALE 1` 改成 `60` 重新烧录，三个周期同比压缩 60 倍：红 16 ms、蓝 1 s、绿 60 s，**一轮 60 秒就能把三种周期全验完**：

- 串口里 `BLUE ON` 每 1000 ms 一条、间隔稳定；
- `GREEN ON` 恰好在 `t=60000 ms` 出现一次；
- `RED ON` 约每 16 ms 一条（日志很多，属预期，只看时间戳规律即可）；
- 三个灯的事件时间戳互不错位 → 三套定时互不干扰、1:60:3600 的周期比例成立 → 逻辑正确。

> 验证完把 `TIME_SCALE` 改回 `1` 重新烧录。忘记改回只是闪得快，不损坏硬件，但别用在正式场合。

## 工作原理（三句话）

- 每个灯是一个小状态机：记下本轮起点 `cycleStartMs`，`millis() - cycleStartMs >= periodMs` 就到点开灯，亮够 `onMs` 关灯；
- 周期用累加（`cycleStartMs += periodMs`）推进，不取"当前时间"，避免误差累积导致越闪越慢；
- 全程只读 `millis()`，无任何 `delay()`；无符号减法天然兼容 `millis()` 约 49.7 天的回绕。

点亮/熄灭统一走 `setLamp(b, on)`：板载模式下就是"给这颗灯换颜色"（`setPixelColor` + `show`），外接模式下就是 `digitalWrite`。

## 常见问题

### 板载灯完全不亮

按这个顺序查，一步一刀，不要跳步：

1. **`ONBOARD_SELFTEST` 是不是 0？** 是的话改回 1 重新烧，看有没有白闪。
2. **烧 `pin-probe/`**（`cd pin-probe && pio run -t upload`）。它把 GPIO 21（外接）和 GPIO 48（板载）轮流点亮：
   - 只有 GPIO 48 那段亮 → 位置对，回到 blink3 板载模式即可；
   - 只有 GPIO 21 那段亮 → 你会用的是外接灯珠路径；
   - **两段都不亮 → 这块板没有板载 WS2812**。ESP32-S3-DevKitC-1 **v1.1 起官方取消了板载灯**。这不是代码问题，改用外接三色灯（`ENABLE_ONBOARD_RGB 0`）或外接 WS2812 灯珠接 GPIO 21。
3. **串口有没有输出？** 没有 → 查板子型号 / USB 端口 / `ARDUINO_USB_CDC_ON_BOOT`。有 → 固件在跑，问题在灯的通路。
4. **是不是烧的还是旧固件？** 看串口头一行：板载模式会打印 `板载 WS2812 @ GPIO 48`。

### 外接三灯某色不亮

多数是 LED 插反（共阳时公共长脚应接 3V3，阴极接电阻侧）或该 GPIO 没接对。

**判定共阳还是共阴**：不用烧别的程序，把三段的 GPIO 当成普通输出直接试 ——

- 逐个 GPIO 输出 **LOW**，灯亮 → **共阳**（公共脚接 3V3）
- 逐个 GPIO 输出 **HIGH**，灯亮 → **共阴**（公共脚接 GND）

原理：共阳的公共脚是阳极，GPIO 拉到低电平才能形成回路；共阴反过来。用万用表量公共脚对 GND 的通断也能判断，但直接试电平最快。

### 蓝/绿偏暗

3.3V 下蓝/绿 Vf 高，100 Ω 只剩约 3 mA，换 68 Ω。

### `TIME_SCALE` 忘记改回

只是闪得快，不损坏硬件。
