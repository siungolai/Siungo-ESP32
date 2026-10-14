/**
 * bus-probe —— I2C 总线物理层体检
 *
 * 适用场景：i2c-scan 里每个地址都返回 endTransmission=5（超时），
 *           说明总线根本没法完成一次事务，问题在物理层而不在地址。
 *           对应接线：SCL -> GPIO4、SDA -> GPIO5、VCC -> 3V3、GND -> GND。
 *
 * 判据（I2C 空闲时 SCL 与 SDA 都应被上拉电阻拉高）：
 *   - 两脚都读到 HIGH      → 上拉正常、线路通，可以怀疑时钟/时序配置
 *   - SDA 恒为 LOW         → SDA 被拉死：接线短路、器件坏、或从机卡在拉低状态
 *   - SCL 恒为 LOW         → SCL 被拉死：同上
 *   - 读到 LOW 但没接任何线 → 引脚被外部短到 GND
 *   - 读到 HIGH 但外部悬空 → 有上拉但没器件，或上拉来自别的通路
 *
 * 先关掉 I2C 外设再当普通 GPIO 读，否则引脚仍被 I2C 矩阵占用，读数无意义。
 */
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

static void report(const char* name, uint8_t pin) {
  // 开内部弱上拉后再读：如果本来是悬空，读数会被内部上拉抬到 HIGH；
  // 如果被外部短到 GND，读数依然是 LOW。两者据此区分。
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  const int hi = digitalRead(pin);

  // 再切成纯输入（无上拉）读一次，用于交叉验证
  pinMode(pin, INPUT);
  delay(5);
  const int fl = digitalRead(pin);

  Serial.printf("%s (GPIO%u): pullup=%s  float=%s\n", name, pin,
                hi == HIGH ? "HIGH" : "LOW", fl == HIGH ? "HIGH" : "LOW");
  return;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== bus-probe: I2C physical layer check ===");
  Serial.printf("SCL=GPIO%u  SDA=GPIO%u\n", PIN_I2C_SCL, PIN_I2C_SDA);
  Serial.println();

  // 关键：先把 I2C 外设从这两个脚上摘下来，否则 GPIO 读数受外设驱动影响
  Wire.end();
  delay(10);

  Serial.println("---- 静态电平 ----");
  report("SCL", PIN_I2C_SCL);
  report("SDA", PIN_I2C_SDA);
  Serial.println();

  Serial.println("---- 时钟脉冲测试（SCL 手动翻转 9 次）----");
  // 如果从机把 SDA 拉死了，标准解法是手动打 9 个 SCL 把它的移位寄存器走完，
  // 从机应当自行释放 SDA。这里顺便验证 SCL 是否真的能被人为拉高拉低。
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT);
  for (uint8_t i = 0; i < 9; ++i) {
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
    if (digitalRead(PIN_I2C_SDA) == HIGH) {
      Serial.printf("  clock %u: SDA=HIGH\n", i + 1);
    } else {
      Serial.printf("  clock %u: SDA=LOW  <-- 从机仍占着总线\n", i + 1);
    }
  }
  pinMode(PIN_I2C_SCL, INPUT);
  Serial.println();

  Serial.println("---- 判读 ----");
  Serial.println("  两脚 pullup 都是 HIGH  -> 电气正常，去看器件地址/供电");
  Serial.println("  任一脚 pullup 是 LOW   -> 该脚被拉死，查短路或坏器件");
  Serial.println("  两脚都是 LOW           -> 大概率没接 OLED，或接到了 GND");
  Serial.println("  9 个时钟后 SDA 仍 LOW  -> 从机死锁，可尝试重新上电");
  Serial.println();
  delay(2000);
}

void loop() {
  // 每隔 3 秒重复测一次，方便边动线材边看结果
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
