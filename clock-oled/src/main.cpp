/**
 * clock-oled —— SSD1306 128x64 I2C OLED 时间与日期显示
 *
 * 接线（ESP32-S3-DevKitC-1 N16R8）：
 *   OLED SCL  -> GPIO 4
 *   OLED SDA  -> GPIO 5
 *   OLED VCC  -> 3V3
 *   OLED GND  -> GND
 *
 * 行为：
 *   1. 上电连 WiFi，用 NTP 把系统时钟校准到真实时间；
 *   2. 校准后每 1 秒刷新屏幕，大号显示 HH:MM，下面显示秒 / 日期 / 星期；
 *   3. 之后每 CATCHUP_MS 秒重新对一次时，抵消 ESP32 内部 RC 振荡器的走时误差。
 *
 * 时间从哪来（关键设计点）：
 *   ESP32 **没有带电池的 RTC 芯片**，断电即失时间。所以：
 *     - 真值来源只有 NTP，必须联网；
 *     - 拿到时间后写入系统时钟（settimeofday），此后 RTC 自己走字；
 *     - 断网也能继续显示，但会按 RC 振荡器误差漂（实测 S3 常见 ±10ppm 级，
 *       即每天约 ±1 秒；无外部晶振的板子可能到 ±100ppm，即每天约 ±9 秒）。
 *       所以联网恢复后立刻重新对时，纠正累计漂移。
 *
 * 为什么用 settimeofday 而不是手搓一套计时：
 *   ESP-IDF 自带系统时钟 + configTzTime() 应用 POSIX 时区，
 *   localtime_r() 直接把 UTC 换算成本地日期时间（含星期），
 *   闰年、月末、时区规则全都不用自己算——这是最容易写错的地方。
 *
 * 刷新为什么不用 delay(1000)：
 *   用"上次刷新的时间戳 + 1000ms"比较，非阻塞；将来要加按键、
 *   蜂鸣器、传感器，主循环不用重构。与 I2C 传输时间天然解耦。
 */
#include <Arduino.h>
#include <string.h>
#include <time.h>

#include <WiFi.h>

#include "secrets.h"
#include "ssd1306_i2c.h"

// ------------------------------------------------------------------ 配置
// 引脚按用户实际接线：I2C 时钟线(SCL) 接 4 号位，数据线(SDA) 接 5 号位
static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

// 从机地址：模块背面丝印 0x78 是"含读写位的 8 位写法"，7 位地址是 0x3C。
// 少数模块出厂为 0x3D（丝印 0x7A），探测不到时会自动切换一次。
static const uint8_t OLED_ADDR_PRIMARY = 0x3C;
static const uint8_t OLED_ADDR_ALT = 0x3D;

static const uint32_t I2C_HZ = 400000;

// 刷新周期：1 秒。秒位必须每秒动一次，这是校时的直观证据。
static const uint32_t REFRESH_MS = 1000;

// 屏幕掉线后的重试间隔
static const uint32_t OLED_RETRY_MS = 5000;

// 重新对时的间隔：1 小时。每次都是一次 DNS + UDP 往返，开销可忽略，
// 但足以把 RC 振荡器的漂移压到"看不见"的量级。
static const uint32_t CATCHUP_MS = 3600000UL;

// NTP 首次同步的等待上限。超过就先进主循环显示"未对时"，
// 不要卡在 setup() 里黑屏——用户看不见任何反馈会以为烧录失败。
static const uint32_t NTP_WAIT_MS = 20000;

Ssd1306I2c display(PIN_I2C_SCL, PIN_I2C_SDA, OLED_ADDR_PRIMARY, I2C_HZ);

static uint32_t lastRefresh = 0;
static uint32_t refreshCount = 0;
static uint32_t lastOledRetry = 0;
static uint32_t lastCatchup = 0;

// 系统时钟是否已经校准过（对过时至少一次）。
// 未校准时 RTC 从 1970-01-01 起走，屏幕上必须明确区分，否则用户会
// 以为"显示的是 1970 年"是程序 bug。
static bool timeSynced = false;

// 上次 NTP 同步距今的秒数，用于屏幕右下角显示"数据新鲜度"。
static uint32_t lastSyncAgeS = 0;

// ------------------------------------------------------------------ 版面行位
// 每个文本块顶边所在的像素行（0 = 屏幕最上）。集中放这里，调整版面时
// 只改这几个数，不用在读渲染代码时到处找魔数。
//
// 三行内容（时间 14 行、日期 14 行、星期 16 行，共 44 行）在 64 行里
// 尽量拉开：上下边距和两块间距都取 5 行，视觉上三行均匀分布、星期贴近底部。
//   时间  5..18   日期 24..37   星期 43..58
// 信号数字单独贴在最底行 57..63，靠右对齐（x=110..127），
// 与居中的"星期三"（x=40..87）列范围不重叠，所以两者虽然行范围有交叠，
// 视觉上并不冲突——这也是它没跟着三行一起均分的原因。
static const uint8_t kRowTimeY = 5;   // 时间  5..18
static const uint8_t kRowDateY = 24;  // 日期 24..37
static const uint8_t kRowWeekY = 43;  // 星期 43..58
static const uint8_t kRowRssiY = 57;  // 信号 57..63（贴到屏幕最底行）

// ------------------------------------------------------------------ 工具
/**
 * 把 WiFi 状态变成一句人话，打印到串口 / 显示在屏幕上。
 * 串口日志一律 ASCII：中文经 UTF-8 输出到部分串口工具会变乱码，
 * 而这些恰是故障时唯一能看到的线索。
 */
static const char* wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_CONNECT_FAILED:  return "bad password";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected";
    case WL_CONNECTED:       return "connected";
    default:                 return "unknown";
  }
}

/** 不带阻塞地把系统时钟对齐到 NTP。返回是否成功。 */
static bool syncTime() {
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  // configTzTime 起了一个后台 SNTP 任务，用轮询等它写进系统时钟。
  // 为什么不用 getLocalTime(&t, timeout) 一把梭：那个调用内部会阻塞，
  // 期间屏幕完全不动；这里改成短轮询，每 200ms 让出一帧的时间。
  const uint32_t deadline = millis() + NTP_WAIT_MS;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    time_t now = time(nullptr);
    // 2020-01-01 之后的时间才算"对上了"。
    // 未校准时 time() 返回的是 1970 年起的秒数（一个很小的值）。
    if (now > 1577836800) {
      timeSynced = true;
      lastSyncAgeS = 0;
      return true;
    }
    delay(200);
  }
  return false;
}

/** 连接 WiFi（带超时）。返回是否连上。 */
static bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 关省电：省电模式会给 NTP 往返加几百毫秒抖动
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      Serial.printf("WiFi: timeout (%s)\n",
                    wifiStatusText(WiFi.status()));
      return false;
    }
    delay(250);
  }

  Serial.printf("WiFi: connected, IP=%s  RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

/** 把年份夹到 1000..9999，给 snprintf 的 %04d 一个确定的输出上界。 */
static int clampYear(int y) {
  if (y < 1000) return 1000;
  if (y > 9999) return 9999;
  return y;
}

/**
 * 屏幕顶部状态行用的小标记。
 * 'W' = WiFi 在线，'w' = 无 WiFi；后面跟时间数据是否新鲜。
 * 这两个字符加起来就能一眼判读："显示的时间还可不可信"。
 */
// 中文星期表。索引就是 tm_wday（0 = 周日）。
// 只用到"一二三四五六日"，全部在 font_cn.h 的字表里。
static const char* kWeekdayCn(int wday) {
  static const char* kTable[7] = {"日", "一", "二", "三", "四", "五", "六"};
  return (wday >= 0 && wday < 7) ? kTable[wday] : "?";
}

/** 渲染整屏。每 1 秒调用一次。 */
static void render() {
  display.clear();

  // ---- 版式分工 ----
  // 屏幕 64 行。绘制函数支持从任意像素行起画（drawText*Y），所以行距可以
  // 随意安排，不必凑成 8 的倍数——早先用页号作起点，结果每行底部都空出
  // 8 像素、屏幕中段整页空白，看着像没用满。
  //
  // 当前排布（各块高 14~16 像素，块间留 3~4 像素，底部贴到第 63 行）：
  //   时间 y=1..14    日期 y=18..31    星期 y=35..50    信号 y=57..63
  // 调整任何一块的 y，注意别让相邻两块重叠（间隔至少 1 像素）。

  if (!timeSynced) {
    // 还没对上时间：不显示 1970 年那种假读数，直接给排查清单。
    // 用 ASCII 而不是中文，是因为此刻最需要的是"能看懂怎么修"。
    display.drawText5x7Y(0, 8, "--:--");
    display.drawText5x7Y(0, 24, "waiting for NTP");
    display.drawText5x7Y(0, 32, "check WiFi in");
    display.drawText5x7Y(0, 48, "src/secrets.h");
    display.flush();
    return;
  }

  // ---- 取本地时间 ----
  // localtime_r 是线程安全版本，且会应用 configTzTime 设好的时区规则，
  // 闰年 / 月末 / 夏令时都不用自己算。
  const time_t nowEpoch = time(nullptr);
  struct tm t{};
  localtime_r(&nowEpoch, &t);

  const char* wdCn = kWeekdayCn(t.tm_wday);

  // ---- 时间：y=1..14 ----
  // 这是整个屏幕的视觉主体。8 个字符 × 12 列 = 96 列，128 列宽度里
  // 居中后左右各留 16 列，是最大且仍能居中的字号。字形高 14 像素。
  char hhmmss[12];
  snprintf(hhmmss, sizeof(hhmmss), "%02d:%02d:%02d", t.tm_hour, t.tm_min,
           t.tm_sec);
  const uint8_t clockW = static_cast<uint8_t>(strlen(hhmmss) * 12);
  display.drawText5x7Scaled2xY(static_cast<uint8_t>((OLED_WIDTH - clockW) / 2),
                               kRowTimeY, hhmmss);

  // ---- 日期：y=18..31 ----
  // 纯数字 "2026-10-14"，与时间同一字号（2 倍放大），视觉成一体。
  // 10 个字符 × 12 列 = 120 列，128 列里居中后左右各留 4 列，刚好放得下。
  //
  // 为什么不用 "2026年10月14日"：汉字每个 16 列，和放大后的数字混排是
  // 8×12 + 3×16 = 144 列，比屏幕宽 16 列，放不下。纯数字才容得下年份。
  const int year = clampYear(t.tm_year + 1900);
  // 缓冲给足：年份理论上是 4 位，但编译器按 int 最大宽度算，
  // 只给 16 会报 -Wformat-truncation。给 32 换编译零警告。
  char dateBuf[32];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", year, t.tm_mon + 1,
           t.tm_mday);
  const uint8_t dateW = static_cast<uint8_t>(strlen(dateBuf) * 12);
  display.drawText5x7Scaled2xY(static_cast<uint8_t>((OLED_WIDTH - dateW) / 2),
                               kRowDateY, dateBuf);

  // ---- 星期：y=35..50 ----
  // "星期三" = 3 个汉字 = 48 列，居中。汉字高 16 像素，跨第 4、5 两页。
  char weekBuf[16];
  snprintf(weekBuf, sizeof(weekBuf), "星期%s", wdCn);
  const uint8_t weekW = Ssd1306I2c::cnTextWidth(weekBuf);
  display.drawTextCnY(static_cast<uint8_t>((OLED_WIDTH - weekW) / 2), kRowWeekY,
                      weekBuf);

  // ---- 信号：y=57..63 ----
  // 只显示信号数字（RSSI），不带文字标签——数字本身就说明问题。
  // 右对齐起点要按实际字符数算：RSSI 可能是 -45 也可能是 -100。
  char rssi[16];
  snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI());
  const uint8_t rssiW = static_cast<uint8_t>(strlen(rssi) * 6);
  display.drawText5x7Y(static_cast<uint8_t>(OLED_WIDTH - rssiW), kRowRssiY,
                       rssi);

  display.flush();
}

void setup() {
  Serial.begin(115200);
  delay(200);  // 等 USB CDC 枚举完，否则前几行日志会丢

  Serial.println();
  Serial.println("=== clock-oled ===");
  Serial.printf("I2C: SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL,
                PIN_I2C_SDA, static_cast<unsigned long>(I2C_HZ));
  Serial.printf("TZ: %s  NTP: %s / %s / %s\n", TZ_INFO, NTP_SERVER_1,
                NTP_SERVER_2, NTP_SERVER_3);

  if (!display.begin()) {
    // 主地址没应答。日志走 ASCII，理由同 uptime-oled：这些是故障时
    // 唯一能看到的线索，乱码就白写了。
    Serial.printf("[ERR] no ACK at 0x%02X.\n", OLED_ADDR_PRIMARY);
    Serial.println("Check, in this order:");
    Serial.println("  1) SCL -> GPIO4 and SDA -> GPIO5 (swapped wires = no ACK)");
    Serial.println("  2) VCC -> 3V3, GND -> GND; on-board power LED lit?");
    Serial.println("  3) bare panel needs 4.7k pull-ups to 3V3 (modules have them)");
    Serial.println("  4) address: some modules are 0x3D, not 0x3C");
    Serial.println("Run uptime-oled/tools/i2c-scan to see whether ANY device answers.");
    Serial.println("Clock logic keeps running; per-second output continues below.");
  } else {
    Serial.println("OLED init OK, refreshing every 1s.");
  }

  // 先画一帧"未对时"，保证上电就有反馈，不黑屏。
  render();
  lastRefresh = millis();

  // 再联网对时。拔掉网线也不会卡死：连接和 NTP 都有超时。
  if (connectWiFi()) {
    if (syncTime()) {
      struct tm t{};
      const time_t e = time(nullptr);
      localtime_r(&e, &t);
      Serial.printf("NTP: synced -> %04d-%02d-%02d %02d:%02d:%02d\n",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                    t.tm_min, t.tm_sec);
    } else {
      Serial.println("NTP: no reply; showing NO-TIME screen, will retry.");
    }
  } else {
    Serial.println("WiFi: unavailable; showing NO-TIME screen, will retry.");
  }

  lastCatchup = millis();
  render();
}

void loop() {
  const uint32_t now = millis();

  // ---- 屏幕不在线时，每 5 秒重试一次初始化 ----
  // 接线没插好的情况下，插好就能自己恢复，不必重新上电。
  if (!display.ready() && now - lastOledRetry >= OLED_RETRY_MS) {
    lastOledRetry = now;
    Serial.println("[retry] probing OLED again...");
    if (display.begin()) {
      Serial.println("[retry] OLED came online.");
      lastRefresh = millis();  // 重新对齐刷新节拍
    }
  }

  // ---- 每 5 秒检查一次网络；断了就重连，连上就补对时 ----
  // 这个检查比刷新周期长，因为重连本身有耗时，不需要每秒试探。
  static uint32_t lastNetCheck = 0;
  if (now - lastNetCheck >= 5000) {
    lastNetCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      if (connectWiFi() && !timeSynced) {
        syncTime();
      }
    } else if (!timeSynced || now - lastCatchup >= CATCHUP_MS) {
      // 两种情况都重新对时：
      //   1) 还没对上过（首次失败后的重试）；
      //   2) 到了周期（抵消 RC 振荡器漂移）。
      if (syncTime()) {
        lastCatchup = millis();
        Serial.println("NTP: re-synced.");
      }
    }
  }

  // ---- 每秒刷新屏幕 ----
  if (now - lastRefresh < REFRESH_MS) {
    return;  // 非阻塞：到点才干活，其余时间让出 CPU
  }
  lastRefresh += REFRESH_MS;  // 累加而非赋值，避免单次超时造成长期漂移
  ++refreshCount;

  // 数据新鲜度：距上次成功对时过了多久。断网时这个数会一直涨，
  // 涨到几分钟以上就该知道屏幕上的秒可能不准了。
  if (timeSynced) {
    ++lastSyncAgeS;
  }

  render();

  // ---- 串口日志：每秒一行，纯 ASCII ----
  if (timeSynced) {
    struct tm t{};
    const time_t e = time(nullptr);
    localtime_r(&e, &t);
    Serial.printf("[%lu] %04d-%02d-%02d %02d:%02d:%02d  wifi=%s  age=%lus\n",
                  static_cast<unsigned long>(now / 1000UL), t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  static_cast<unsigned long>(lastSyncAgeS));
  } else {
    Serial.printf("[%lu] NO-TIME  wifi=%s  refreshes=%lu\n",
                  static_cast<unsigned long>(now / 1000UL),
                  wifiStatusText(WiFi.status()),
                  static_cast<unsigned long>(refreshCount));
  }
}
