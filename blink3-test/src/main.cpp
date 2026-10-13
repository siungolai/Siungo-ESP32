/**
 * blink3-test —— 硬件判定测试
 *
 * 每秒切换一次电平：
 *   阶段 A：全部 LOW 1s —— 共阳接法（公共端接 3V3）此时三灯应全亮
 *   阶段 B：全部 HIGH 1s —— 共阴接法（公共端接 GND）此时三灯应全亮
 *
 * 判定：
 *   - 只在 A 亮 → 共阳，主程序应保持现在的低电平点亮逻辑
 *   - 只在 B 亮 → 共阴，主程序需把 digitalWrite 高低电平对调回 active HIGH
 *   - 两个阶段都不亮 → 硬件问题：线松、公共端没接好、电阻断、灯珠坏
 */
#include <Arduino.h>

static const uint8_t PINS[] = {4, 5, 6};

void setup() {
  Serial.begin(115200);
  for (uint8_t p : PINS) {
    pinMode(p, OUTPUT);
  }
  Serial.println("blink3-test: 交替全亮判定开始");
}

void loop() {
  Serial.println("TEST A: all LOW  (共阳接法此时应全亮 1s)");
  for (uint8_t p : PINS) digitalWrite(p, LOW);
  delay(1000);

  Serial.println("TEST B: all HIGH (共阴接法此时应全亮 1s)");
  for (uint8_t p : PINS) digitalWrite(p, HIGH);
  delay(1000);
}
