/**
 * lcd1602-uptime —— LCD1602 (I2C/PCF8574) 显示通电时间
 *
 * 接线（ESP32-S3-DevKitC-1 N16R8）：
 *   LCD SCL -> GPIO 4
 *   LCD SDA -> GPIO 5
 *   LCD VCC -> 5V   <-- 1602 是 5V 器件；**SDA/SCL 必须经电平转换**
 *   LCD GND -> GND
 *
 * 行为：上电后每 1 秒刷新一次，显示自本次上电起的累计时长。
 *       第 1 行：标题 + 刷新计数（计数在跳 = 刷新真的在跑）
 *       第 2 行：HH:MM:SS 绝对读数，满 24 小时进位成天
 *
 * 计时为什么用 millis()：
 *   - 需求精度是"1 秒级"，millis() 是自上电起的单调计数（48 位累加），
 *     语义上正好等于"通电时间"，不需要额外维护起点；
 *   - 刷新判定用"上次时间戳 + 1000ms"比较，非阻塞，没有 delay(1000)；
 *   - 时间戳用 `last += 1000` 而不是 `= now`，避免某次刷新被 I2C 拖长
 *     后误差长期累积（与 uptime-oled 同一套写法）。
 *   注意：ESP32 进深度睡眠时 millis() 会停走；本示例不涉及深睡。
 */
#include <Arduino.h>

#include "lcd1602_i2c.h"

// ------------------------------------------------------------------ 配置
// I2C 引脚：沿用同仓库 uptime-oled 的 GPIO4/5（两脚都是 S3 的普通 IO，
// 无 strapping、非 SPI0/1，可安全使用）。
static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

// PCF8574 背包地址。0x27 是市售蓝色模块最常见的出厂值；
// 少数模块（PCF8574A 芯片）是 0x3F。下面两个都放进来，探测不到就换。
static const uint8_t LCD_ADDR_PRIMARY = 0x27;
static const uint8_t LCD_ADDR_ALT = 0x3F;

// PCF8574 标称最高 100kHz。降到 100k 换容错：5V 供电 + 电平转换模块
// 会引入额外的上升沿延迟，慢一点更不容易出错。
static const uint32_t I2C_HZ = 100000;

static const uint32_t REFRESH_MS = 1000;    // 刷新周期 1s
static const uint32_t RETRY_MS = 5000;      // 屏幕掉线后的重试间隔

Lcd1602I2c lcd(PIN_I2C_SCL, PIN_I2C_SDA, LCD_ADDR_PRIMARY, I2C_HZ);

static uint32_t lastRefresh = 0;
static uint32_t refreshCount = 0;
static uint32_t lastRetry = 0;

/**
 * 把总秒数格式化成 1602 上最合适的两行读数。
 *
 * 第一行主数值（紧凑，给"当前跑了多久"）：
 *   0..3599   -> "MM:SS"
 *   满 24 小时 -> "DDdHH:MM"（天不顶掉，能一直往上走）
 *
 * 第二行明细（HH:MM:SS，带秒的绝对读数，永不进位）：
 *   天数 > 0 时前面加 "Nd " 前缀，最长 "365d 23:59:59" = 14 字符，放得下 16 列。
 */
static void formatUptime(uint32_t totalSec, char* out, size_t outLen) {
  const uint32_t days = totalSec / 86400UL;
  const uint32_t rem = totalSec % 86400UL;
  const uint32_t hours = rem / 3600UL;
  const uint32_t minutes = (rem % 3600UL) / 60UL;
  const uint32_t seconds = rem % 60UL;

  if (days > 0) {
    snprintf(out, outLen, "%lud%02lu:%02lu", static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  } else {
    snprintf(out, outLen, "%02lu:%02lu", static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  }
}

/** 渲染整屏。每 1 秒调用一次。 */
static void render(uint32_t totalSec) {
  // 第 1 行：标题 + 刷新计数
  // "#123" 就是"每秒确实在刷"的现场证据——数字在跳说明刷新周期在跑，
  // 而不是屏幕冻在某一帧（LCD 断电后还会保留内容，静止画面不可信）。
  char line0[LCD_COLS + 1];
  snprintf(line0, sizeof(line0), "UPTIME #%lu",
           static_cast<unsigned long>(refreshCount));
  lcd.printLine(0, line0);

  // 第 2 行：HH:MM:SS 绝对读数
  char line1[LCD_COLS + 1];
  const uint32_t d = totalSec / 86400UL;
  const uint32_t h = (totalSec % 86400UL) / 3600UL;
  const uint32_t m = (totalSec % 3600UL) / 60UL;
  const uint32_t s = totalSec % 60UL;
  if (d > 0) {
    snprintf(line1, sizeof(line1), "%lud %02lu:%02lu:%02lu",
             static_cast<unsigned long>(d), static_cast<unsigned long>(h),
             static_cast<unsigned long>(m), static_cast<unsigned long>(s));
  } else {
    snprintf(line1, sizeof(line1), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(h), static_cast<unsigned long>(m),
             static_cast<unsigned long>(s));
  }
  lcd.printLine(1, line1);
}

/** 初始化失败时的 ASCII 排查清单（中文经部分串口工具会乱码）。 */
static void printTroubleshooting() {
  Serial.println("Check, in this order:");
  Serial.println("  1) SCL -> GPIO4, SDA -> GPIO5 (swapped = no ACK)");
  Serial.println("  2) backpack VCC/GND connected; backlight lit?");
  Serial.println("  3) 5V build: SDA/SCL MUST go through a level shifter.");
  Serial.println("     PCF8574 pull-ups sit at backpack VCC; 5V on S3 IO");
  Serial.println("     exceeds abs-max VDD+0.3V and damages the pin.");
  Serial.println("  4) address: 0x27 most common, 0x3F on PCF8574A modules");
  Serial.println("Run tools/i2c-scan to see whether ANY device answers.");
}

void setup() {
  Serial.begin(115200);
  delay(200);  // 等 USB CDC 枚举完，否则前几行日志会丢

  Serial.println();
  Serial.println("=== lcd1602-uptime ===");
  Serial.printf("I2C: SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL,
                PIN_I2C_SDA, static_cast<unsigned long>(I2C_HZ));
  Serial.println("Target: PCF8574 backpack @ 0x27 (LCD1602, 16x2)");

  if (!lcd.begin()) {
    Serial.printf("[ERR] no ACK at 0x%02X. Trying 0x%02X...\n",
                  LCD_ADDR_PRIMARY, LCD_ADDR_ALT);
    if (lcd.beginAt(LCD_ADDR_ALT)) {
      Serial.printf("LCD init OK at 0x%02X (fallback).\n", LCD_ADDR_ALT);
    } else {
      Serial.printf("[ERR] no ACK at 0x%02X either.\n", LCD_ADDR_ALT);
      printTroubleshooting();
      Serial.println("Uptime logic keeps running; per-second output below.");
    }
  } else {
    Serial.printf("LCD init OK at 0x%02X, refreshing every 1s.\n",
                  LCD_ADDR_PRIMARY);
  }

  // 开机先画一帧，避免上电后空白等到第 1 秒
  render(0);
  lastRefresh = millis();
}

void loop() {
  const uint32_t now = millis();

  // 屏幕不在线时每 5 秒重试：线没插好时插好即可自恢复，不必重新上电。
  if (!lcd.ready() && now - lastRetry >= RETRY_MS) {
    lastRetry = now;
    Serial.println("[retry] probing LCD again...");
    if (lcd.begin()) {
      Serial.println("[retry] LCD came online.");
      lastRefresh = millis();  // 重新对齐刷新节拍
    }
  }

  if (now - lastRefresh < REFRESH_MS) {
    return;  // 非阻塞：到点才干活，其余时间让出 CPU
  }
  lastRefresh += REFRESH_MS;  // 累加而非赋值，避免单次超时造成长期漂移
  ++refreshCount;

  const uint32_t totalSec = now / 1000UL;
  render(totalSec);

  char line[16];
  formatUptime(totalSec, line, sizeof(line));
  Serial.printf("[%lu] uptime=%s  refreshes=%lu\n",
                static_cast<unsigned long>(totalSec), line,
                static_cast<unsigned long>(refreshCount));
}
