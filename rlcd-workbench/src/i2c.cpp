#include "i2c.h"

#include <Arduino.h>

#include "config.h"

namespace i2cbus {
namespace {

i2c_master_bus_handle_t g_bus = nullptr;
int g_lastError = 0;

constexpr TickType_t kDataTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kDoneTicks = pdMS_TO_TICKS(1000);

// 每次传输前排空上一次的挂起操作。官方 i2c_bsp 也是这么做的：新一代驱动允许
// 异步排队，不排空就拿新数据去覆盖还在途的传输，读回来的会是上一笔。
void waitDone() {
  if (g_bus != nullptr) {
    i2c_master_bus_wait_all_done(g_bus, kDoneTicks);
  }
}

}  // namespace

bool begin(int sda, int scl) {
  if (g_bus != nullptr) {
    return true;
  }

  i2c_master_bus_config_t conf = {};
  conf.clk_source = I2C_CLK_SRC_DEFAULT;
  conf.i2c_port = I2C_NUM_0;
  conf.scl_io_num = static_cast<gpio_num_t>(scl);
  conf.sda_io_num = static_cast<gpio_num_t>(sda);
  // 板上已有 5.1K 外部上拉（R20/R22），内部上拉只是兜底，不会把边沿拖慢。
  conf.flags.enable_internal_pullup = true;
  conf.glitch_ignore_cnt = 7;

  const esp_err_t err = i2c_new_master_bus(&conf, &g_bus);
  g_lastError = static_cast<int>(err);
  if (err != ESP_OK) {
    g_bus = nullptr;
    return false;
  }
  return true;
}

bool ready() { return g_bus != nullptr; }

int lastError() { return g_lastError; }

bool addDevice(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *out) {
  if (g_bus == nullptr || out == nullptr) {
    return false;
  }

  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address = addr;
  dev.scl_speed_hz = hz;

  const esp_err_t err = i2c_master_bus_add_device(g_bus, &dev, out);
  g_lastError = static_cast<int>(err);
  if (err != ESP_OK) {
    *out = nullptr;
    return false;
  }
  return true;
}

void removeDevice(i2c_master_dev_handle_t dev) {
  if (g_bus != nullptr && dev != nullptr) {
    i2c_master_bus_rm_device(dev);
  }
}

int write(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len) {
  if (dev == nullptr || buf == nullptr) {
    return static_cast<int>(ESP_ERR_INVALID_ARG);
  }
  waitDone();
  const esp_err_t err = i2c_master_transmit(dev, buf, len, kDataTicks);
  g_lastError = static_cast<int>(err);
  return g_lastError;
}

int read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len) {
  if (dev == nullptr || buf == nullptr) {
    return static_cast<int>(ESP_ERR_INVALID_ARG);
  }
  waitDone();
  const esp_err_t err = i2c_master_receive(dev, buf, len, kDataTicks);
  g_lastError = static_cast<int>(err);
  return g_lastError;
}

int writeRead(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wrLen, uint8_t *rd,
              size_t rdLen) {
  if (dev == nullptr || wr == nullptr || rd == nullptr) {
    return static_cast<int>(ESP_ERR_INVALID_ARG);
  }
  waitDone();
  const esp_err_t err = i2c_master_transmit_receive(dev, wr, wrLen, rd, rdLen, kDataTicks);
  g_lastError = static_cast<int>(err);
  return g_lastError;
}

void scan() {
  if (g_bus == nullptr) {
    return;
  }
  int found = 0;
  Serial.println("[i2c] scan 0x08..0x77");
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    // i2c_master_probe 只做一次地址探测，不建永久设备句柄。
    if (i2c_master_probe(g_bus, addr, 50) == ESP_OK) {
      Serial.printf("[i2c]   found 0x%02X\n", (unsigned)addr);
      ++found;
    }
  }
  Serial.printf("[i2c] scan done, %d device(s)\n", found);
}

}  // namespace i2cbus
