/**
 * dht-probe —— DHT11 数据脚物理层自检
 *
 * 用途：dht11-oled 报"读不到"时，先用这个把问题分成三类——
 *   - 数据脚 idle 是 HIGH（被上拉拉高）  -> 供电/接线通，传感器大概活着；
 *                                           读不到就是时序或地址之外的别的问题
 *   - 数据脚 idle 是 LOW（一直贴在 GND） -> 接错脚、接反、或短路
 *   - 数据脚悬空（内部上拉开/关读数不同）-> 传感器根本没接上
 *
 * 判据依据：DHT 单总线空闲时应被上拉电阻拉到 HIGH；
 * 主机发起始信号时才拉低，传感器应答时再拉低 80us。
 *
 * 配套 dht11-oled 使用，解决"程序跑起来但读不到数"的第一步定位。
 */
#include <Arduino.h>

static const uint8_t PIN_DHT_DATA = 7;

/** 开内部上拉读一次，再纯输入读一次，两次不同 = 外部悬空。 */
static void reportIdle() {
  pinMode(PIN_DHT_DATA, INPUT_PULLUP);
  delay(5);
  const int withPullup = digitalRead(PIN_DHT_DATA);

  pinMode(PIN_DHT_DATA, INPUT);
  delay(5);
  const int floating = digitalRead(PIN_DHT_DATA);

  Serial.printf("DATA (GPIO%u): internal-pullup=%s  floating=%s\n", PIN_DHT_DATA,
                withPullup == HIGH ? "HIGH" : "LOW",
                floating == HIGH ? "HIGH" : "LOW");

  if (withPullup == LOW) {
    Serial.println("  -> 数据脚被拉到 GND：接错脚 / 短路 / 传感器内部短路");
  } else if (floating == LOW) {
    Serial.println("  -> 内部上拉开=HIGH、关=LOW：外部悬空，上拉来自 ESP32 自己");
    Serial.println("     传感器可能没接上，或者模块没带自己的上拉电阻");
  } else {
    Serial.println("  -> 空闲为 HIGH，外部上拉在线，供电/接线大概率是通的");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== dht-probe: DHT11 data line idle check ===");
  Serial.printf("DATA = GPIO%u\n", PIN_DHT_DATA);
  Serial.println();
  reportIdle();
  Serial.println();
  Serial.println("每隔 3 秒重测一次，方便边动线材边看结果。");
  Serial.println();
}

void loop() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 3000) {
    return;
  }
  last = now;
  Serial.printf("[%lus] ", static_cast<unsigned long>(now / 1000));
  reportIdle();
}
