# pin-probe · 板载 WS2812 通路自检

临时判定项目：**只回答一个问题——这块板的板载全彩灯到底接在哪个 GPIO？** 查完就可以删。

配合 `blink3` 的"板载灯不亮"排查使用。`blink3` 本身已经不是"先天不亮"，见 `blink3/README.md` 的常见问题一节。

## 判什么

上电后循环两段，每段把同一串颜色（白 → 红 → 绿 → 蓝）打到不同的 GPIO：

| 段 | GPIO | 说明 |
|---|---|---|
| 1 | 21 | 常见的外接 WS2812 灯珠接法 |
| 2 | 48 | ESP32-S3-DevKitC-1 官方板载 WS2812 的引脚 |

### 结论表

| 现象 | 结论 |
|---|---|
| **只有第 1 段亮** | 你的灯是**外接**在 **GPIO 21** 的 |
| **只有第 2 段亮** | 灯是**板载**的，数据脚 **GPIO 48** → `blink3` 板载模式默认就是它，直接用 |
| **两段都不亮** | 硬件问题：这块板没有板载 WS2812（S3-DevKitC-1 **v1.1 起取消了板载灯**），或灯/走线损坏 |

> 依据：ESP32-S3-DevKitC-1 **v1.0** 的 WS2812 数据脚是 **GPIO 48**。
> 另外 Arduino core 2.0.17 的 S3 变体给出 `PIN_NEOPIXEL = 48`。两种来源都指向 48。
>
> **2026-10-14 实测结论：本机这块板是 v1.0，板载灯在 GPIO 48，第 2 段亮、第 1 段不亮。**

## 为什么不能写 `RGB_BUILTIN`

Arduino core 2.0.17 的 S3 变体里：

```c
static const uint8_t LED_BUILTIN = SOC_GPIO_PIN_COUNT + PIN_NEOPIXEL;   // 48 + 48 = 96
#define RGB_BUILTIN LED_BUILTIN                                         // 96
```

而 `cores/esp32/esp32-hal-rgb-led.c` 里：

```c
if (pin == RGB_BUILTIN) { _pin = RGB_BUILTIN - SOC_GPIO_PIN_COUNT; }    // 96 - 48 = 48 ✅
```

`SOC_GPIO_PIN_COUNT` 是在**运行时**按芯片取值的：S3 恰好是 48，于是 `96 - 48 = 48`，**结论是对的**。
但这属于"依赖编译器常量与芯片常量凑巧相等"，只要换到别的 S3/C3 变体就可能错位。
本项目一律传字面量（21 / 48），不经过那行改写，行为确定。

## 编译烧录

```bash
cd pin-probe
pio run -t upload
pio device monitor   # 115200
```

板子配置与 `blink3` 相同（ESP32-S3-DevKitC-1 N16R8，原生 USB 串口），零第三方依赖。

## 串口输出长这样

```
pin-probe: 板载 WS2812 通路自检开始
先看哪一段把灯点亮 —— 第 1 段=GPIO21，第 2 段=GPIO48
>>> 阶段 1/2  用 GPIO 21
    GPIO 21  WHITE  (255,255,255)
    GPIO 21  RED    (255,0,0)
    GPIO 21  GREEN  (0,255,0)
    GPIO 21  BLUE   (0,0,255)
>>> 阶段 2/2  用 GPIO 48
    GPIO 48  WHITE  (255,255,255)
    ...
```
