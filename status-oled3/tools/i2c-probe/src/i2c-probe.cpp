/**
 * i2c-probe.cpp -- v7: the SDA/SCL order test
 *
 * Root cause candidate, reported by the operator 2026-10-17:
 *   "g4 connects scl; g5 connects sda"
 * i.e. on the bench, GPIO4 -> module SCL and GPIO5 -> module SDA.
 *
 * config.h had I2C_SDA_PIN = 4 and I2C_SCL_PIN = 5, which is the opposite
 * order. So the two lines were crossed between firmware and wiring. config.h
 * was fixed to I2C_SDA_PIN = 5 / I2C_SCL_PIN = 4 on 2026-10-17; this tool
 * keeps both orders in the scan so the crossing can never silently return.
 *
 * I2C START, STOP and ACK are all defined relative to SDA's edge against
 * SCL. Swap the two and no slave can ever recognise a START, which is
 * exactly the symptom we have been chasing: idle HIGH/HIGH, pull-ups alive
 * and on a live rail, every address NACK, zero timeouts.
 *
 * Every earlier "swapped" pass in this tool was invalid. It lacked
 * Wire.end(), and Wire.cpp:300-303 makes a second begin() on the same bus a
 * silent no-op that never reaches initPins(), so those passes actually
 * re-scanned GPIO4/GPIO5. That is why every pass printed byte-identical
 * output. This version calls Wire.end() before every begin().
 *
 * Three scans in one run, so a single flashing settles the question:
 *   A1  EMPTY bus GPIO10/GPIO11   -- unchanged control, board stays honest
 *   A2  SDA=GPIO5 SCL=GPIO4       -- config.h's current order, matches the bench
 *   A3  SDA=GPIO4 SCL=GPIO5       -- the crossed order that never answers
 */
#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kSdaModule = 5;   // config.h: I2C_SDA_PIN
constexpr uint8_t kSclModule = 4;   // config.h: I2C_SCL_PIN
constexpr uint8_t kSdaEmpty = 10;
constexpr uint8_t kSclEmpty = 11;

constexpr uint32_t kBusHz = 400000;
constexpr uint8_t kTimeoutMs = 50;

constexpr uint8_t kScanFirst = 0x08;
constexpr uint8_t kScanLast = 0x77;

struct Histogram {
  uint16_t ok = 0;
  uint16_t nackAddr = 0;   // 2
  uint16_t nackData = 0;   // 3
  uint16_t other = 0;      // 4
  uint16_t timeout = 0;    // 5
};

void probeAddress(uint8_t addr, Histogram& h) {
  Serial.print("  0x");
  if (addr < 0x10) Serial.print('0');
  Serial.print(static_cast<unsigned>(addr), HEX);
  Serial.print(" -> ");

  Wire.beginTransmission(addr);
  const uint8_t err = Wire.endTransmission();
  switch (err) {
    case 0:  Serial.println("ACK");         ++h.ok; break;
    case 2:  Serial.println("2 nack_addr"); ++h.nackAddr; break;
    case 3:  Serial.println("3 nack_data"); ++h.nackData; break;
    case 5:  Serial.println("5 TIMEOUT");   ++h.timeout; break;
    default: Serial.println(err);           ++h.other; break;
  }
  Serial.flush();
}

void printSummary(const char* label, const Histogram& h) {
  Serial.print("  summary: ack=");
  Serial.print(h.ok);
  Serial.print(" nack_addr=");
  Serial.print(h.nackAddr);
  Serial.print(" nack_data=");
  Serial.print(h.nackData);
  Serial.print(" timeout=");
  Serial.print(h.timeout);
  Serial.print(" other=");
  Serial.print(h.other);
  Serial.print(" (of ");
  Serial.print(static_cast<unsigned>(kScanLast - kScanFirst + 1));
  Serial.println(" addresses)");
  Serial.print("  ");
  Serial.println(label);
  Serial.flush();
}

void probeKeyAddresses() {
  Wire.beginTransmission(0x70);
  const uint8_t err70 = Wire.endTransmission();
  Serial.print("  mux  0x70 -> endTransmission()=");
  Serial.println(static_cast<unsigned>(err70));
  Serial.flush();

  Wire.beginTransmission(0x3C);
  const uint8_t err3c = Wire.endTransmission();
  Serial.print("  oled 0x3C -> endTransmission()=");
  Serial.println(static_cast<unsigned>(err3c));
  Serial.flush();
}

// Idle level as found, read with the pin as a plain input with no internal
// pull-up, before Wire.begin() turns the I2C peripheral on. This is the only
// way to tell "pulled up by something external" from "the ESP32's own pullup".
void reportIdle(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT);
  pinMode(scl, INPUT);
  const int sdaLevel = digitalRead(sda);
  const int sclLevel = digitalRead(scl);
  Serial.print("  idle before Wire.begin: SDA=");
  Serial.print(sdaLevel == HIGH ? "HIGH" : "LOW");
  Serial.print(" SCL=");
  Serial.println(sclLevel == HIGH ? "HIGH" : "LOW");
  Serial.flush();
}

void runScan(uint8_t sda, uint8_t scl, const char* label) {
  Serial.println();
  Serial.print("--- ");
  Serial.print(label);
  Serial.print(" : SDA=GPIO");
  Serial.print(static_cast<unsigned>(sda));
  Serial.print(" SCL=GPIO");
  Serial.print(static_cast<unsigned>(scl));
  Serial.print(" at ");
  Serial.print(static_cast<unsigned long>(kBusHz / 1000));
  Serial.println(" kHz ---");
  Serial.flush();

  reportIdle(sda, scl);

  // Wire.end() first, ALWAYS. Without it the second begin() on this bus is a
  // silent no-op (Wire.cpp:300-303) and this scan would re-use the previous
  // pin pair, which is exactly the mistake that produced v4's phantom
  // "everything returns error 5" and made every swapped pass look identical.
  Wire.end();
  Wire.begin(sda, scl, kBusHz);
  Wire.setTimeOut(kTimeoutMs);

  Histogram h;
  for (uint8_t addr = kScanFirst; addr <= kScanLast; ++addr) {
    probeAddress(addr, h);
    // A heartbeat so a wedge is visible as a gap, and the last line printed
    // names the address where it happened.
    if ((addr & 0x0F) == 0x0F) {
      Serial.print("  ... reached 0x");
      if (addr < 0x10) Serial.print('0');
      Serial.println(static_cast<unsigned>(addr), HEX);
      Serial.flush();
    }
  }
  printSummary(h.ok == 0 ? "NOTHING answered on this bus" : "SOMETHING ACKED", h);
  probeKeyAddresses();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== status-oled3 i2c-probe v7 (SDA/SCL order test) ===");
  Serial.println("bench wiring reported by operator: GPIO4->SCL, GPIO5->SDA");
  Serial.println("config.h currently asks for:      GPIO5=SDA, GPIO4=SCL");
  Serial.println();
  Serial.println("A2 is the order that matches the bench. If A2 ACKs 0x70 and");
  Serial.println("A3 does not, the lines are crossed and the fix is two pins");
  Serial.println("in config.h -- no rewiring needed.");
  Serial.flush();

  runScan(kSdaEmpty, kSclEmpty, "scan A1 EMPTY bus GPIO10/GPIO11 (control)");

  runScan(kSdaModule, kSclModule, "scan A2 SDA=GPIO5 SCL=GPIO4 (matches bench)");

  runScan(kSclModule, kSdaModule, "scan A3 SDA=GPIO4 SCL=GPIO5 (crossed order)");

  Wire.end();
  Serial.println();
  Serial.println("=== probe done; idling (no bus traffic) ===");
  Serial.flush();
}

void loop() {
  delay(1000);
}
