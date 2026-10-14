/**
 * i2c-scan —— 扫一遍 I2C 总线，看 PCF8574 背包到底应不应答、在哪个地址。
 *
 * 用途：lcd1602-uptime 报"0x27 无应答"时，先用这个把问题分成两类——
 *   - 扫到地址（0x27 / 0x3F / 其它）→ 器件活着，是驱动地址或接线顺序的问题；
 *   - 一个地址都扫不到              → 接线 / 供电 / 上拉 / 电平转换的问题，
 *                                     跟代码无关。
 *
 * 扫描范围 0x08..0x77 是 I2C 规范保留段之外的全部可用 7 位地址。
 *
 * ---- 5V 供电专项提示 ----
 * 本项目按 5V 给 LCD 供电。PCF8574 背包的板上拉电阻挂在背包 VCC 上，
 * 所以 5V 供电时 SDA/SCL 的 HIGH 就是 5V。ESP32-S3 的 IO 不耐 5V，
 * 必须先过电平转换模块再看这份扫描结果。
 * 扫描能在**电平转换之后**跑通，才说明链路真的可用。
 */
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;
static const uint32_t I2C_HZ = 100000;  // PCF8574 标称上限 100kHz

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=== i2c-scan (PCF8574 backpack) ===");
  Serial.printf("SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL, PIN_I2C_SDA,
                static_cast<unsigned long>(I2C_HZ));
  Serial.println("Target: PCF8574 @ 0x27 (or PCF8574A @ 0x3F)");
  Serial.println();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
  Wire.setTimeOut(50);
  delay(100);
}

void loop() {
  uint8_t found = 0;

  Serial.println("---- 扫描 0x08..0x77 ----");
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    const uint8_t err = Wire.endTransmission();

    if (err == 0) {
      ++found;
      Serial.printf("  [OK ] 0x%02X  应答\n", addr);
      if (addr >= 0x20 && addr <= 0x27) {
        Serial.println("         ^ PCF8574 (A0/A1/A2 全接地时是 0x27)");
      } else if (addr >= 0x38 && addr <= 0x3F) {
        Serial.println("         ^ PCF8574A 版本，改 lcd1602-uptime 的地址常量");
      }
    } else if (err != 2) {
      // err==2 是"地址无应答"，属正常空地址；其它错误码说明总线本身有问题
      Serial.printf("  [ERR] 0x%02X  endTransmission=%u\n", addr, err);
    }
    delay(2);
  }

  Serial.println();
  if (found == 0) {
    Serial.println("结论：总线上一颗器件都没有。按顺序查：");
    Serial.println("  1) SCL 是否接 GPIO4、SDA 是否接 GPIO5（接反通常就是全无应答）");
    Serial.println("  2) 背包 VCC/GND 是否接好；屏上背光亮不亮（亮=供电到了）");
    Serial.println("  3) 5V 供电时 SDA/SCL 是否经过电平转换模块；");
    Serial.println("     没经过就直接接 ESP32 属于超压，必须停下来改线");
    Serial.println("  4) 排线是否插到面包板另一排（最常见的假接触）");
    Serial.println("  5) 换一片模块试——PCF8574 坏掉的情况也有");
  } else {
    Serial.printf("结论：扫到 %u 颗器件，对照上面列出的地址。\n", found);
  }
  Serial.println();
  Serial.println("错误码参考：1=数据过长 2=地址无应答 3=数据无应答 4=其它 5=超时");
  Serial.println();
  delay(3000);
}
