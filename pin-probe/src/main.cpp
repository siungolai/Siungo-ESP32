/**
 * pin-probe —— 板载 WS2812 通路自检（临时项目，查完可删）
 *
 * 目的：把"代码问题"和"硬件/引脚问题"一刀切开。
 *
 *   阶段 1：neopixelWrite(21, ...) 白 → 红 → 绿 → 蓝  循环
 *   阶段 2：neopixelWrite(48, ...) 同一串颜色
 *
 * 判读：
 *   - 只有阶段 1 亮 → 灯在 GPIO 21（用 GPIO 21 是对的）
 *   - 只有阶段 2 亮 → 灯在 GPIO 48（ESP32-S3-DevKitC-1 v1.0 的板载 WS2812；v1.1 无此灯）
 *   - 两个都不亮   → 硬件问题：这块板没有板载 WS2812，或灯/走线损坏
 *
 * 为什么不能写 RGB_BUILTIN：core 2.0.17 的 S3 变体里
 *     LED_BUILTIN = SOC_GPIO_PIN_COUNT + PIN_NEOPIXEL   // 48 + 48 = 96
 *     RGB_BUILTIN = LED_BUILTIN                          // 96
 * 而 esp32-hal-rgb-led.c 会做
 *     if (pin == RGB_BUILTIN) { _pin = RGB_BUILTIN - SOC_GPIO_PIN_COUNT; }   // 96 - 48 = 48
 * 数值上碰巧能算回 48，但那是在赌"编译期常量 == 芯片运行时常量"；换变体就可能错位。
 * 这里一律传字面量，完全绕开那行改写。
 *
 * 目标板：ESP32-S3-DevKitC-1（Arduino 框架，PlatformIO 构建），零第三方依赖。
 */

#include <Arduino.h>

static const uint32_t SERIAL_BAUD = 115200;

// 每段颜色停多久
static const uint32_t STEP_MS = 700;

// 候选数据脚：先试 21（常见的外接灯珠接法），再试 48（S3-DevKitC-1 官方板载灯）
static const uint8_t PROBE_PINS[] = {21, 48};
constexpr size_t PROBE_PIN_COUNT = sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0]);

// 白 / 红 / 绿 / 蓝
struct Rgb { uint8_t r, g, b; const char* name; };
static const Rgb COLORS[] = {
  { 255, 255, 255, "WHITE" },
  { 255,   0,   0, "RED"   },
  {   0, 255,   0, "GREEN" },
  {   0,   0, 255, "BLUE"  },
};
constexpr size_t COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);   // 等原生 USB CDC 枚举完，避免开头几行日志丢失
  Serial.println();
  Serial.println("pin-probe: 板载 WS2812 通路自检开始");
  Serial.println("先看哪一段把灯点亮 —— 第 1 段=GPIO21，第 2 段=GPIO48");
}

void loop() {
  for (size_t p = 0; p < PROBE_PIN_COUNT; p++) {
    uint8_t pin = PROBE_PINS[p];
    Serial.print(">>> 阶段 ");
    Serial.print(p + 1);
    Serial.print("/");
    Serial.print(PROBE_PIN_COUNT);
    Serial.print("  用 GPIO ");
    Serial.println(pin);

    for (size_t c = 0; c < COLOR_COUNT; c++) {
      const Rgb& col = COLORS[c];
      Serial.print("    GPIO ");
      Serial.print(pin);
      Serial.print("  ");
      Serial.print(col.name);
      Serial.print("  (");
      Serial.print(col.r);
      Serial.print(",");
      Serial.print(col.g);
      Serial.print(",");
      Serial.println(col.b);

      // 传字面量 pin，不要传 RGB_BUILTIN / LED_BUILTIN（见文件头注释）
      neopixelWrite(pin, col.r, col.g, col.b);
      delay(STEP_MS);
    }

    neopixelWrite(pin, 0, 0, 0);   // 本阶段结束先熄灭
    delay(200);
  }

  Serial.println("--- 一轮结束，重复 ---");
  delay(500);
}
