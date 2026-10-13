# blink3 · 三色 LED 定时闪烁最小示例

共阳 RGB 灯珠（三色公共端接 3V3），三种周期，全部非阻塞：

| 灯 | 周期 | 每次亮多久 |
|---|---|---|
| 红 | 1 秒 | 100 ms |
| 蓝 | 1 分钟 | 100 ms |
| 绿 | 1 小时 | 100 ms |

到点亮、亮够即灭，三个灯互不干扰，`loop()` 里没有任何 `delay()`。

## 假设（与代码一致，改代码时同步改这里）

- **开发板**：ESP32-S3-DevKitC-1（N16R8，16MB Flash + 8MB PSRAM）。选它是因为本仓库（Siungo-ESP32）的 web-led 项目就用这块板；代码只用了标准 Arduino API，换 Arduino Uno 时只需改引脚常量，其余原样。
- **开发环境**：VS Code + PlatformIO（Arduino 框架），`platformio.ini` 与 web-led 保持一致；零第三方库。
- **LED 接法**：共阳 RGB 模块，**公共脚（长脚）→ 3V3，各色阴极 → 限流电阻 → GPIO**，低电平点亮（active LOW）。若手头是**分立 LED 共阴接法**（阴极接 GND），把 `src/main.cpp` 里三处 `digitalWrite` 的 `HIGH`/`LOW` 对调即可。

## 接线

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
| `PIN_LED_RED / BLUE / GREEN` | 4 / 5 / 6 | 三个 GPIO |
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

VS Code 装了 PlatformIO IDE 插件后，用底部状态栏的 →（上传）、⌂（监视器）亦可。代码只含标准 Arduino API，也可整段复制进 Arduino IDE 当 .ino 用（开发板选 `ESP32S3 Dev Module`，Flash Size 16MB、PSRAM OPI 8MB）。

## 验证方法

**生产模式（TIME_SCALE = 1）**

1. 烧录后串口应打出 `blink3: RED=1s / BLUE=1min / GREEN=1h, non-blocking`。
2. 红灯：每秒一次 100 ms 短亮，数 10 秒应有 10 次。
3. 蓝灯：第 60 秒、第 120 秒各亮一次 —— 等 2 分钟即可确认分钟级逻辑。

**小时级怎么快速验（TIME_SCALE = 60）**

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

## 常见问题

- **全不亮**：先看串口有没有日志——有日志说明固件在跑，查接线（LED 长短脚、电阻、GPIO 编号）；没日志查板子型号（`ESP32S3 Dev Module`）和 USB 端口。
- **某色不亮**：多数是 LED 插反（共阳时公共长脚应接 3V3，阴极接电阻侧）或该 GPIO 没接对。
- **蓝/绿偏暗**：3.3V 下蓝/绿 Vf 高，100 Ω 只剩约 3 mA，换 68 Ω。
- **用分立 LED（共阴接法）**：`digitalWrite` 的 HIGH/LOW 对调（见"假设"）。
