#include "battery.h"

#include <Arduino.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>

#include "config.h"

namespace battery {
namespace {

adc_oneshot_unit_handle_t g_unit = nullptr;
adc_cali_handle_t g_cali = nullptr;
bool g_ok = false;

// 平均几次。16 次在 400 kHz 下的总耗时不到 1 ms，代价可以忽略，
// 但能把纹波压掉大半。
constexpr int kAverages = 16;

}  // namespace

bool begin() {
  if (g_ok) {
    return true;
  }

  adc_cali_curve_fitting_config_t cali = {};
  cali.unit_id = ADC_UNIT_1;
  cali.atten = ADC_ATTEN_DB_12;
  cali.bitwidth = ADC_BITWIDTH_12;
  if (adc_cali_create_scheme_curve_fitting(&cali, &g_cali) != ESP_OK) {
    // 校准不可用时不做静默降级 —— 没校准的毫伏值系统性偏低约 10 %，
    // 电池百分比会一路显示成「快没电了」，比读不到更误导人。
    g_cali = nullptr;
    return false;
  }

  adc_oneshot_unit_init_cfg_t uinit = {};
  uinit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&uinit, &g_unit) != ESP_OK) {
    return false;
  }

  adc_oneshot_chan_cfg_t chan = {};
  chan.bitwidth = ADC_BITWIDTH_12;
  chan.atten = ADC_ATTEN_DB_12;
  if (adc_oneshot_config_channel(g_unit, ADC_CHANNEL_3, &chan) != ESP_OK) {
    return false;
  }

  g_ok = true;
  return true;
}

bool ready() { return g_ok; }

int percentFromVolts(float v) {
  constexpr int last = cfg::kSocKnotCount - 1;
  // 两端饱和：低于最低锚点一律 0、高于最高锚点一律 100。
  // 「端点不撒谎」是这条曲线的验收条件之一 —— 0% 和 100% 只该出现在真的
  // 到了那两头的时候，不能因为表算出来是 -3% 或 104% 才被迫夹。
  if (v <= cfg::kSocKnotV[0]) {
    return cfg::kSocKnotPct[0];
  }
  if (v >= cfg::kSocKnotV[last]) {
    return cfg::kSocKnotPct[last];
  }
  for (int i = 1; i <= last; ++i) {
    if (v <= cfg::kSocKnotV[i]) {
      const float v0 = cfg::kSocKnotV[i - 1];
      const float span = cfg::kSocKnotV[i] - v0;
      const int p0 = cfg::kSocKnotPct[i - 1];
      const int dp = cfg::kSocKnotPct[i] - p0;
      const float t = (v - v0) / span;
      // 截断而不是四舍五入：让「显示值 <= 真实估算值」，宁可少报一点也不多报。
      return p0 + static_cast<int>(t * static_cast<float>(dp));
    }
  }
  return cfg::kSocKnotPct[last];
}

bool read(Sample *out) {
  if (!g_ok || out == nullptr) {
    return false;
  }
  *out = Sample{};

  int32_t sum = 0;
  int good = 0;
  for (int i = 0; i < kAverages; ++i) {
    int raw = 0;
    if (adc_oneshot_read(g_unit, ADC_CHANNEL_3, &raw) == ESP_OK) {
      sum += raw;
      ++good;
    }
  }
  if (good == 0) {
    return false;
  }

  out->raw = static_cast<int>(sum / good);
  if (adc_cali_raw_to_voltage(g_cali, out->raw, &out->pinMv) != ESP_OK) {
    return false;
  }

  out->packV = 0.001f * static_cast<float>(out->pinMv) * cfg::kBatteryDivider;
  out->pct = percentFromVolts(out->packV);
  out->ok = true;
  return true;
}

}  // namespace battery
