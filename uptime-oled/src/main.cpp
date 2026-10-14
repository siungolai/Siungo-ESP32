/**
 * uptime-oled —— SSD1306 128x64 I2C OLED 显示通电时间
 *
 * 接线（ESP32-S3-DevKitC-1 N16R8）：
 *   OLED SCL  -> GPIO 4
 *   OLED SDA  -> GPIO 5
 *   OLED VCC  -> 3V3
 *   OLED GND  -> GND
 *
 * 行为：上电后每 1 秒刷新一次屏幕，显示自本次上电起的累计时长。
 *       满 24 小时自动进位成天，大号数值始终显示"秒"这一档，
 *       下一行显示完整分解，再下面是进度条。
 *
 * 计时为什么用 millis() 而不是 RTC/硬件定时器：
 *   - 需求精度是"1 秒级"，millis() 是 ESP32-Arduino 里单调递增的微秒级
 *     计数器换算而来，48 位累加，跑几年都不会溢出，精度绰绰有余；
 *   - 刷新用"上一次刷新的时间戳 + 1000ms"比较，是非阻塞写法，
 *     不用 delay(1000)，将来要加按键、传感器不受影响；
 *   - 与 SSD1306 的 I2C 传输（约 25ms）之间天然解耦。
 *   注意：millis() 计的是"上电至今"，深睡会停走；本示例不涉及低功耗深睡。
 */
#include <Arduino.h>

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
static const uint32_t REFRESH_MS = 1000;  // 更新周期 1s

// 进度条：满一圈代表 1 分钟（60 秒），直观看到"秒在走"
static const uint32_t BAR_PERIOD_S = 60;
static const uint8_t BAR_X0 = 0;
static const uint8_t BAR_X1 = 127;
static const uint8_t BAR_PAGE = 7;
static const uint8_t BAR_FILLED = 0x7E;  // 实心行：中间 6 行点亮，上下各留 1 行留白
static const uint8_t BAR_EMPTY = 0x00;

Ssd1306I2c display(PIN_I2C_SCL, PIN_I2C_SDA, OLED_ADDR_PRIMARY, I2C_HZ);

static uint32_t lastRefresh = 0;
static uint32_t refreshCount = 0;
static uint32_t lastRetry = 0;
static const uint32_t RETRY_MS = 5000;  // 屏幕掉线后的重试间隔

/**
 * 把总秒数格式化成"最大单位 + 下一级单位"的紧凑写法。
 *   0..3599    -> "MM:SS"        （59:59 以内）
 *   1..99 天   -> "DDdHH:MM"     （满 24 小时进位成天）
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
  display.clear();

  // 第 0 页：标题
  display.drawText5x7(0, 0, "UPTIME");
  // 第 0 页右侧：刷新计数，验证"确实每秒在动"
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "#%lu", static_cast<unsigned long>(refreshCount));
  const size_t cntLen = strlen(cnt);
  const uint8_t cntX =
      (cntLen * 6 <= OLED_WIDTH) ? static_cast<uint8_t>(OLED_WIDTH - cntLen * 6)
                                 : 0;
  display.drawText5x7(cntX, 0, cnt);

  // 第 1 页：分隔线
  display.fillPageSpan(0, OLED_WIDTH - 1, 1, 0x18);

  // 第 2~3 页：大号主数值（2 倍放大），显示 MM:SS 或 DDdHH:MM 的后半段
  char mainText[16];
  formatUptime(totalSec, mainText, sizeof(mainText));
  display.drawText5x7Scaled2x(0, 2, mainText);

  // 第 4 页：留白（把主数值和明细区分开）
  // 第 5 页：完整分解，永不进位的绝对读数
  char detail[32];
  const uint32_t d = totalSec / 86400UL;
  const uint32_t h = (totalSec % 86400UL) / 3600UL;
  const uint32_t m = (totalSec % 3600UL) / 60UL;
  const uint32_t s = totalSec % 60UL;
  if (d > 0) {
    snprintf(detail, sizeof(detail), "%lud %02lu:%02lu:%02lu",
             static_cast<unsigned long>(d), static_cast<unsigned long>(h),
             static_cast<unsigned long>(m), static_cast<unsigned long>(s));
  } else {
    snprintf(detail, sizeof(detail), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(h), static_cast<unsigned long>(m),
             static_cast<unsigned long>(s));
  }
  display.drawText5x7(0, 5, detail);

  // 第 6 页：总秒数（无人机/设备计时常用读数）
  char secs[32];
  snprintf(secs, sizeof(secs), "TOTAL %lu s",
           static_cast<unsigned long>(totalSec));
  display.drawText5x7(0, 6, secs);

  // 第 7 页：进度条，60 秒跑满一圈
  const uint32_t phase = totalSec % BAR_PERIOD_S;
  display.fillPageSpan(BAR_X0, BAR_X1, BAR_PAGE, BAR_EMPTY);
  const uint32_t span = BAR_X1 - BAR_X0 + 1;
  uint32_t filled = (phase * span) / BAR_PERIOD_S;
  if (filled > 0) {
    display.fillPageSpan(BAR_X0, static_cast<uint8_t>(BAR_X0 + filled - 1),
                         BAR_PAGE, BAR_FILLED);
  }

  display.flush();
}

void setup() {
  Serial.begin(115200);
  delay(200);  // 等 USB CDC 枚举完，否则前几行日志会丢

  Serial.println();
  Serial.println("=== uptime-oled ===");
  Serial.printf("I2C: SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL,
                PIN_I2C_SDA, static_cast<unsigned long>(I2C_HZ));

  if (!display.begin()) {
    // 主地址没应答。串口日志一律用 ASCII：中文经 UTF-8 输出到部分
    // 串口工具会变乱码，而这条信息恰恰是故障时唯一能看到的线索。
    Serial.printf("[ERR] no ACK at 0x%02X.\n", OLED_ADDR_PRIMARY);
    Serial.println("Check, in this order:");
    Serial.println("  1) SCL -> GPIO4 and SDA -> GPIO5 (swapped wires = no ACK)");
    Serial.println("  2) VCC -> 3V3, GND -> GND; on-board power LED lit?");
    Serial.println("  3) bare panel needs 4.7k pull-ups to 3V3 (modules have them)");
    Serial.println("  4) address: some modules are 0x3D, not 0x3C");
    Serial.println("Run tools/i2c-scan to see whether ANY device answers.");
    Serial.println("Uptime logic keeps running; per-second output continues below.");
  } else {
    Serial.println("OLED init OK, refreshing every 1s.");
  }

  // 开机先画一帧，避免上电后空屏等到第 1 秒
  render(0);
  lastRefresh = millis();
}

void loop() {
  const uint32_t now = millis();

  // 屏幕不在线时，每 5 秒重试一次初始化。
  // 好处：接线没插好的情况下，插好就能自己恢复，不必重新上电。
  if (!display.ready() && now - lastRetry >= RETRY_MS) {
    lastRetry = now;
    Serial.println("[retry] probing OLED again...");
    if (display.begin()) {
      Serial.println("[retry] OLED came online.");
      lastRefresh = millis();  // 重新对齐刷新节拍
    }
  }

  if (now - lastRefresh < REFRESH_MS) {
    return;  // 非阻塞：到点才干活，其余时间让出 CPU
  }
  lastRefresh += REFRESH_MS;  // 累加而非赋值，避免单次超时造成长期漂移
  ++refreshCount;

  // 以"上电时刻"为原点。millis() 本身即自上电起算，
  // 这里换算成秒，就是需求要的"通电时间"。
  const uint32_t totalSec = now / 1000UL;

  render(totalSec);

  char line[16];
  formatUptime(totalSec, line, sizeof(line));
  Serial.printf("[%lu] uptime=%s  refreshes=%lu\n",
                static_cast<unsigned long>(totalSec), line,
                static_cast<unsigned long>(refreshCount));
}
