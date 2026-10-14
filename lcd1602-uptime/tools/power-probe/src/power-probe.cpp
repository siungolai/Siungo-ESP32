/**
 * power-probe —— 背包供电与总线电压体检（脱离 I2C 外设，纯 GPIO 读数）
 *
 * 适用场景：背光亮（说明 VCC/GND 到位），但 i2c-scan 全段无应答，
 *           且 link-probe 显示 SCL/SDA 静态都是 HIGH（上拉存在）。
 *
 * 这个组合最可能的成因是「上拉电阻无源生效，但 PCF8574 芯片没工作」。
 * 本工具把总线按不同方式驱动，用读数反推外部电路的真实状态：
 *
 *   测试 1  两脚纯输入（无内部上拉）
 *     HIGH -> 外部有上拉（背包的 4.7k 在起作用，正常）
 *     LOW  -> 外部没有上拉，link-probe 的 HIGH 是内部上拉造成的假象
 *
 *   测试 2  一脚输出 LOW、另一脚输入
 *     读数应跟着变 LOW（线是通的、没有被别的器件强驱高）
 *     若读数仍是 HIGH -> 该脚被外部强驱高（可能接到了 3V3，接错位）
 *
 *   测试 3  SDA 输出 LOW 时，读 SCL
 *     SCL 也跟着 LOW -> 两脚被外部短在一起（排针短路 / 焊桥）
 *
 * 这三个测试全部只读电平、不发 I2C，即使器件不在线也不会卡住总线。
 */
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_I2C_SCL = 4;
static const uint8_t PIN_I2C_SDA = 5;

static const char* lvl(int v) { return v == HIGH ? "HIGH" : "LOW"; }

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== power-probe: backpack wiring check ===");
  Serial.printf("SCL=GPIO%u  SDA=GPIO%u\n", PIN_I2C_SCL, PIN_I2C_SDA);
  Serial.println();

  // 把 I2C 外设摘下来，两脚交回给 GPIO
  Wire.end();
  delay(20);

  // ---- 测试 1：纯输入，判断外部上拉是否存在 ----
  pinMode(PIN_I2C_SCL, INPUT);
  pinMode(PIN_I2C_SDA, INPUT);
  delay(20);
  const int sclFloat = digitalRead(PIN_I2C_SCL);
  const int sdaFloat = digitalRead(PIN_I2C_SDA);
  Serial.println("---- 测试 1: 纯输入（无内部上拉）----");
  Serial.printf("  SCL=%s  SDA=%s\n", lvl(sclFloat), lvl(sdaFloat));
  if (sclFloat == HIGH && sdaFloat == HIGH) {
    Serial.println("  判读: 外部上拉存在，背包的 4.7k 正在起作用。");
  } else {
    Serial.println("  判读: 外部上拉缺失/不足，总线拉不起来。");
  }
  Serial.println();

  // ---- 测试 2：一脚驱低，看另一脚是否被带着走 ----
  Serial.println("---- 测试 2: SCL 输出 LOW，读 SDA ----");
  pinMode(PIN_I2C_SCL, OUTPUT);
  digitalWrite(PIN_I2C_SCL, LOW);
  pinMode(PIN_I2C_SDA, INPUT);
  delay(20);
  const int sdaWhenSclLow = digitalRead(PIN_I2C_SDA);
  Serial.printf("  SCL=LOW(驱动)  SDA=%s\n", lvl(sdaWhenSclLow));
  Serial.println("  判读: SDA 应为 HIGH（两线独立）。若为 LOW，两脚被短在一起。");
  Serial.println();

  Serial.println("---- 测试 3: SDA 输出 LOW，读 SCL ----");
  pinMode(PIN_I2C_SCL, INPUT);
  pinMode(PIN_I2C_SDA, OUTPUT);
  digitalWrite(PIN_I2C_SDA, LOW);
  delay(20);
  const int sclWhenSdaLow = digitalRead(PIN_I2C_SCL);
  Serial.printf("  SDA=LOW(驱动)  SCL=%s\n", lvl(sclWhenSdaLow));
  Serial.println("  判读: SCL 应为 HIGH（两线独立）。若为 LOW，两脚被短在一起。");
  Serial.println();

  // ---- 测试 4：两脚都驱低，看能否拉下来（有无被外部强驱高）----
  Serial.println("---- 测试 4: 两脚都输出 LOW ----");
  pinMode(PIN_I2C_SCL, OUTPUT);
  pinMode(PIN_I2C_SDA, OUTPUT);
  digitalWrite(PIN_I2C_SCL, LOW);
  digitalWrite(PIN_I2C_SDA, LOW);
  delay(20);
  const int sclLow = digitalRead(PIN_I2C_SCL);
  const int sdaLow = digitalRead(PIN_I2C_SDA);
  Serial.printf("  SCL=%s  SDA=%s  (应为 LOW)\n", lvl(sclLow), lvl(sdaLow));
  if (sclLow == HIGH || sdaLow == HIGH) {
    Serial.println("  判读: 有脚被外部强驱高 —— 该脚可能接到了 3V3，查排针位置。");
  } else {
    Serial.println("  判读: 两脚都能正常拉低，输出驱动没问题。");
  }
  Serial.println();

  // 收尾：交回高阻输入，避免一直驱低总线
  pinMode(PIN_I2C_SCL, INPUT);
  pinMode(PIN_I2C_SDA, INPUT);

  Serial.println("---- 综合判读 ----");
  Serial.println("  上拉存在 + 背光亮 + 全段无应答");
  Serial.println("  => 总线电气正常，但 PCF8574 芯片没响应。查:");
  Serial.println("     a) 背包 4 针顺序是否为 GND/VCC/SDA/SCL（别记错，对着丝印看）");
  Serial.println("     b) VCC 是否真的 3.3V（在排针上量，别量面包板上的线）");
  Serial.println("     c) 背包与屏之间的排针是否虚焊（捏一捏、重新压紧）");
  Serial.println("     d) 换一片背包模块试");
  Serial.println();
  delay(3000);
}

void loop() {
  // 每 5 秒重测测试 1，方便边动线边看
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 5000) {
    return;
  }
  last = now;

  Wire.end();
  delay(5);
  pinMode(PIN_I2C_SCL, INPUT);
  pinMode(PIN_I2C_SDA, INPUT);
  delay(20);
  Serial.printf("[%lus] SCL=%s  SDA=%s  (纯输入)\n",
                static_cast<unsigned long>(now / 1000), lvl(digitalRead(PIN_I2C_SCL)),
                lvl(digitalRead(PIN_I2C_SDA)));
}
