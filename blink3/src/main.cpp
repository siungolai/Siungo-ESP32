/**
 * blink3 —— 三色定时闪烁最小示例（板载 WS2812 模式 / 外接三灯模式）
 *
 * 默认用「板载 WS2812 全彩灯」一颗灯，按 红 1s / 蓝 1min / 绿 1h 依次闪三色，
 * 与板子的 BOOT 键（GPIO 0）配合做上电自检：开机能亮 = 板载灯通路完全正常。
 *
 * 把下面的 ENABLE_ONBOARD_RGB 改成 0，即切回原版「外接三色 LED」方案：
 * GPIO 4 / 5 / 6，共阳灯珠（公共端接 3V3，低电平点亮）。
 *
 * 目标板：ESP32-S3-DevKitC-1（Arduino 框架，PlatformIO 构建）
 */

#include <Arduino.h>

// ==================== 可调参数（只改这一段） ====================

// 1 = 用板载 WS2812 全彩灯（默认，需要 Adafruit NeoPixel 库，见 platformio.ini）
// 0 = 用外接三色 LED（GPIO 4/5/6，共阳接法），此时无任何第三方依赖
#define ENABLE_ONBOARD_RGB 1

// 时间压缩倍数：1 = 真实周期（生产）；60 = 台架验证（蓝 1s / 绿 60s，一分钟验完全部逻辑）。
// 验证完成后务必改回 1。
#define TIME_SCALE 1

// 外接三色 LED 的引脚（仅 ENABLE_ONBOARD_RGB = 0 时使用）
static const uint8_t PIN_LED_RED   = 4;
static const uint8_t PIN_LED_BLUE  = 5;
static const uint8_t PIN_LED_GREEN = 6;

// 板载 WS2812 的数据脚。变体宏 PIN_NEOPIXEL 走编译期，正好是 S3 DevKitC 的 GPIO 48。
//
// 不要去用核心的 RGB_BUILTIN / LED_BUILTIN：core 2.0.17 里
//     LED_BUILTIN = SOC_GPIO_PIN_COUNT + PIN_NEOPIXEL   // 96
//     RGB_BUILTIN = LED_BUILTIN                          // 96
// 而 esp32-hal-rgb-led.c 会做
//     if (pin == RGB_BUILTIN) _pin = RGB_BUILTIN - SOC_GPIO_PIN_COUNT;   // 96 - 48 = 48
// 数值上碰巧算回 48，但那是在赌"编译期常量 == 芯片运行时常量"。传字面量最稳。
// 另外：只有 S3-DevKitC-1 v1.0 有板载灯，v1.1 起取消了。灯不亮先烧 pin-probe 判定。
#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 48
#endif
static const uint8_t PIN_ONBOARD = (uint8_t)PIN_NEOPIXEL;

// 板载灯亮度：板载 WS2812 直怼 3.3V 很刺眼，压到 1/4 量级即可看清
static const uint8_t ONBOARD_BRIGHTNESS = 64;

// 闪烁周期（毫秒）：红 1 秒 / 蓝 1 分钟 / 绿 1 小时
static const uint32_t PERIOD_RED_MS   = 1000UL    / TIME_SCALE;
static const uint32_t PERIOD_BLUE_MS  = 60000UL   / TIME_SCALE;
static const uint32_t PERIOD_GREEN_MS = 3600000UL / TIME_SCALE;

// 每次亮灯时长（毫秒）：到点亮这么久再熄灭，本周期内不再动作
static const uint32_t ON_DURATION_MS = 100UL / TIME_SCALE;

// 串口日志波特率（ESP32-S3 原生 USB）
static const uint32_t SERIAL_BAUD = 115200;

// 开机自检：上电先白色闪两下，让"板载灯通路是否正常"一眼可见。
// 只想安静看周期闪烁时改成 0。
#define ONBOARD_SELFTEST 1

// 约束检查：亮灯时长必须大于 0 且短于最短周期，否则会出现"常亮/看不见闪"
static_assert(ON_DURATION_MS > 0, "ON_DURATION_MS rounds to 0 at this TIME_SCALE");
static_assert(ON_DURATION_MS < PERIOD_RED_MS, "ON_DURATION_MS must be shorter than every period");

// ==================== 以下一般无需修改 ====================

#if ENABLE_ONBOARD_RGB
#include <Adafruit_NeoPixel.h>
// NEO_GRB + NEO_KHZ800 是 WS2812B 的通用参数
static Adafruit_NeoPixel onboardPixel(1, PIN_ONBOARD, NEO_GRB + NEO_KHZ800);
#endif

// 一个 LED 的完整闪烁状态机
struct Blinker {
  const char* name;        // 日志用名
  uint8_t     pin;         // GPIO 编号（板载模式下为数据脚，仅作记录）
  uint32_t    periodMs;    // 闪烁周期
  uint32_t    onMs;        // 每次亮灯时长
  uint32_t    cycleStartMs;// 本轮周期的起始时刻
  bool        isOn;        // 当前是否点亮
  uint8_t     r, g, b;     // 该"灯"的颜色（板载模式下决定 WS2812 显示的颜色）
};

// 颜色表：红 / 蓝 / 绿（与外接三灯的物理颜色一致）
static Blinker blinkers[] = {
  { "RED",   PIN_LED_RED,   PERIOD_RED_MS,   ON_DURATION_MS, 0, false, 255,   0,   0 },
  { "BLUE",  PIN_LED_BLUE,  PERIOD_BLUE_MS,  ON_DURATION_MS, 0, false,   0,   0, 255 },
  { "GREEN", PIN_LED_GREEN, PERIOD_GREEN_MS, ON_DURATION_MS, 0, false,   0, 255,   0 },
};

constexpr size_t BLINKER_COUNT = sizeof(blinkers) / sizeof(blinkers[0]);

// 点亮/熄灭某一路。板载模式下是"给板载灯换颜色"，因为一颗灯同一时刻只能显示一种颜色。
void setLamp(const Blinker& b, bool on) {
#if ENABLE_ONBOARD_RGB
  if (on) {
    onboardPixel.setPixelColor(0, onboardPixel.Color(b.r, b.g, b.b));
  } else {
    onboardPixel.setPixelColor(0, 0, 0, 0);
  }
  onboardPixel.show();
#else
  // 共阳：拉低才亮；熄灭时恢复高电平
  digitalWrite(b.pin, on ? LOW : HIGH);
#endif
}

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

#if ENABLE_ONBOARD_RGB
  onboardPixel.begin();
  onboardPixel.setBrightness(ONBOARD_BRIGHTNESS);
  onboardPixel.clear();              // 上电先清成熄灭
  onboardPixel.show();

  // 自检：先白色闪两下（0.15s 亮 / 0.15s 灭），看得见就说明板载灯通路 ok。
  // 不依赖 WiFi、不依赖外接线：这条过得去，后面的闪烁就一定是逻辑问题而不是硬件问题。
#if ONBOARD_SELFTEST
  for (uint8_t i = 0; i < 2; i++) {
    onboardPixel.setPixelColor(0, onboardPixel.Color(255, 255, 255));
    onboardPixel.show();
    delay(150);
    onboardPixel.clear();
    onboardPixel.show();
    delay(150);
  }
#endif
#else
  for (Blinker& b : blinkers) {
    pinMode(b.pin, OUTPUT);
    digitalWrite(b.pin, HIGH);       // 上电默认熄灭（共阳：低电平才亮）
  }
#endif

  uint32_t now = millis();
  for (Blinker& b : blinkers) {
    b.cycleStartMs = now;            // 以开机为起点，第一次闪烁落在一个整周期后
  }

#if ENABLE_ONBOARD_RGB
  Serial.print("blink3: 板载 WS2812 @ GPIO ");
  Serial.print(PIN_ONBOARD);
  Serial.println(" , RED=1s / BLUE=1min / GREEN=1h, non-blocking");
#else
  Serial.println("blink3: 外接三色 LED @ GPIO 4/5/6, RED=1s / BLUE=1min / GREEN=1h, non-blocking");
#endif
}

void loop() {
  uint32_t now = millis();   // 每轮只读一次，三个灯共用同一时间基准

  for (Blinker& b : blinkers) {
    if (now - b.cycleStartMs >= b.periodMs) {
      // 周期到：点亮。用累加而非"取当前时间"推进，避免误差累积导致越闪越慢；
      // now 与 cycleStartMs 均为无符号数，减法在 millis() 约 49.7 天回绕时依然正确。
      b.cycleStartMs += b.periodMs;
      b.isOn = true;
      setLamp(b, true);
      logBlink(b.name, now);
    } else if (b.isOn && now - b.cycleStartMs >= b.onMs) {
      // 亮够时长：熄灭，静候下一个周期
      b.isOn = false;
      setLamp(b, false);
    }
  }
}
