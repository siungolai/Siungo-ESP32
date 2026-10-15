# clock-oled · 128x64 I2C OLED 时钟（时间 + 日期）

联网用 NTP 校准时钟，然后**每 1 秒**刷新屏幕，大号显示时间、下面显示日期与星期。
挂在 **SSD1306**（128x64 I2C OLED）上。断电重连后自动重新对时。

> ⚠️ **首次烧录前必须先建 `src/secrets.h`**（填 WiFi 名密码），否则编译不过。
> 见下面「WiFi 配置」一节。

## 接线

| OLED 排针 | ESP32-S3 |
|---|---|
| SCL（时钟线） | **GPIO 4** |
| SDA（数据线） | **GPIO 5** |
| VCC | 3V3 |
| GND | GND |

> 与同仓库的 `uptime-oled` / `dht11-oled` **完全同一组引脚**，
> 换项目烧录不用动线。
>
> 引脚编号写在 `src/main.cpp` 顶部的 `PIN_I2C_SCL` / `PIN_I2C_SDA`。
> ESP32 的 `Wire.begin(sda, scl, freq)` 允许运行期指定任意 GPIO，
> 所以换脚只改这两个常量，不需要改 `platformio.ini`。
>
> **SCL 与 SDA 接反 = 完全不显示**，这是最常见的不亮原因。

## WiFi 配置（必做）

时间只能从网络来（原因见下面「时间从哪来」），所以必须配 WiFi：

```bash
cd clock-oled
cp src/secrets.h.example src/secrets.h   # Windows PowerShell: Copy-Item
```

然后编辑 `src/secrets.h`，把两个 `CHANGE_ME` 换成真实值：

```c
#define WIFI_SSID "你家的WiFi名"
#define WIFI_PASS "你家的WiFi密码"
```

> `secrets.h` 已在仓库根 `.gitignore` 里，**不会被提交**。
> ESP32-S3 **只支持 2.4GHz**，5GHz 的 SSID 连不上。

### 时区

`TZ_INFO` 用 POSIX 时区格式，默认已设成中国标准时间：

```c
#define TZ_INFO "CST-8"
```

> ⚠️ **符号方向容易搞反**：POSIX 里**正号表示西经**，
> UTC+8 要写**负号**（`CST-8`）。写成 `CST+8` 会显示成 UTC-8，
> 差 16 小时，是最容易踩的坑。
> 有夏令时的地区写成 `CST-8CDT,M4.1.0,M10.5.0` 这类规则即可，程序不用改。

## 屏幕布局

```
┌──────────────────────────────┐
│W SYNC 61s                #123│  ← 第 0 页：状态 + 刷新计数
│──────────────────────────────│  ← 第 1 页：分隔线
│  ██  ██  ██  ██  ██      :05 │
│  ██  ██  ██  ██  ██          │  ← 第 2~3 页：2 倍放大 HH:MM，右缘秒
│                              │  ← 第 4 页：留白
│2026-10-14              Tue   │  ← 第 5 页：ISO 日期 + 星期
│Tue 14 Oct 2026               │  ← 第 6 页：英文日期写法
│sync 61s ago  RSSI -52        │  ← 第 7 页：数据新鲜度 + 信号强度
└──────────────────────────────┘
```

- **大号 `HH:MM`**：屏幕的视觉主体，2 倍放大占两页。
- **右缘 `:SS`**：小字秒位，每秒在跳——"时钟活着"一眼可见。
- **`#123` 刷新计数**：每秒 +1。它就是"1 秒更新一次"的**现场证据**，
  数字在跑说明刷新周期真的在跑，而不是屏幕冻在某一帧。
- **状态行**：`W SYNC 61s` = WiFi 在线、距上次对时 61 秒；
  `w FREE-RUN 12min` = 断网、时间在自由走、已 12 分钟没校正过。

### 没有网络时长什么样

程序**不会**显示 1970 年那种假读数，而是给一块明确的排查屏：

```
┌──────────────────────────────┐
│CLOCK  NO NTP YET         #7  │
│──────────────────────────────│
│--:--                         │
│waiting for NTP               │
│check WiFi in                 │
│src/secrets.h                 │
└──────────────────────────────┘
```

同时后台每 5 秒重试一次 WiFi 与 NTP，连上就自动切回正常显示，
**不需要重新上电**。

## 时间从哪来（关键设计点）

**ESP32 没有带电池的 RTC 芯片，断电即失时间。** 所以：

| 阶段 | 时间来源 | 准确性 |
|---|---|---|
| 刚上电 | RTC 从 1970-01-01 起走 | 无意义 → 显示 `NO NTP YET` |
| NTP 成功后 | 写入系统时钟，**RTC 自己走字** | 与 NTP 服务器一致 |
| 断网期间 | RTC 自由走 | 按振荡器误差漂 |
| 恢复联网 | 立即重新对时 | 漂移被纠正 |

### 为什么会漂、漂多少

ESP32-S3 内部是 **RC 振荡器**，不是温补晶振。常见误差约 **±10 ppm**
（即每天约 ±1 秒）；没有外部晶振的板子可能到 **±100 ppm**（每天约 ±9 秒）。
温度变化会让它进一步漂。

因此本程序**每小时自动重新对时一次**（`CATCHUP_MS = 3600000`），
把累计漂移压回"看不见"的量级。每小时一次 DNS + UDP 往返的开销可忽略。

> 想要断电不丢时间，需要外挂 **DS3231** 这类带纽扣电池的 RTC 模块
> （I2C，可与 OLED 共用总线，挂在 0x68）。本示例没有这颗硬件，
> 所以走"联网对时 + 定期纠正"这条路。

### 为什么用 `settimeofday` / `configTzTime` 而不是手搓计时

ESP-IDF 自带系统时钟，`configTzTime()` 会把 POSIX 时区规则装进 C 库，
`localtime_r()` 直接把 UTC 换算成本地日期时间（含星期）。

**闰年、月末天数、时区规则全都不用自己算**——这几处恰恰是手写时钟最容易错的地方
（比如把 2100 年当闰年）。

## 刷新为什么不用 `delay(1000)`

判定用 `now - lastRefresh >= 1000`，**非阻塞**：

- 将来要加按键、蜂鸣器、传感器，主循环不用重构；
- 与 SSD1306 的 I2C 传输（约 25ms）天然解耦；
- 时间戳用 `lastRefresh += 1000` 而**不是** `= now`，
  避免某次刷新被 I2C 拖长后误差长期累积。

> 屏幕不在线时 `flush()` 直接返回（它每帧要发 8 页 × 2 次 I2C 事务，
> 从机不在线时每次都要阻塞到 50ms 超时，一轮近 800ms，足以把 1 秒节拍带偏）。
> 这条解耦是从 `uptime-oled` 沿用下来的实测结论。

## 编译烧录

```bash
cd clock-oled
pio run              # 编译
pio run -t upload    # 烧录（板子接 COM4，用板载原生 USB）
pio device monitor   # 串口监视 115200
```

依赖：只用 Arduino core 自带的 `Wire` + `WiFi`，**零第三方库**。
OLED 驱动是自写裸驱，与 `uptime-oled` 同一份代码。

## 串口输出长这样

```
=== clock-oled ===
I2C: SCL=GPIO4  SDA=GPIO5  400000 Hz
TZ: CST-8  NTP: ntp.aliyun.com / ntp.tencent.com / cn.pool.ntp.org
OLED init OK, refreshing every 1s.
WiFi: connecting to "MyHome" ...
WiFi: connected, IP=192.168.1.42  RSSI=-52 dBm
NTP: synced -> 2026-10-14 21:52:24
[0] 2026-10-14 21:52:24  wifi=up  age=0s
[1] 2026-10-14 21:52:25  wifi=up  age=1s
[2] 2026-10-14 21:52:26  wifi=up  age=2s
```

初始化失败时（屏幕不在线）会打印 ASCII 版排查清单：

```
[ERR] no ACK at 0x3C.
Check, in this order:
  1) SCL -> GPIO4 and SDA -> GPIO5 (swapped wires = no ACK)
  ...
[retry] probing OLED again...
```

> 日志刻意用 ASCII 而不是中文：中文经 UTF-8 送到部分串口工具会变乱码，
> 而这条信息恰恰是故障时唯一能看到的线索。

## 资源占用

| 项目 | 占用 |
|---|---|
| RAM | 46,144 字节 / 327,680（**14.1%**） |
| Flash | 720,125 字节 / 3,342,336（**21.5%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

> RAM / Flash 明显高于 `uptime-oled`（6.2% / 8.5%），
> 全部来自 WiFi 协议栈（`libWiFi`），与 OLED 或时钟逻辑无关。
> 这是联网对时的必然代价，属正常。

## 已知边界

- **没有电池备份的 RTC**：断电后必须重新联网对时，见上文。
- **SSID 用 5GHz 连不上**：ESP32-S3 只支持 2.4GHz。
- **NTP 需要能出 UDP 123**：部分企业网/校园网会封，此时只能长期
  `NO NTP YET`。可用手机热点验证。
- **首次同步最长等 20 秒**（`NTP_WAIT_MS`）：超过就先进主循环显示排查屏，
  不黑屏。联网慢的网络上看到排查屏几秒后自动恢复属正常。
- **星期与月份用固定 ASCII 表**（`Sun`/`Mon`…），避开 `strftime` 的
  locale 依赖，所以**不会**跟随系统语言变成中文，这是刻意选择。

## 相关文档

- OLED 驱动、显存模型与接口：[`../HARDWARE.md`](../HARDWARE.md) §2
- PCA9548A 与三块屏的通道分配：[`../HARDWARE.md`](../HARDWARE.md) §4
- 同款屏的非阻塞刷新范例：[`../uptime-oled/README.md`](../uptime-oled/README.md)
- 硬件诊断工具（屏完全无应答时用）：[`../uptime-oled/tools/i2c-scan`](../uptime-oled/tools/i2c-scan)
- 仓库总览：[`../README.md`](../README.md)
