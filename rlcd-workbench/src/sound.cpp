// rlcd-workbench / 本地提示音实现（v0.1.5，docs/adr/0022）
//
// 为什么不走官方 codec_board：那一整套（字符串解析器 + 板型表 + tca9554 +
// 三处 IO 回调）是为了「一份代码适配十几种板子」。我们只有一种板子，
// 代价却是一个要编进固件的 txt、一套要调通的解析器、一个仓促的宏开关 ——
// 任何一环没对上，症状都是「不响且不知道为什么」。ADR-0022 决策 §6 选了精简版。
//
// 代价必须写明：这是不用官方验过的路径。拼错 if 层结构不会报错，只会无声。
// 所以 config.h 里留了 kSoundSelfTest 这个开机长鸣开关当 bringup 仪器。
//
// 这一层只回答「能不能响」，不回答「该不该响」—— 后者是 main.cpp 的事
// （ADR-0022 决策 §0 的判定树）。

#include "sound.h"

#include <Arduino.h>

#include <cstring>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"


#include "config.h"
#include "i2c.h"

namespace sound {

namespace {

constexpr const char *kTag = "SND";

const char *g_reason = "not initialized";
bool g_ready = false;
bool g_busy = false;

// 自己维护「开流了没有」，不靠 esp_codec_dev_open 的返回值判断：
// esp_codec_dev.c 里已 open 时打一行 "already open" 就 return OK，
// 成功但不按新参数重新配置。信返回值会得到「看起来开了其实没配」。
bool g_opened = false;

const audio_codec_data_if_t *g_data_if = nullptr;
const audio_codec_ctrl_if_t *g_ctrl_if = nullptr;
const audio_codec_gpio_if_t *g_gpio_if = nullptr;
const audio_codec_if_t *g_codec_if = nullptr;
esp_codec_dev_handle_t g_out = nullptr;
i2s_chan_handle_t g_tx = nullptr;

// open 之后到第一次写数据之间等多久。NS4150B 使能到稳定输出有建立时间，
// 官方 codec_board 的全套初始化本身耗了几百毫秒，这个延时被掩盖掉了；
// 我们精简版初始化快得多，不等一下可能第一声被吃掉或爆音（ADR-0022 §4 的姊妹决定）。
// 10 ms 相对 150 ms 的提示音可忽略。
constexpr uint32_t kPaSettleMs = 10;

// 播放一次用的采样格式。16 kHz / 双声道 / 16 bit：
// 单声道会被 _i2s_data_set_fmt 自动扩成双声道（它内部 memcpy 一份 eq_fs 再改
// channel=2 加 channel_mask），所以这里直接要双声道，省一次内部改写。
constexpr uint32_t kSampleRate = cfg::kChimeSampleRate;
constexpr uint8_t kChannel = 2;
constexpr uint8_t kBits = 16;

// 写块大小。官方示例 256 B；16 kHz 双声道 16 bit 下 256 B 只有 4 ms，
// 写调用太密。512 B = 8 ms，够平滑又不占太多栈。
constexpr size_t kChunkBytes = 512;

void setReason(const char *why) { g_reason = why; }

// 打开输出流并等功放稳定。成功后 g_opened = true。
bool openStream() {
  if (g_opened) {
    return true;
  }
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = kSampleRate;
  fs.channel = kChannel;
  fs.bits_per_sample = kBits;
  fs.channel_mask = 0;  // 0 = 驱动自己填满所有声道

  const int ret = esp_codec_dev_open(g_out, &fs);
  if (ret != ESP_CODEC_DEV_OK) {
    setReason("open failed");
    return false;
  }
  g_opened = true;

  // 音量：**必须在 open 之后设，且每次都设**。
  //
  // 上板排查时的确凿结论。es8311.c:243 的 suspend() 会写
  // ES8311_DAC_REG32 = 0x00，而每次 esp_codec_dev_open() 都先 suspend 再 start，
  // start 却不恢复这个寄存器 —— 结果 DAC 音量停在 0x00（约 -95.5 dB），
  // 写进去的 PCM 全被衰减成听不见，而 esp_codec_dev_write() 照样返回成功。
  // 所以「写入无报错」不等于「有声音」，日志层面的 ok 是假象。
  //
  // es8311_set_vol() 还要求 codec->is_open == true（es8311.c:353），
  // 放在 open 之前会直接返回 WRONG_STATE。
  //
  // 取档位而不是常量（docs/adr/0023）：每档的 DAC 值由 kVolumeLevels 决定，
  // 用户改了档位下次出声就生效 —— 因为这里每次都重新读一遍。
  esp_codec_dev_set_out_vol(g_out, cfg::kVolumeLevels[cfg_g_sound_volume()]);

  // 重新使能 I2S 通道 —— **这不是多余的一步，是 v0.1.5 最隐蔽的一个坑**。
  //
  // audio_codec_data_i2s.c 在只有 TX 的配置下，_i2s_data_enable() 第 434-441
  // 行先无条件 `_i2s_drv_enable(..., false)` 把通道关掉，然后走
  // check_fs_compatible() 第 271-278 行的分支：那里只 `set_fs()` 重配格式就
  // `return` 了，**没有任何地方再把通道打开**。于是 open 之后通道处于关闭态，
  // write() 照样返回成功（数据被塞进没启动的 DMA），耳朵什么也听不到。
  //
  // 症状：**第一声永远听得见，之后全哑**。因为通道在 init_std_mode() 之后初始
  // 就是使能状态，第一次 open 的 disable 恰好把它关掉；如果随后 set_fs 只是
  // 重配时分频而不碰使能位，硬件仍在跑 —— 于是第一声出得来。从第二次 open 起
  // 通道真的断了，就再也出不来。
  //
  // 那条 `E (xxx) i2s_common: i2s_channel_disable(1262): the channel has not
  // been enabled yet` 就是这个 bug 的心跳：它每次 open 都打一遍，说明每次都在
  // 关一个本来就关着的通道。早先把它当「无害副作用」放过，代价是白烧两版固件。
  //
  // 修法：自己 enable。重复 enable 由 IDF 返回 ESP_ERR_INVALID_STATE 吃掉，
  // 不影响正常路径。
  i2s_channel_enable(g_tx);

  // 等功放建立。这一步不能省 —— 见 kPaSettleMs 的注释。
  delay(kPaSettleMs);
  return true;
}

// 关流。esp_codec_dev 的 disable_when_closed 默认 true，
// close() 会调 es8311_enable(false) -> es8311_pa_power(ES_PA_DISABLE)，
// 也就是功放由驱动顺手关掉。**不要自己碰 GPIO46**，否则和驱动抢同一根脚。
//
// close 之前必须等数据真的放完（kChimeFlushMs）。理由见 config.h 里
// kChimeToneMs 那段注释：write() 返回成功只代表数据进了 I2S 的 DMA 队列，
// 不代表已经变成声音出了喇叭。功放一断，队列里没放完的部分就永远听不见了。
void closeStream() {
  if (!g_opened) {
    return;
  }
  delay(cfg::kChimeFlushMs);
  esp_codec_dev_close(g_out);
  g_opened = false;
}

// 生成并推送 ms 毫秒的方波。
//
// 为什么用方波不用正弦：这个喇叭（8 Ω 2 W，小腔体）在 2.7 kHz 上基频就够响，
// 正弦要多算一次 sinf，而方波在 1bpp 的「够响」目标下没有可听差别。
// 幅度取 60% 满量程，留余量防削顶 —— 削顶的爆音比小声更糟。
void pushSquare(uint16_t hz, uint32_t ms) {
  const uint32_t totalFrames = (kSampleRate * ms) / 1000;
  const uint32_t halfPeriod = kSampleRate / (hz * 2);
  const int16_t high = (int16_t)((32767L * cfg::kChimeAmplitude) / 100);
  const int16_t low = (int16_t)-high;

  uint8_t buf[kChunkBytes];
  auto *frames = reinterpret_cast<int16_t *>(buf);
  const size_t framesPerChunk = kChunkBytes / (sizeof(int16_t) * kChannel);

  uint32_t produced = 0;
  uint32_t phase = 0;
  while (produced < totalFrames) {
    size_t n = framesPerChunk;
    if (totalFrames - produced < n) {
      n = totalFrames - produced;
    }
    for (size_t i = 0; i < n; ++i) {
      const int16_t v = (phase < halfPeriod) ? high : low;
      // 双声道同一份数据：左右都给，避免有的 codec 只取某一路。
      frames[i * kChannel + 0] = v;
      frames[i * kChannel + 1] = v;
      if (++phase >= halfPeriod * 2) {
        phase = 0;
      }
    }
    esp_codec_dev_write(g_out, buf, (int)(n * sizeof(int16_t) * kChannel));
    produced += n;
  }
}

// 推一段静音。为什么需要：两声之间那段间隔如果只是「不写」，
// I2S 会继续送上一次留在 DMA 里的数据，听感上两声是连着的。
void pushSilence(uint32_t ms) {
  const uint32_t totalFrames = (kSampleRate * ms) / 1000;
  uint8_t buf[kChunkBytes];
  memset(buf, 0, sizeof(buf));
  const size_t framesPerChunk = kChunkBytes / (sizeof(int16_t) * kChannel);
  uint32_t produced = 0;
  while (produced < totalFrames) {
    size_t n = framesPerChunk;
    if (totalFrames - produced < n) {
      n = totalFrames - produced;
    }
    esp_codec_dev_write(g_out, buf, (int)(n * sizeof(int16_t) * kChannel));
    produced += n;
  }
}

}  // namespace

bool ready() { return g_ready; }

const char *reason() { return g_reason; }

bool begin() {
  if (g_ready) {
    return true;
  }
  g_reason = "starting";

  // ---- 1. I2S 发送通道 ----
  // 注意：这一层必须自己建通道。esp_codec_dev 的 _i2s_data_open 只记录
  // tx_handle/rx_handle 指针，不建通道（读过它的实现确认过）。
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.auto_clear = true;  // 欠载时自动填 0，而不是重复上次数据
  esp_err_t err = i2s_new_channel(&chanCfg, &g_tx, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "i2s_new_channel failed: %d", (int)err);
    setReason("i2s channel");
    return false;
  }

  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg.sample_rate_hz = kSampleRate;
  stdCfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = (gpio_num_t)cfg::kI2sMclk;
  stdCfg.gpio_cfg.bclk = (gpio_num_t)cfg::kI2sBclk;
  stdCfg.gpio_cfg.ws = (gpio_num_t)cfg::kI2sWs;
  stdCfg.gpio_cfg.dout = (gpio_num_t)cfg::kI2sDout;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;  // 只播放，不接录音方向
  err = i2s_channel_init_std_mode(g_tx, &stdCfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "i2s init std failed: %d", (int)err);
    setReason("i2s std");
    return false;
  }

  // ---- 2. codec 三层接口 ----
  // I2C：用官方的 audio_codec_ctrl_i2c.c，把项目现有那条总线的句柄交给它。
  //
  // 这是第二版实现。第一版按「本项目用 Arduino TwoWire」的假设自己写了一份
  // ctrl_if 走 Wire，上板后每次写都返回 err=4（总线状态机异常）—— 因为
  // src/i2c.cpp 用的是 IDF 的 i2c_new_master_bus()，Wire 从未被 begin() 过，
  // 那份实现调的是一个空壳。i2c.h:6-9 早就写明「用 IDF 的 i2c_master 而不是
  // Arduino 的 Wire」，是我没读到就下了结论，白烧两版。
  //
  // 官方实现要的正是 i2c_master_bus_handle_t，与本项目驱动同源，天然对得上。
  if (!i2cbus::ready()) {
    ESP_LOGE(kTag, "i2c bus not ready");
    setReason("i2c bus");
    return false;
  }
  audio_codec_i2c_cfg_t i2cCfg = {};
  i2cCfg.port = 0;
  // 官方约定：这里是 8 位地址，内部做 (addr >> 1) 取 7 位再挂设备。
  // 所以必须 << 1 —— 直接传 0x18 会被它当成 0x0C，去找一个不存在的从机。
  i2cCfg.addr = (uint8_t)(cfg::kAddrEs8311 << 1);
  i2cCfg.bus_handle = i2cbus::busHandle();
  g_ctrl_if = audio_codec_new_i2c_ctrl(&i2cCfg);
  if (g_ctrl_if == nullptr) {
    ESP_LOGE(kTag, "ctrl_if failed");
    setReason("ctrl_if");
    return false;
  }

  // I2S data：把上面建好的 tx handle 塞给它。
  audio_codec_i2s_cfg_t i2sCfg = {};
  i2sCfg.port = I2S_NUM_0;
  i2sCfg.tx_handle = g_tx;
  i2sCfg.rx_handle = nullptr;
  g_data_if = audio_codec_new_i2s_data(&i2sCfg);
  if (g_data_if == nullptr) {
    ESP_LOGE(kTag, "data_if failed");
    setReason("data_if");
    return false;
  }

  // GPIO：官方 es8311_pa_power 靠它开关功放脚。
  // **必须给** —— pa_pin != -1 但 gpio_if == NULL 时它会直接 return，
  // 功放永远不使能，症状就是彻底的无声。
  g_gpio_if = audio_codec_new_gpio();
  if (g_gpio_if == nullptr) {
    ESP_LOGE(kTag, "gpio_if failed");
    setReason("gpio_if");
    return false;
  }

  // ---- 3. ES8311 ----
  // 这一段是**严格照抄官方 codec_init.c:414-423** 的字段集，不多设一个。
  // 教训：第一版我自己「补全」了 pa_reverted / master_mode / digital_mic /
  // invert_mclk / invert_sclk / no_dac_ref / mclk_div 七个字段，还漏了 hw_gain。
  // 结果是 codec 初始化成功、I2C 全通、功放也真的拉高了，但就是没声音 ——
  // 典型症状是「一切正常除了不响」，最难查。
  //
  // 其中 mclk_div=256 与 no_dac_ref=true 是两个最可疑的：官方一个都没设，
  // 强制改它们会让内部时钟分频和 DAC 参考走岔，数据送出去了却解不出来。
  // 官方只在 board_cfg.txt 里给出 pa_gain: 6，落到 hw_gain.pa_gain。
  es8311_codec_cfg_t codecCfg = {};
  codecCfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;  // 本板 out/in 不是同一颗 codec
  codecCfg.ctrl_if = g_ctrl_if;
  codecCfg.gpio_if = g_gpio_if;
  codecCfg.pa_pin = (int16_t)cfg::kPaCtrl;
  codecCfg.use_mclk = true;   // 官方 use_mclk: 1，本板 MCLK 是实线 GPIO16
  codecCfg.hw_gain.pa_gain = 6.0f;  // 官方 pa_gain: 6
  g_codec_if = es8311_codec_new(&codecCfg);
  if (g_codec_if == nullptr) {
    ESP_LOGE(kTag, "es8311_codec_new failed");
    setReason("es8311");
    return false;
  }

  esp_codec_dev_cfg_t devCfg = {};
  devCfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  devCfg.codec_if = g_codec_if;
  devCfg.data_if = g_data_if;
  g_out = esp_codec_dev_new(&devCfg);
  if (g_out == nullptr) {
    ESP_LOGE(kTag, "esp_codec_dev_new failed");
    setReason("dev_new");
    return false;
  }

  g_ready = true;
  g_reason = "ok";
  ESP_LOGI(kTag, "ES8311 ready (%u Hz, %u ch, %u bit)", (unsigned)kSampleRate,
           (unsigned)kChannel, (unsigned)kBits);

  // ---- 4. 可选自检 ----
  // 修 bug 用的一次性开关，正常版本为 false（config.h）。
  // m01088 那个「短音被吃掉」的问题就是用这个开关定位的：
  // 先让它连响三声 300 ms 长音（全响），再连响五声 150 ms 提示音（只第一下响），
  // 两组行为的唯一差别就是音频长度 —— 长音覆盖了开流关流的固定开销。
  if (cfg::kSoundSelfTest) {
    playTone(1000, cfg::kSoundSelfTestMs);
  }
  return true;
}

bool playTone(uint16_t hz, uint32_t ms) {
  if (!g_ready || g_busy) {
    return false;
  }
  if (hz == 0 || ms == 0) {
    return false;
  }
  g_busy = true;
  bool ok = false;
  do {
    if (!openStream()) {
      break;
    }
    pushSquare(hz, ms);
    // 尾部补一点静音，再关流。不留这一下的话，关流那一刻 DMA 里还有
    // 半截波形，功放断电时会把它变成一声「咔」。
    pushSilence(cfg::kChimeFlushMs);
    ok = true;
  } while (false);
  closeStream();
  g_busy = false;
  return ok;
}

bool playChime() {
  if (!g_ready || g_busy) {
    return false;
  }
  g_busy = true;
  bool ok = false;
  do {
    if (!openStream()) {
      break;
    }
    // 两声「嘀嘀」：150 ms 响 + 40 ms 静 + 150 ms 响 = 340 ms。
    // 长度为什么从 60 ms 改成 150 ms，见 config.h 里 kChimeToneMs 的教训。
    pushSquare(cfg::kChimeHz, cfg::kChimeToneMs);
    pushSilence(cfg::kChimeGapMs);
    pushSquare(cfg::kChimeHz, cfg::kChimeToneMs);
    pushSilence(cfg::kChimeFlushMs);  // 见 playTone 里同一段注释
    ok = true;
  } while (false);
  closeStream();
  g_busy = false;
  return ok;
}

}  // namespace sound
