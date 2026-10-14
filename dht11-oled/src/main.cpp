/**
 * dht11-oled —— DHT11 温湿度实时监测，SSD1306 128x64 I2C OLED 显示
 *
 * 接线（ESP32-S3-DevKitC-1 N16R8）：
 *   DHT11 DATA -> GPIO 7      ← 用户实际接线，就是"7 号位"
 *   DHT11 VCC  -> 3V3
 *   DHT11 GND  -> GND
 *
 *   OLED  SCL  -> GPIO 4
 *   OLED  SDA  -> GPIO 5
 *   OLED  VCC  -> 3V3
 *   OLED  GND  -> GND
 *
 * 行为：每 1 秒采样一次温湿度并整屏重画，屏幕上带刷新计数，
 *       数字在跳就是"1 秒一刷"的现场证据。
 *
 * ── 温湿度为什么要自己写时序，而不用 Adafruit 的 DHT 库 ──────────────
 * 本项目最初用的是 Adafruit `DHT sensor library`，实机上撞了两个坑：
 *
 *   1. **温湿度必须一次传输解出来。** DHT11 的 40 位是一次不可分割的
 *      传输，5 字节里 data[0..1]=湿度、data[2..3]=温度、data[4]=校验和。
 *      库把 `data[5]` 设成 private，外面取不到，想同时要两个值就只能
 *      调 `readHumidity()` + `readTemperature()` 两次 —— 而 DHT11 两次转换
 *      之间要求 ≥1s，背靠背连调两次，**第二次读到的是垃圾**。
 *      实机症状：湿度正常、温度永远显示 `--`。
 *   2. **超时是关中断自旋。** `expectPulse()` 在 InterruptLock 下
 *      `while (digitalRead())`，传感器不在线时自旋到看门狗整板重启。
 *
 * 所以现在用 `src/dht11.h` 里的自写裸驱 `dht11Read()`：
 * 一次传输解出全部 5 个字节，且每个等待循环都有 cycle 上限，
 * 不可能无限自旋。DHT11 的物理下限是 1Hz，所以采样节拍取 1000ms。
 *
 * 主循环为什么不用 delay(1000)：刷新判定是 now - lastTick >= 1000 的
 * 非阻塞写法，一次采样 + 一次 I2C 传输约 25~30ms，剩下的时间让出 CPU，
 * 将来加按键、网络、告警都不用重构主循环。
 */
#include <Arduino.h>

#include "dht11.h"
#include "font5x7.h"
#include "ssd1306_i2c.h"

// ------------------------------------------------------------------ 配置
// DHT11 数据脚 —— 用户实际接线：7 号位
static const uint8_t PIN_DHT_DATA = 7;

// I2C：SCL=4 号位、SDA=5 号位（与 uptime-oled 同一套接线）
static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

// 从机地址：模块背面丝印 0x78 是"含读写位的 8 位写法"，7 位地址是 0x3C。
// 少数模块出厂为 0x3D（丝印 0x7A），探测不到时会自动切换一次。
static const uint8_t OLED_ADDR_PRIMARY = 0x3C;
static const uint8_t OLED_ADDR_ALT = 0x3D;

static const uint32_t I2C_HZ = 400000;

// 屏幕重画节拍：用户要求的"刷新时间 1s"
static const uint32_t TICK_MS = 1000;

// 采样节拍。DHT11 数据手册规定两次测量间隔 ≥1s，这里取同一个 1s——
// 自写时序之后不再受 Adafruit 那个 2000ms 硬编码的束缚。
// 留 1000ms（而非刚好贴着 1s）是给传输本身留余量：一次读约 25ms。
static const uint32_t SAMPLE_MS = 1000;

// 采样失败后的重试节拍。失败通常是接线/干扰问题，
// 隔久一点再试，避免在故障时把串口刷屏。
static const uint32_t RETRY_MS = 2000;

static const uint32_t OLED_RETRY_MS = 5000;  // 屏幕掉线后的重试间隔

static const uint32_t DHT_WARMUP_MS = 1500;  // 上电先空转，等传感器供电稳定

Ssd1306I2c display(PIN_I2C_SCL, PIN_I2C_SDA, OLED_ADDR_PRIMARY, I2C_HZ);

static DhtReading reading;  // 最新一次采样结果（见 dht11.h）
static bool hasReading = false;

static uint32_t lastTick = 0;
static uint32_t lastSample = 0;
static uint32_t lastOledRetry = 0;
static uint32_t tickCount = 0;    // 自检：每刷一屏 +1
static uint32_t failCount = 0;    // 自检：采样失败累计次数
static uint32_t blankStreakS = 0; // 连续无有效读数累计秒数

// 数据脚的物理状态，由 sampleOnce() 每次采样时刷新。
// 用来区分"传感器没接"和"接了但读错了"——两者的排查方向完全不同。
static bool lineFloating = false;  // 外部悬空：传感器没接上

// ------------------------------------------------------------------ 小工具

/**
 * 把文本右对齐画在第 page 页（5x7 字库，字宽 6 像素含间距）。
 * 用 OLED_WIDTH 当右边界，所以右边永远贴齐屏幕。
 */
static void drawTextRight(uint8_t page, const char* s) {
  const size_t len = strlen(s);
  const uint16_t width = static_cast<uint16_t>(len) * 6;
  const uint8_t x = (width <= OLED_WIDTH)
                        ? static_cast<uint8_t>(OLED_WIDTH - width)
                        : 0;
  display.drawText5x7(x, page, s);
}

/**
 * 画一位小号数字。5x7 字模直接当"3x5 小字"用：只点亮列 0~2 的
 * 第 1~5 位（bit1..bit5），去掉左右各 1 列、上下各 1 行——
 * 正好每个字 3x5 像素，格点 4 像素，两个数字合计 7 像素。
 * 屏幕右下角那个"数据已存了多少秒"的小角标就是这么来的。
 */
static void drawTinyDigit(uint8_t x, uint8_t y, uint8_t digit) {
  static const char kDigits[] = "0123456789";
  const uint8_t* g = glyph5x7(kDigits[digit % 10]);
  for (uint8_t col = 1; col <= 3; ++col) {
    uint8_t bits = static_cast<uint8_t>((g[col] >> 1) & 0x1F);
    for (uint8_t r = 0; r < 5; ++r) {
      if (bits & (1u << r)) {
        display.setPixel(static_cast<uint8_t>(x + col - 1),
                         static_cast<uint8_t>(y + r));
      }
    }
  }
}

/** 画一个不足 10 的数（0..9 一位，10..99 两位），返回画完后的列。 */
static uint8_t drawTinyNumber(uint8_t x, uint8_t y, uint8_t value) {
  if (value >= 10) {
    drawTinyDigit(x, y, static_cast<uint8_t>(value / 10));
    x = static_cast<uint8_t>(x + 4);
  }
  drawTinyDigit(x, y, static_cast<uint8_t>(value % 10));
  return static_cast<uint8_t>(x + 3);
}

/**
 * 拼第 0 页的状态徽标（右边太长就顺次降级）：
 *   LIVE 4s   数据新鲜（<5s）且温度有效时计数
 *   ERR 3s    温度无效、但湿度还有值
 *   -- 4s     两个值都没了（传感器没应答）
 *   --        连一次有效读数都还没有
 */
static void formatBadge(char* out, size_t outLen, uint32_t ageS) {
  const char* tag = "LIVE";
  uint32_t n = ageS;

  if (!hasReading) {
    snprintf(out, outLen, "--");
    return;
  }
  if (!reading.tempValid && !reading.humValid) {
    tag = "--";
  } else if (!reading.tempValid) {
    tag = "ERR";
  }

  if (n > 99) {
    n = 99;
  }
  snprintf(out, outLen, "%s %lus", tag, static_cast<unsigned long>(n));
  if (strlen(out) > 11) {
    // "ERR 99s" 是 7 个字符，够用；真超长就退回不带秒的版本
    snprintf(out, outLen, "%s", tag);
  }
}

/** 渲染整屏。每 1 秒调用一次；数值来自最近一次采样。 */
static void render(uint32_t ageS) {
  display.clear();

  // 第 0 页：标题 + 右上角状态徽标
  display.drawText5x7(0, 0, "DHT11");
  char badge[16];
  formatBadge(badge, sizeof(badge), ageS);
  drawTextRight(0, badge);

  // 第 1 页：分隔线
  display.fillPageSpan(0, OLED_WIDTH - 1, 1, 0x18);

  // 第 2~3 页：湿度大号数值 + 右侧"数据年龄"小角标
  //            降级链：湿度不可用 -> 显示温度；两个都没有 -> "--"
  bool bigIsTemp = false;
  char bigValue[16] = "--";
  if (reading.humValid) {
    snprintf(bigValue, sizeof(bigValue), "%u%%", static_cast<unsigned>(reading.humidity));
  } else if (reading.tempValid) {
    snprintf(bigValue, sizeof(bigValue), "%ld", static_cast<long>(reading.temperature));
    bigIsTemp = true;
  }
  display.drawText5x7Scaled2x(0, 2, bigValue);
  // 角标贴屏幕最右下：最后两位画在 x=121，最多向左再占两位到 x=113
  if (hasReading && ageS < 100) {
    const uint8_t digits = (ageS >= 10) ? 2 : 1;
    drawTinyNumber(static_cast<uint8_t>(124 - 4 * digits),
                   static_cast<uint8_t>(11), static_cast<uint8_t>(ageS));
  }

  // 第 4 页：湿度单位（大号只放了数字，量纲单独一行写清楚）
  if (reading.humValid) {
    display.drawText5x7(0, 4, bigIsTemp ? "TEMP  C" : "HUMIDITY %");
  }

  // 第 5 页：另一个量的明细读数
  char line[32];
  if (reading.humValid && reading.tempValid) {
    snprintf(line, sizeof(line), "TEMP   %ld C", static_cast<long>(reading.temperature));
    display.drawText5x7(0, 5, line);
  } else if (!reading.humValid) {
    snprintf(line, sizeof(line), "HUM    %u %%", static_cast<unsigned>(reading.humidity));
    display.drawText5x7(0, 5, line);
  } else {
    display.drawText5x7(0, 5, "TEMP   -- C");
  }

  // 第 6 页：刷新计数（证明 1 秒节拍在跑）+ 右下角采样失败标记
  snprintf(line, sizeof(line), "#%lu", static_cast<unsigned long>(tickCount));
  display.drawText5x7(0, 6, line);
  if (failCount > 0) {
    drawTextRight(6, "ERR");
  }

  // 第 7 页：一行健康度 / 诊断（也用 ASCII：串口与字库都只认 ASCII）。
  // 顺序有讲究：先说"线是什么状态"（能直接指导动手去插哪根线），
  // 再说具体错在哪，最后才是正常态。
  char health[32];
  if (lineFloating) {
    snprintf(health, sizeof(health), "NO SENSOR - CHECK DATA WIRE");
  } else if (!hasReading) {
    snprintf(health, sizeof(health), "WAITING %lus",
             static_cast<unsigned long>(blankStreakS));
  } else if (reading.status != DHT_OK) {
    snprintf(health, sizeof(health), "%s", dhtStatusText(reading.status));
  } else {
    snprintf(health, sizeof(health), "OK  #%lu",
             static_cast<unsigned long>(tickCount));
  }
  display.drawText5x7(0, 7, health);

  display.flush();
}

/**
 * 采一次 DHT11，并记录失败次数与"数据脚到底是什么状态"。
 *
 * lineFloating = 摘掉内部上拉后读到 LOW，说明外部什么都没接（悬空）
 * 这个标志决定屏幕第 7 页写的是"没接传感器"还是具体错误码。
 */
static bool sampleOnce() {
  bool externallyPulled = false;
  const bool lineOk = dhtDataLineLooksAlive(PIN_DHT_DATA, &externallyPulled);
  // 摘掉内部上拉后仍读到 HIGH，说明外部上拉在线（模块真的接上了）。
  lineFloating = !lineOk;

  if (!lineOk) {
    // 线明显不对时不读传感器：省下 20ms 的起始拉低，也避免把
    // "线的问题"误报成"器件不应答"。
    reading.status = DHT_ERR_NO_RESPONSE;
    reading.tempValid = false;
    reading.humValid = false;
    ++failCount;
    return false;
  }

  const uint8_t st = dht11Read(PIN_DHT_DATA, reading);
  if (st != DHT_OK) {
    ++failCount;
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);  // 等 USB CDC 枚举完，否则前几行日志会丢

  Serial.println();
  Serial.println("=== dht11-oled ===");
  Serial.printf("DHT11: DATA=GPIO%u (self-written timing)  sample every %lums\n",
                PIN_DHT_DATA, static_cast<unsigned long>(SAMPLE_MS));
  Serial.printf("OLED : SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL,
                PIN_I2C_SDA, static_cast<unsigned long>(I2C_HZ));
  Serial.printf("Screen refreshes every %lums.\n",
                static_cast<unsigned long>(TICK_MS));

  // 自写时序没有 begin()：引脚在 dht11Read() 里按需切换方向。
  // 这里只把引脚设成带内部上拉的输入，作为空闲态。
  pinMode(PIN_DHT_DATA, INPUT_PULLUP);
  Serial.println("DHT11 pin armed. Warming up...");

  if (!display.begin()) {
    // 串口日志一律用 ASCII：中文经 UTF-8 输出到部分串口工具会变乱码，
    // 而这条信息恰恰是故障时唯一能看到的线索。
    Serial.printf("[ERR] no OLED ACK at 0x%02X.\n", OLED_ADDR_PRIMARY);
    Serial.println("Check, in this order:");
    Serial.println("  1) OLED SCL -> GPIO4 and SDA -> GPIO5 (swapped wires = no ACK)");
    Serial.println("  2) OLED VCC -> 3V3, GND -> GND; on-board power LED lit?");
    Serial.println("  3) bare panel needs 4.7k pull-ups to 3V3 (modules have them)");
    Serial.println("  4) address: some modules are 0x3D, not 0x3C");
    Serial.println("Run tools/i2c-scan to see whether ANY device answers.");
    Serial.println("DHT11 sampling keeps running; per-second output continues below.");
  } else {
    Serial.println("OLED init OK.");
  }

  // 开机先画一帧，避免上电后空屏等到第 1 秒
  render(0);

  const uint32_t now = millis();
  lastTick = now;
  lastOledRetry = now;
  lastSample = now - SAMPLE_MS;  // 让第一次采样在预热结束后立刻发生
}
void loop() {
  const uint32_t now = millis();

  // 屏幕不在线时，每 5 秒重试一次初始化。好处：接线没插好的情况下，
  // 插好就能自己恢复，不必重新上电。
  if (!display.ready() && now - lastOledRetry >= OLED_RETRY_MS) {
    lastOledRetry = now;
    Serial.println("[retry] probing OLED again...");
    if (display.begin()) {
      Serial.println("[retry] OLED came online.");
      lastTick = now;  // 重新对齐刷新节拍
    }
  }

  // ---- 采样：每 SAMPLE_MS 一次（失败则隔 RETRY_MS 再试）----
  // 上电后先空转 DHT_WARMUP_MS，等传感器供电稳下来再问第一次。
  const uint32_t sampleDue =
      hasReading ? SAMPLE_MS : ((now < DHT_WARMUP_MS) ? UINT32_MAX : RETRY_MS);
  if (now - lastSample >= sampleDue) {
    lastSample = now;
    const bool ok = sampleOnce();
    if (ok) {
      hasReading = true;
    }
    // 日志搞成一行固定字段，方便直接丢进表格或脚本里做趋势
    char csv[96];
    formatDhtCsv(now / 1000UL, reading, failCount, csv, sizeof(csv));
    Serial.println(csv);

    // 数据脚明显不对时额外打一段 ASCII 诊断。只在状态变化时打，
    // 免得每秒刷屏把有用的日志冲掉。
    if (!ok) {
      static int lastDiag = -2;
      const int diag = static_cast<int>(reading.status);
      if (diag != lastDiag) {
        lastDiag = diag;
        if (lineFloating) {
          Serial.printf("[ERR] No sensor on GPIO%u (line is floating).\n",
                        PIN_DHT_DATA);
          Serial.println("      Nothing external is pulling the data line up.");
          Serial.println("      Check: DATA -> GPIO7, VCC -> 3V3, GND -> GND");
        } else {
          Serial.printf("[ERR] DHT11 read failed: %s\n",
                        dhtStatusText(reading.status));
          if (reading.status == DHT_ERR_NO_RESPONSE) {
            Serial.println("      Sensor did not answer the start signal.");
            Serial.println("      Check: DATA -> GPIO7, VCC -> 3V3, GND -> GND");
          } else if (reading.status == DHT_ERR_CHECKSUM) {
            Serial.println("      Bits arrived but the checksum failed.");
            Serial.println("      Usually a long/marginal wire or a noisy supply.");
          } else if (reading.status == DHT_ERR_TIMEOUT) {
            Serial.println("      Read stalled mid-transfer - wire or contact issue.");
          } else if (reading.status == DHT_ERR_RANGE) {
            Serial.println("      Checksum OK but values are outside DHT11 range.");
          }
        }
        Serial.println("      Run tools/dht-probe to read the pin state directly.");
      }
    }
  }

  // ---- 刷新：每 TICK_MS 重画一屏（非阻塞，累加而非赋值避免长期漂移）----
  if (now - lastTick < TICK_MS) {
    return;
  }
  lastTick += TICK_MS;
  ++tickCount;

  // 屏幕右下角的"数据年龄"以"最近一次成功采样"为零点
  const uint32_t ageS =
      hasReading ? ((now - reading.atMs) / 1000UL) : 0;
  if (!hasReading || (!reading.tempValid && !reading.humValid)) {
    ++blankStreakS;
  } else {
    blankStreakS = 0;
  }

  render(ageS);

  Serial.printf("[tick %lu] %s  age=%lus  fails=%lu\n",
                static_cast<unsigned long>(tickCount),
                hasReading ? "data" : "no-data",
                static_cast<unsigned long>(ageS),
                static_cast<unsigned long>(failCount));
}
