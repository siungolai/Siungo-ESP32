/**
 * link-probe —— I2C 总线物理层体检（1602/PCF8574 专用）
 *
 * 适用场景：i2c-scan 全段 0x08..0x77 都是「无应答」（err==2）且**没有 [ERR] 行**。
 *   这说明总线能正常完成事务、电气没被拉死，只是没有器件应答。
 *   本工具进一步把「引脚电平」和「上拉来源」量出来，区分：
 *
 *     - SCL/SDA 静止电平是 HIGH（约 3.3V）
 *         → 上拉存在、线是通的。屏不应答 = 背包没供电 / 屏坏 / 地址真的不对。
 *     - SCL/SDA 静止电平是 LOW 或飘
 *         → 上拉缺失或被拉死。查 VCC、GND、排线。
 *
 * 判据依据：I2C 空闲时 SCL 与 SDA 都应被上拉电阻拉高。
 *   开内部弱上拉后读一次、纯输入（无上拉）再读一次，交叉验证：
 *     pullup=HIGH float=HIGH  → 外部有上拉或悬空（需靠下面区分）
 *     pullup=HIGH float=LOW   → 外部把线拉死了（上拉靠内部才抬起）
 *     pullup=LOW  float=LOW   → 被外部短到 GND
 *
 * 本工具与 uptime-oled/tools/bus-probe 的区别：
 *   那边测的是 SSD1306(3.3V) 场景，这边针对 1602+PCF8574，
 *   并额外给出「背包到底有没有供电」的辅助判断。
 */
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

/**
 * 读一个引脚的静态电平，并给出上拉来源判读。
 * 返回 pullup 状态与 float 状态，供下方综合判断。
 */
static void report(const char* name, uint8_t pin) {
  // 开内部弱上拉后再读：如果外部悬空，读数会被内部上拉抬到 HIGH；
  // 如果被外部短到 GND，读数依然是 LOW。
  pinMode(pin, INPUT_PULLUP);
  delay(10);
  const int hi = digitalRead(pin);

  // 再切成纯输入（无内部上拉）读一次，用于区分「有外部上拉」和「纯悬空」
  pinMode(pin, INPUT);
  delay(10);
  const int fl = digitalRead(pin);

  Serial.printf("%s (GPIO%u): pullup=%s  float=%s\n", name, pin,
                hi == HIGH ? "HIGH" : "LOW", fl == HIGH ? "HIGH" : "LOW");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== link-probe: I2C physical layer (1602 / PCF8574) ===");
  Serial.printf("SCL=GPIO%u  SDA=GPIO%u\n", PIN_I2C_SCL, PIN_I2C_SDA);
  Serial.println();

  // 关键：先把 I2C 外设从这两个脚上摘下来，
  // 否则引脚仍被 I2C 矩阵占用，GPIO 读数没有意义。
  Wire.end();
  delay(20);

  Serial.println("---- 静态电平 ----");
  report("SCL", PIN_I2C_SCL);
  report("SDA", PIN_I2C_SDA);
  Serial.println();

  // 把两脚都设成带内部上拉的输入，保持在这个状态供下面判读
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  delay(10);
  const int sclHi = digitalRead(PIN_I2C_SCL);
  const int sdaHi = digitalRead(PIN_I2C_SDA);

  Serial.println("---- 判读 ----");
  if (sclHi == HIGH && sdaHi == HIGH) {
    Serial.println("两脚都是 HIGH -> 上拉正常、总线电气通。");
    Serial.println("  此时若 i2c-scan 全段无应答，问题在屏那一侧：");
    Serial.println("    a) 背包 VCC 是否真的接到 3V3（量一下，别只看插头）");
    Serial.println("    b) 屏上背光亮不亮？不亮 = 供电没到");
    Serial.println("    c) 排线是否插到面包板另一排（最常见假接触）");
    Serial.println("    d) 换一片背包/屏试");
  } else if (sclHi == LOW && sdaHi == LOW) {
    Serial.println("两脚都是 LOW -> 大概率没接任何器件，或接到了 GND。");
    Serial.println("  查：SCL/SDA 是否插对排针、是否插到面包板同一列短路。");
  } else {
    Serial.println("只有一脚是 LOW -> 那一脚被拉死。");
    Serial.println("  查：该脚是否短到 GND、排线是否断/错位。");
  }
  Serial.println();

  // 时钟脉冲测试：手动打 9 个 SCL。
  // 若从机把 SDA 拉死了，这是标准解法（把它的移位寄存器走完，从机应释放 SDA）。
  Serial.println("---- 时钟脉冲测试（SCL 手动翻转 9 次）----");
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT);
  for (uint8_t i = 0; i < 9; ++i) {
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
    const int sda = digitalRead(PIN_I2C_SDA);
    Serial.printf("  clock %u: SDA=%s%s\n", i + 1, sda == HIGH ? "HIGH" : "LOW",
                  sda == HIGH ? "" : "   <-- 从机仍占着总线");
  }
  pinMode(PIN_I2C_SCL, INPUT);
  Serial.println();
  delay(2000);
}

void loop() {
  // 每 3 秒重复测一次，方便边动线材边看结果
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 3000) {
    return;
  }
  last = now;

  Wire.end();
  delay(5);
  Serial.printf("[%lus] ", static_cast<unsigned long>(now / 1000));
  report("SCL", PIN_I2C_SCL);
  Serial.print("         ");
  report("SDA", PIN_I2C_SDA);
}
