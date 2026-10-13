/**
 * blink3 —— 三色 LED 定时闪烁最小示例
 *
 * 红灯：每 1 秒闪一次；蓝灯：每 1 分钟闪一次；绿灯：每 1 小时闪一次。
 * 三个周期各自独立，采用 millis() 非阻塞定时，主循环不被延时函数占用。
 *
 * 目标板：ESP32-S3-DevKitC-1（Arduino 框架，PlatformIO 构建）
 * 接线假设：共阳 RGB 灯珠（公共长脚接 VCC），各色阴极经限流电阻接 GPIO，
 *           低电平点亮（active LOW）。
 */

#include <Arduino.h>

// ==================== 可调参数（只改这一段） ====================

// 时间压缩倍数：1 = 真实周期（生产）；60 = 台架验证（蓝 1s / 绿 60s，一分钟验完全部逻辑）。
// 验证完成后务必改回 1。
#define TIME_SCALE 1

// LED 引脚（ESP32-S3-DevKitC-1 已引出、无特殊功能的 GPIO）
static const uint8_t PIN_LED_RED   = 4;
static const uint8_t PIN_LED_BLUE  = 5;
static const uint8_t PIN_LED_GREEN = 6;

// 闪烁周期（毫秒）：红 1 秒 / 蓝 1 分钟 / 绿 1 小时
static const uint32_t PERIOD_RED_MS   = 1000UL    / TIME_SCALE;
static const uint32_t PERIOD_BLUE_MS  = 60000UL   / TIME_SCALE;
static const uint32_t PERIOD_GREEN_MS = 3600000UL / TIME_SCALE;

// 每次亮灯时长（毫秒）：到点亮这么久再熄灭，本周期内不再动作
static const uint32_t ON_DURATION_MS = 100UL / TIME_SCALE;

// 串口日志波特率（ESP32-S3 原生 USB）
static const uint32_t SERIAL_BAUD = 115200;

// 约束检查：亮灯时长必须大于 0 且短于最短周期，否则会出现"常亮/看不见闪"
static_assert(ON_DURATION_MS > 0, "ON_DURATION_MS rounds to 0 at this TIME_SCALE");
static_assert(ON_DURATION_MS < PERIOD_RED_MS, "ON_DURATION_MS must be shorter than every period");

// ==================== 以下一般无需修改 ====================

// 一个 LED 的完整闪烁状态机
struct Blinker {
  const char* name;        // 日志用名
  uint8_t     pin;         // GPIO 编号
  uint32_t    periodMs;    // 闪烁周期
  uint32_t    onMs;        // 每次亮灯时长
  uint32_t    cycleStartMs;// 本轮周期的起始时刻
  bool        isOn;        // 当前是否点亮
};

static Blinker blinkers[] = {
  { "RED",   PIN_LED_RED,   PERIOD_RED_MS,   ON_DURATION_MS, 0, false },
  { "BLUE",  PIN_LED_BLUE,  PERIOD_BLUE_MS,  ON_DURATION_MS, 0, false },
  { "GREEN", PIN_LED_GREEN, PERIOD_GREEN_MS, ON_DURATION_MS, 0, false },
};

constexpr size_t BLINKER_COUNT = sizeof(blinkers) / sizeof(blinkers[0]);

// 每次点亮时往串口打一条带时间戳的记录，用于验证长周期是否准时
void logBlink(const char* name, uint32_t nowMs) {
  Serial.print("[t=");
  Serial.print(nowMs);
  Serial.print(" ms] ");
  Serial.print(name);
  Serial.println(" ON");
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  uint32_t now = millis();
  for (Blinker& b : blinkers) {
    pinMode(b.pin, OUTPUT);
    digitalWrite(b.pin, HIGH);     // 上电默认熄灭（共阳：低电平才亮）
    b.cycleStartMs = now;          // 以开机为起点，第一次闪烁落在一个整周期后
  }

  Serial.println("blink3: RED=1s / BLUE=1min / GREEN=1h, non-blocking");
}

void loop() {
  uint32_t now = millis();   // 每轮只读一次，三个灯共用同一时间基准

  for (Blinker& b : blinkers) {
    if (now - b.cycleStartMs >= b.periodMs) {
      // 周期到：开灯。用累加而非"取当前时间"推进，避免误差累积导致越闪越慢；
      // now 与 cycleStartMs 均为无符号数，减法在 millis() 约 49.7 天回绕时依然正确。
      b.cycleStartMs += b.periodMs;
      b.isOn = true;
      digitalWrite(b.pin, LOW);    // 点亮（共阳：低电平）
      logBlink(b.name, now);
    } else if (b.isOn && now - b.cycleStartMs >= b.onMs) {
      // 亮够时长：关灯，静候下一个周期
      b.isOn = false;
      digitalWrite(b.pin, HIGH);   // 熄灭（共阳：高电平）
    }
  }
}
