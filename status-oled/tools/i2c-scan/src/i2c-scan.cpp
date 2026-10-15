/**
 * i2c-scan —— 扫一遍 I2C 总线，看 OLED 到底应不应答、在哪个地址。
 *
 * 用途：status-oled 的 P4 总线页显示「0x3C 无应答」时，先用这个把问题分成两类——
 *   - 扫到地址（0x3C / 0x3D / 其它）→ 器件活着，是驱动地址或接线顺序的问题；
 *   - 一个地址都扫不到            → 接线 / 供电 / 上拉电阻的问题，跟代码无关。
 *
 * 扫描范围 0x08..0x77 是 I2C 规范保留段之外的全部可用 7 位地址。
 *
 * 与 status-oled 主程序的差异：这里用 100kHz 扫描（容错高），主程序跑 400kHz。
 */
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;
static const uint32_t I2C_HZ = 100000;  // 扫描先用 100kHz，比 400kHz 容错高

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=== i2c-scan ===");
  Serial.printf("SCL=GPIO%u  SDA=GPIO%u  %lu Hz\n", PIN_I2C_SCL, PIN_I2C_SDA,
                static_cast<unsigned long>(I2C_HZ));
  Serial.println("目标：SSD1306 OLED，期望地址 0x3C 或 0x3D");
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
      if (addr == 0x3C || addr == 0x3D) {
        Serial.println("         ^ 这就是 OLED，改 status-oled 的地址常量即可");
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
    Serial.println("  1) SCL 是否接 GPIO4、SDA 是否接 GPIO5（接反了通常就是全无应答）");
    Serial.println("  2) OLED 的 VCC 接 3V3、GND 接 GND，模块上有电源指示灯的话看它亮不亮");
    Serial.println("  3) 裸屏/无上拉的模块必须自己外挂 4.7k 上拉到 3V3；成品模块一般自带");
    Serial.println("  4) 排线是否插到面包板另一排（最常见的假接触）");
  } else {
    Serial.printf("结论：扫到 %u 颗器件，对照上面列出的地址。\n", found);
  }
  Serial.println();
  Serial.println("错误码参考：1=数据过长 2=地址无应答 3=数据无应答 4=其它 5=超时");
  Serial.println();
  delay(3000);
}
