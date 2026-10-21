/**
 * i2c-probe.cpp -- v1: pet-oled3 bringup diagnostic
 *
 * Two questions, one flashing:
 *
 *   Phase A -- is the I2C pair wired the right way round?
 *     I2C START, STOP and ACK are all defined relative to SDA's edge against
 *     SCL. Swap the two lines and no slave can ever recognise a START, even
 *     though every electrical indicator still looks perfect (idle HIGH/HIGH,
 *     pull-ups alive, no timeouts, just NACK on every address). That failure
 *     mode cost the sibling project (status-oled3) 36 hours of bench time, so
 *     this tool always tests BOTH orders on the same two pins and prints which
 *     one actually ACKs.
 *
 *   Phase B -- does each mux channel reach its panel?
 *     The PCA9548A control register is ONE byte; bit N set selects channel N.
 *     For channel 0/1/2 we write exactly (1 << N), probe the SSD1306 at 0x3C
 *     behind that channel, then write 0x00 to disconnect everything again.
 *     All three panels share the factory address 0x3C, so only the mux
 *     channel tells them apart -- this is the step that proves the
 *     channel-to-panel mapping.
 *
 * The one rule this tool exists to get right:
 *     Wire.end() must be called before EVERY Wire.begin().
 *     In this Arduino core (Wire.cpp around lines 300-303) a second
 *     Wire.begin() on the same bus is a SILENT NO-OP that never re-initialises
 *     the pins: it only logs "Bus already started in Master Mode." and returns
 *     without ever reaching initPins()/i2cInit(). An earlier probe version
 *     skipped Wire.end(), so its "swapped order" pass re-scanned the very same
 *     pins and every pass printed byte-identical output -- a fake result that
 *     looked like a real measurement. Every bus re-initialisation below
 *     therefore does Wire.end() -> Wire.begin(sda, scl, 400000) ->
 *     Wire.setTimeOut(50), and the tool additionally CROSS-CHECKS the two
 *     orders against each other: two orders that produce identical results is
 *     the fingerprint of that bug, and it is reported as a warning instead of
 *     being silently accepted as a measurement.
 *
 * Serial output is ASCII only, on purpose: a bench fault is often diagnosed
 * from the serial log alone, and UTF-8 text is unreadable in some serial
 * tools. Every logical line is flushed so a long report is not lost when the
 * monitor is closed early.
 *
 * Nothing here ships. This project exists only to produce facts.
 */
#include <Arduino.h>
#include <Wire.h>

namespace {

// --- bench wiring, treated as ground truth --------------------------------
// Operator report: "g4 connects scl; g5 connects sda" -- the PCA9548A
// module's SCL lands on GPIO4 and its SDA lands on GPIO5.
constexpr uint8_t kSdaOrderA = 5;  // order A: matches the bench
constexpr uint8_t kSclOrderA = 4;
constexpr uint8_t kSdaOrderB = 4;  // order B: the crossed order
constexpr uint8_t kSclOrderB = 5;

constexpr uint8_t kMuxAddr = 0x70;   // PCA9548A, A0/A1/A2 tied to GND
constexpr uint8_t kOledAddr = 0x3C;  // SSD1306 factory address (all panels)

constexpr uint32_t kBusHz = 400000;
constexpr uint16_t kTimeoutMs = 50;

constexpr uint8_t kScanFirst = 0x08;  // first 7-bit address worth scanning
constexpr uint8_t kScanLast = 0x77;   // last 7-bit address worth scanning
constexpr uint16_t kScanCount = kScanLast - kScanFirst + 1;  // 112 addresses

constexpr uint8_t kChannelCount = 3;     // channels 0/1/2 carry a panel
constexpr uint8_t kHeartbeatEvery = 16;  // heartbeat period, in addresses

// Error codes returned by Wire.endTransmission().
// 0 = ACK, 2 = address NACK, 3 = data NACK, 4 = other error, 5 = timeout.
struct Histogram {
  uint16_t ack = 0;
  uint16_t nackAddr = 0;
  uint16_t nackData = 0;
  uint16_t other = 0;
  uint16_t timeout = 0;
};

struct ScanResult {
  Histogram hist;
  uint8_t muxErr = 0;   // endTransmission() result for 0x70
  uint8_t oledErr = 0;  // endTransmission() result for 0x3C
  bool muxAcked = false;
  bool busInitOk = true;
};

void printHex2(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(static_cast<unsigned>(value), HEX);
}

// Short ASCII name for an endTransmission() code. Kept in one place so the
// per-address lines, the key-address lines and the Phase B lines all speak
// the same language.
const char* errName(uint8_t err) {
  switch (err) {
    case 0: return "ACK";
    case 2: return "nack_addr";
    case 3: return "nack_data";
    case 4: return "other";
    case 5: return "TIMEOUT";
    default: return "unknown";
  }
}

bool sameHistogram(const Histogram& a, const Histogram& b) {
  return a.ack == b.ack && a.nackAddr == b.nackAddr && a.nackData == b.nackData &&
         a.other == b.other && a.timeout == b.timeout;
}

void probeAddress(uint8_t addr, Histogram& h) {
  Serial.print("  0x");
  printHex2(addr);
  Serial.print(" -> ");

  Wire.beginTransmission(addr);
  const uint8_t err = Wire.endTransmission();
  switch (err) {
    case 0: Serial.print("ACK");         ++h.ack;       break;
    case 2: Serial.print("2 nack_addr"); ++h.nackAddr;  break;
    case 3: Serial.print("3 nack_data"); ++h.nackData;  break;
    case 4: Serial.print("4 other");     ++h.other;     break;
    case 5: Serial.print("5 TIMEOUT");   ++h.timeout;   break;
    default: Serial.print("unknown");    ++h.other;     break;
  }
  Serial.println();
  Serial.flush();
}

// Idle level of the two lines, read as plain inputs with NO internal
// pull-up. Because the internal pull-up is off, a HIGH reading can only come
// from an external pull-up (the PCA9548A module carries its own 10k pull-ups)
// while a LOW means nothing is holding the line up. Reading this BEFORE
// Wire.begin() matters: Wire.begin() hands the pins to the I2C peripheral and
// turns the ESP32's own internal pull-ups on, which would mask exactly the
// difference we want to observe.
//
// pinMode(pin, INPUT) explicitly disables both the internal pull-up and the
// internal pull-down, so a HIGH here really means "something outside the
// ESP32 is pulling this line up".
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

// Bring the bus up on a given pin pair and report whether the re-initialise
// actually took effect.
//
// The order inside this function is the whole point of the tool:
//   1. Wire.end()  -- releases the bus. Without it the next Wire.begin() is a
//      silent no-op (Wire.cpp:300-303) that never re-initialises the pins.
//      Doing it FIRST also matters for the idle reading: the running I2C
//      peripheral has the ESP32's internal pull-ups switched on
//      (esp32-hal-i2c.c sets scl_pullup_en/sda_pullup_en = GPIO_PULLUP_ENABLE),
//      so a bus that is still up would report HIGH even with no external
//      pull-up at all.
//   2. reportIdle() -- the pins are plain GPIO again and the internal
//      pull-ups are off, so the idle level tells us whether an EXTERNAL
//      pull-up (the PCA9548A module carries its own 10k) is present. This
//      must happen BEFORE Wire.begin() for the reason above.
//   3. Wire.begin() -- now the pins really are re-assigned to the I2C
//      peripheral on the requested pair.
//
// The return values are printed because a failed Wire.end() would make step 3
// a no-op: the scan would then re-use the previous pin pair and the whole
// pass would be a fake measurement that still looks like a legitimate
// "nothing answered" result.
bool initBus(uint8_t sda, uint8_t scl) {
  // Wire.end() FIRST, ALWAYS (Wire.cpp:300-303 makes begin() a no-op without
  // it).
  const bool ended = Wire.end();

  reportIdle(sda, scl);

  const bool started = Wire.begin(sda, scl, kBusHz);
  Wire.setTimeOut(kTimeoutMs);

  Serial.print("  bus init: Wire.end()=");
  Serial.print(ended ? "ok" : "FAILED");
  Serial.print("  Wire.begin()=");
  Serial.print(started ? "ok" : "FAILED");
  Serial.print("  clock=");
  Serial.print(static_cast<unsigned long>(Wire.getClock()));
  Serial.println(" Hz");

  if (!ended) {
    Serial.println("  WARNING: Wire.end() failed -- the pins may NOT have been re-assigned.");
    Serial.println("  WARNING: treat this pass as INVALID, it may have re-scanned the previous pins.");
  }
  Serial.flush();
  return ended && started;
}

// One full bus scan on the given pin pair.
ScanResult runScan(uint8_t sda, uint8_t scl, const char* label) {
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

  // The bus is torn down and brought back up on these exact pins, so the
  // idle reading is taken on a genuinely released pair.
  const bool ended = initBus(sda, scl);

  Histogram h;
  uint8_t sinceHeartbeat = 0;
  for (uint8_t addr = kScanFirst; addr <= kScanLast; ++addr) {
    probeAddress(addr, h);
    ++sinceHeartbeat;
    if (sinceHeartbeat >= kHeartbeatEvery) {
      sinceHeartbeat = 0;
      // Heartbeat: a wedged bus shows up as a gap in this line, and the
      // address it names tells us where the wedge happened.
      Serial.print("  ... reached 0x");
      printHex2(addr);
      Serial.println();
      Serial.flush();
    }
  }

  Serial.print("  summary: ack=");
  Serial.print(h.ack);
  Serial.print(" nack_addr=");
  Serial.print(h.nackAddr);
  Serial.print(" nack_data=");
  Serial.print(h.nackData);
  Serial.print(" other=");
  Serial.print(h.other);
  Serial.print(" timeout=");
  Serial.print(h.timeout);
  Serial.print(" (of ");
  Serial.print(static_cast<unsigned>(kScanCount));
  Serial.println(" addresses scanned)");
  Serial.flush();

  ScanResult result;
  result.hist = h;
  result.busInitOk = ended;

  // The two addresses this project actually cares about, printed with their
  // raw endTransmission() code so a reader never has to guess.
  Wire.beginTransmission(kMuxAddr);
  result.muxErr = Wire.endTransmission();
  Serial.print("  mux  0x70 -> endTransmission()=");
  Serial.print(static_cast<unsigned>(result.muxErr));
  Serial.print(" (");
  Serial.print(errName(result.muxErr));
  Serial.println(")");
  Serial.flush();

  Wire.beginTransmission(kOledAddr);
  result.oledErr = Wire.endTransmission();
  Serial.print("  oled 0x3C -> endTransmission()=");
  Serial.print(static_cast<unsigned>(result.oledErr));
  Serial.print(" (");
  Serial.print(errName(result.oledErr));
  Serial.println(")");
  Serial.flush();

  result.muxAcked = (result.muxErr == 0);
  if (result.muxAcked) {
    Serial.println("  verdict for this order: mux ACKED at 0x70");
  } else {
    Serial.println("  verdict for this order: mux did NOT answer at 0x70");
  }
  Serial.flush();

  return result;
}

// Phase B: per-channel mux probe on the order that ACKed 0x70.
void probeChannels(uint8_t sda, uint8_t scl) {
  Serial.println();
  Serial.println("=== Phase B: per-channel mux probe ===");
  Serial.print("bus: SDA=GPIO");
  Serial.print(static_cast<unsigned>(sda));
  Serial.print(" SCL=GPIO");
  Serial.print(static_cast<unsigned>(scl));
  Serial.println();
  Serial.flush();

  // Re-initialise the bus from scratch for this phase, same rule as always:
  // Wire.end() before Wire.begin().
  const bool busOk = initBus(sda, scl);
  if (!busOk) {
    Serial.println("  WARNING: the bus did not re-initialise cleanly -- Phase B results are NOT valid.");
    Serial.flush();
  }

  bool answered[kChannelCount] = {};

  for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
    // Select ONLY this channel: the control register is one byte and bit N
    // set means channel N. The write must be one complete transaction
    // (begin -> write -> endTransmission); the channel only takes effect
    // after the STOP.
    Wire.beginTransmission(kMuxAddr);
    Wire.write(static_cast<uint8_t>(1u << ch));
    const uint8_t writeErr = Wire.endTransmission();

    // Now ask the panel address on the bus. It can only answer if the mux
    // really let this one channel through.
    Wire.beginTransmission(kOledAddr);
    const uint8_t probeErr = Wire.endTransmission();

    // Disconnect all channels again so the next channel starts from a known
    // state instead of inheriting the previous selection.
    Wire.beginTransmission(kMuxAddr);
    Wire.write(static_cast<uint8_t>(0x00));
    const uint8_t offErr = Wire.endTransmission();

    answered[ch] = (probeErr == 0);

    Serial.print("  ch");
    Serial.print(static_cast<unsigned>(ch));
    Serial.print(": mux_write(0x");
    printHex2(static_cast<uint8_t>(1u << ch));
    Serial.print(")=");
    Serial.print(static_cast<unsigned>(writeErr));
    Serial.print("  probe_0x3C=");
    Serial.print(static_cast<unsigned>(probeErr));
    Serial.print(" (");
    Serial.print(errName(probeErr));
    Serial.print(")  deselect(0x00)=");
    Serial.print(static_cast<unsigned>(offErr));
    Serial.println(answered[ch] ? "  -> ANSWERED" : "  -> no answer");
    Serial.flush();
  }

  Serial.print("  VERDICT: channels that answered at 0x3C: ");
  bool any = false;
  for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
    if (answered[ch]) {
      if (any) {
        Serial.print(",");
      }
      Serial.print("ch");
      Serial.print(static_cast<unsigned>(ch));
      any = true;
    }
  }
  if (!any) {
    Serial.print("NONE");
  }
  Serial.println();
  Serial.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give the USB CDC port time to enumerate; without this the first lines of
  // the report are lost while the host is still opening the port.
  delay(500);

  Serial.println();
  Serial.println("=== pet-oled3 i2c-probe v1 (bus order + per-channel mux probe) ===");
  Serial.println("bench wiring: GPIO4 -> module SCL, GPIO5 -> module SDA");
  Serial.println("expected: PCA9548A mux at 0x70, SSD1306 panels at 0x3C on ch0/ch1/ch2");
  Serial.println("error codes: 0=ACK 2=nack_addr 3=nack_data 4=other 5=TIMEOUT");
  Serial.println("bus: 400000 Hz, per-transaction timeout 50 ms");
  Serial.flush();

  const ScanResult orderA =
      runScan(kSdaOrderA, kSclOrderA, "Phase A order A: SDA=GPIO5 SCL=GPIO4 (bench order)");
  const ScanResult orderB =
      runScan(kSdaOrderB, kSclOrderB, "Phase A order B: SDA=GPIO4 SCL=GPIO5 (crossed order)");

  // Cross-check: the old broken probe printed byte-identical output for
  // every pass because its second Wire.begin() was a no-op. Two orders that
  // produced exactly the same numbers on the SAME two pins are therefore
  // suspicious, and are called out instead of being silently trusted.
  Serial.println();
  Serial.println("=== Phase A cross-check ===");
  if (!orderA.busInitOk || !orderB.busInitOk) {
    Serial.println("WARNING: at least one pass did not re-initialise the bus cleanly.");
    Serial.println("WARNING: the affected pass is NOT a valid measurement.");
  } else if (sameHistogram(orderA.hist, orderB.hist) && orderA.muxErr == orderB.muxErr &&
             orderA.oledErr == orderB.oledErr) {
    Serial.println("WARNING: both orders produced IDENTICAL results on the same two pins.");
    Serial.println("WARNING: that is the fingerprint of a Wire.end() no-op -- the second");
    Serial.println("WARNING: begin() did not re-assign the pins, so pass B is NOT a real test.");
  } else {
    Serial.println("the two orders produced different results: the bus really was re-initialised.");
  }
  Serial.flush();

  // Phase B only makes sense on an order where the mux actually answered.
  if (orderA.muxAcked && orderB.muxAcked) {
    Serial.println();
    Serial.println("note: BOTH orders ACKed 0x70 -- using the bench order (A) for Phase B.");
    Serial.flush();
    probeChannels(kSdaOrderA, kSclOrderA);
  } else if (orderA.muxAcked) {
    Serial.println();
    Serial.println("verdict: order A (SDA=GPIO5 SCL=GPIO4, the bench order) is the working order.");
    Serial.flush();
    probeChannels(kSdaOrderA, kSclOrderA);
  } else if (orderB.muxAcked) {
    Serial.println();
    Serial.println("verdict: only order B (SDA=GPIO4 SCL=GPIO5) ACKed 0x70 -- the lines are CROSSED.");
    Serial.flush();
    probeChannels(kSdaOrderB, kSclOrderB);
  } else {
    Serial.println();
    Serial.println("VERDICT: the PCA9548A mux was NOT found at 0x70 in EITHER order -- Phase B skipped.");
    Serial.println("next step: check the physical layer (power, GND, wire order, pull-ups)");
    Serial.println("before believing any firmware result.");
    Serial.flush();
  }

  // Leave the bus idle. loop() must not generate traffic either: a diagnostic
  // that keeps talking on the bus would perturb what it is measuring.
  Wire.end();
  Serial.println();
  Serial.println("=== pet-oled3 i2c-probe v1 done; idling (no bus traffic) ===");
  Serial.flush();
}

void loop() {
  delay(1000);
}
