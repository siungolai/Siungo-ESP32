/**
 * dht11.h —— DHT11 单总线裸驱（自写时序，不依赖 Adafruit DHT 库）
 *
 * ── 为什么要自己写，而不是用 Adafruit 的 DHT 库 ──────────────────────
 * 库有两个结构性问题，本项目两个都实机踩到了：
 *
 * 1. **没有"读一次拿两个值"的接口。**
 *    `readHumidity()` 和 `readTemperature()` 各自都会去调一次 `read()`，
 *    而 DHT11 的 40 位是**一次不可分割的传输**，5 个字节里
 *    data[0..1]=湿度、data[2..3]=温度、data[4]=校验和。
 *    库把 `data[5]` 设成 private，外面拿不到，
 *    于是想同时要温湿度就只能调两次 —— 而 DHT11 两次转换之间
 *    必须间隔 ≥1s，背靠背连调两次，**第二次读到的是垃圾**。
 *    实机症状就是：湿度正常、温度永远是 `--`。
 *
 * 2. **超时是关中断自旋。** `expectPulse()` 里的
 *    `while (digitalRead(_pin) == level)` 在 InterruptLock 保护下自旋，
 *    传感器不在线时累计超时足以触发 Interrupt wdt timeout 整板重启。
 *
 * 自己写之后这两条都从根上没了：一次传输解出全部 5 个字节，
 * 且时序循环每轮都有明确的 cycle 上限，不可能无限自旋。
 *
 * ── 时序依据 DHT11 数据手册 ────────────────────────────────────────
 *   主机拉低 ≥18ms 起始 -> 主机释放 -> 传感器应答 80us 低 + 80us 高
 *   -> 40 位数据，每位 = 50us 低 + (26~28us=0 / 70us=1) 高
 *   -> 结束 50us 低
 *
 * 判定"0 还是 1"用的是**高低电平的相对宽度**，不是绝对微秒数：
 * 每位都先测低电平持续了多少个循环，再测高电平的循环数，
 * 高 > 低 就是 1。这样不依赖具体的 CPU 主频，换板子也不用改常数。
 */
#pragma once

#include <Arduino.h>

#include <stdint.h>

// DHT11 量程（数据手册）：湿度 20~90 %RH，温度 0~50 °C。
// 超出范围基本是位流被干扰读错了，而不是环境真那么极端。
static const float DHT11_HUM_MIN = 20.0f;
static const float DHT11_HUM_MAX = 90.0f;
static const float DHT11_TEMP_MIN = 0.0f;
static const float DHT11_TEMP_MAX = 50.0f;

// 失败原因，用来在屏幕上给出"该去查什么"的提示
enum DhtStatus : uint8_t {
  DHT_OK = 0,
  DHT_ERR_NO_RESPONSE,  // 传感器没应答起始信号
  DHT_ERR_TIMEOUT,      // 读到一半超时（线太长/干扰/器件半死）
  DHT_ERR_CHECKSUM,     // 40 位收全了但校验和对不上
  DHT_ERR_RANGE,        // 校验和对，但数值离谱（超出 DHT11 量程）
};

struct DhtReading {
  float temperature = 0.0f;  // 摄氏度
  float humidity = 0.0f;     // 相对湿度 %
  bool tempValid = false;
  bool humValid = false;
  uint8_t status = DHT_OK;
  uint32_t atMs = 0;  // 本次采样的 millis() 时间戳
};

/**
 * 读一次 DHT11。**一次传输解出温度和湿度两个值。**
 *
 * 返回 status == DHT_OK 表示校验和通过、两个量都可用。
 * 调用方仍需按 >= 1s 的节拍调度（DHT11 物理上限 1Hz），
 * 本函数不做节流——节流是调用方的职责，这样时序语义更清楚。
 */
inline uint8_t dht11Read(uint8_t pin, DhtReading& out) {
  out.atMs = millis();
  out.tempValid = false;
  out.humValid = false;
  out.status = DHT_OK;

  uint8_t data[5] = {0, 0, 0, 0, 0};

  // ---- 1. 主机起始信号：拉低 20ms ----
  // 数据手册要求 ≥18ms；20ms 留余量。
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);

  // ---- 2. 释放总线，切到输入并开上拉，等传感器应答 ----
  pinMode(pin, INPUT_PULLUP);

  // 用 cycle 计数做超时，绝不用"等某个电平变化"的无界循环。
  // cyclesPerUs 是每微秒的 cycle 数，用它把"微秒"换算成"cycle 上限"。
  const uint32_t cyclesPerUs = microsecondsToClockCycles(1);

  // 等传感器把线拉低（应答信号的前半段，80us 低）
  uint32_t cycles = 0;
  const uint32_t lowTimeout = cyclesPerUs * 100;  // 100us 上限，手册是 80us
  while (digitalRead(pin) == HIGH) {
    if (++cycles > lowTimeout) {
      out.status = DHT_ERR_NO_RESPONSE;
      return out.status;
    }
  }

  // 等传感器把线拉高（应答信号的后半段，80us 高）
  cycles = 0;
  const uint32_t highTimeout = cyclesPerUs * 100;
  while (digitalRead(pin) == LOW) {
    if (++cycles > highTimeout) {
      out.status = DHT_ERR_NO_RESPONSE;
      return out.status;
    }
  }

  // 再等应答高电平结束（线重新变低，进入第 1 位）
  cycles = 0;
  while (digitalRead(pin) == HIGH) {
    if (++cycles > highTimeout) {
      out.status = DHT_ERR_TIMEOUT;
      return out.status;
    }
  }

  // ---- 3. 收 40 位：每位 = 50us 低 + (26~28us 或 70us) 高 ----
  // 这里关中断是**必要的**：40 位时序对中断抖动很敏感。
  // 但与库不同的是，这里的每个等待循环都有 cycle 上限，不可能卡死。
  noInterrupts();
  for (int i = 0; i < 40; ++i) {
    // 先量这一位的**低电平**持续了多少个循环，作为本位的基准宽度。
    uint32_t lowCycles = 0;
    const uint32_t bitLowTimeout = cyclesPerUs * 100;
    while (digitalRead(pin) == LOW) {
      ++lowCycles;
      if (lowCycles > bitLowTimeout) {
        interrupts();
        out.status = DHT_ERR_TIMEOUT;
        return out.status;
      }
    }

    // 再量高电平持续了多少个循环
    uint32_t highCycles = 0;
    const uint32_t bitHighTimeout = cyclesPerUs * 100;
    while (digitalRead(pin) == HIGH) {
      ++highCycles;
      if (highCycles > bitHighTimeout) {
        interrupts();
        out.status = DHT_ERR_TIMEOUT;
        return out.status;
      }
    }

    // 判 0/1：比较**本位的**高低电平实测宽度，而不是拿绝对值去比。
    //
    // 为什么必须自校准：`digitalRead()` 不是一条指令，循环体本身有开销，
    // 实测循环数是被这个开销放大过的。若拿"名义 50us × cyclesPerUs"当阈值，
    // 阈值会比任何真实读数都大得多，**所有位都会被判成 0**。
    //
    // 手册规定每位都是"50us 低 + 变长高"，且 0 与 1 的**低电平宽度相同**：
    //   0 -> 低 ~50us，高 ~26us   => 高 < 低
    //   1 -> 低 ~50us，高 ~70us   => 高 > 低
    // 所以"高 > 低"就是 1。这样两个量都是同一环境下的实测值，
    // 循环开销被同时放大、比较时正好抵消，与 CPU 主频也无关。
    if (highCycles > lowCycles) {
      data[i / 8] |= (1u << (7 - (i % 8)));
    }
  }
  interrupts();

  // ---- 4. 校验和：前 4 字节之和的低 8 位应等于第 5 字节 ----
  const uint8_t sum = data[0] + data[1] + data[2] + data[3];
  if (data[4] != sum) {
    out.status = DHT_ERR_CHECKSUM;
    return out.status;
  }

  // ---- 5. 解码 ----
  // DHT11 的 data[1] 和 data[3] 恒为 0（小数位），只有整数部分有意义。
  out.humidity = static_cast<float>(data[0]);
  out.temperature = static_cast<float>(data[2]);

  out.humValid = out.humidity >= DHT11_HUM_MIN && out.humidity <= DHT11_HUM_MAX;
  out.tempValid =
      out.temperature >= DHT11_TEMP_MIN && out.temperature <= DHT11_TEMP_MAX;

  if (!out.humValid || !out.tempValid) {
    out.status = DHT_ERR_RANGE;
    return out.status;
  }

  out.status = DHT_OK;
  return out.status;
}

/** 把状态码翻成一句能直接画到屏幕上的 ASCII 短语（字库只有 ASCII）。 */
inline const char* dhtStatusText(uint8_t status) {
  switch (status) {
    case DHT_OK:
      return "OK";
    case DHT_ERR_NO_RESPONSE:
      return "NO RESPONSE";
    case DHT_ERR_TIMEOUT:
      return "TIMEOUT";
    case DHT_ERR_CHECKSUM:
      return "BAD CHECKSUM";
    case DHT_ERR_RANGE:
      return "OUT OF RANGE";
    default:
      return "UNKNOWN";
  }
}

/**
 * 判断数据脚上"到底有没有一个活着的 DHT"。
 *
 * 为什么不能只读一次电平就算了：ESP32 开了内部上拉时，引脚**自己**就把线
 * 拉到了 HIGH，这个读数证明不了外部有东西。实机踩过的坑就是这个——
 * 传感器悬空时读到 HIGH，于是放行，库进去自旋到看门狗重启。
 *
 * 所以这里做的是"拔掉自身上拉再读"：把引脚设成纯输入（无上拉），
 * 此时线还能是 HIGH，只可能来自**外部**的上拉电阻——也就是传感器模块
 * 真的接上了（市售模块都自带 4.7k/10k 上拉）。反之读到 LOW 就是悬空或拉死。
 *
 * 注：自写时序之后这条崩溃路径其实已经不存在了（每个循环都有上限），
 * 但这个判定仍然是**给用户看的诊断信息**——"没接传感器"和"接了但读错"
 * 的排查方向完全不同，值得在屏幕上直接说清楚。
 */
inline bool dhtDataLineLooksAlive(uint8_t pin, bool* externallyPulled) {
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(100);

  pinMode(pin, INPUT);  // 纯高阻输入：不再由 ESP32 提供任何上拉
  delayMicroseconds(200);
  const int floating = digitalRead(pin);

  if (externallyPulled != nullptr) {
    *externallyPulled = (floating == HIGH);
  }
  return floating == HIGH;
}

/**
 * 把一次采样拍成一行固定字段的 CSV，便于直接丢进表格看趋势。
 * 无效的量写 `--`，而不是 0——0 是合法读数，混在一起就分不清了。
 * 例：`t=12s,hum=63,temp=24,ok=1,fail=0`
 */
inline void formatDhtCsv(uint32_t sec, const DhtReading& r, uint32_t failCount,
                         char* out, size_t outLen) {
  char hum[12];
  char temp[12];
  if (r.humValid) {
    snprintf(hum, sizeof(hum), "%ld", static_cast<long>(lroundf(r.humidity)));
  } else {
    snprintf(hum, sizeof(hum), "--");
  }
  if (r.tempValid) {
    snprintf(temp, sizeof(temp), "%ld", static_cast<long>(lroundf(r.temperature)));
  } else {
    snprintf(temp, sizeof(temp), "--");
  }

  snprintf(out, outLen, "t=%lus,hum=%s,temp=%s,ok=%u,fail=%lu",
           static_cast<unsigned long>(sec), hum, temp,
           static_cast<unsigned>(r.status == DHT_OK ? 1 : 0),
           static_cast<unsigned long>(failCount));
}
