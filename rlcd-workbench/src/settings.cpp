// rlcd-workbench / 设置注册表实现（v0.1.1，docs/adr/0017）
//
// 当前只有一项：对比度。档位表与默认档都在 src/config.h —— 屏上、串口、
// 面板热键、设置版面四个入口必须说同一个数，所以档位表只写一份。
//
// 这一层刻意不认识 u8g2、不认 main.cpp 的全局量：生效动作由 config.h 声明的
// g_lcd_setContrast() 钩子转发（main.cpp 在 setup() 里挂上），否则设置模块
// 反向依赖显示层，编译单元之间会缠成一个环。

#include "settings.h"

#include <Preferences.h>

#include <cstdio>
#include <cstring>

#include "config.h"

// cfg_g_lcd_setContrast 的定义（extern 声明在 config.h）。
//
// 为什么不在 config.h 里定义：放头文件里会让每个 include config.h 的
// 翻译单元各拿一份，链接时报 multiple definition（M4 实测撞过一次）。
// 为什么不在匿名 namespace 里：main.cpp 要写它，必须是全局具名符号。
// 所以放在这一层 —— settings 之外、全局之内。副作用：任何 TU 都能改它，
// 因此接钩子的时机只在 main.cpp 的 setup() 里，之后不再有人碰。
void (*cfg_g_lcd_setContrast)(uint8_t) = nullptr;

// 声音三项的状态（v0.1.5，docs/adr/0022 决策 §4）。
// settings.cpp 只记不响 —— 见 config.h 里这一组钩子的注释。
// 默认值必须与 kTable 里的 defaultIndex 一致：loadAll() 会把 apply 全跑一遍，
// 那时会覆盖掉这里的初值，但在 loadAll() 之前就有人来读的话要有合理答案。
bool g_soundMaster = true;
bool g_soundChime = true;
bool g_soundQuiet = true;
// 音量档位（v0.1.5 后续，docs/adr/0023）。初值就是默认档 —— sound.cpp 在
// loadAll() 之前若被 open 一次，按 85 出声，而不是按数组第 0 档（40）出。
uint8_t g_soundVolume = cfg::kVolumeDefaultIdx;

void cfg_g_sound_setMaster(bool on) { g_soundMaster = on; }
void cfg_g_sound_setChime(bool on) { g_soundChime = on; }
void cfg_g_sound_setQuiet(bool on) { g_soundQuiet = on; }
void cfg_g_sound_setVolume(uint8_t idx) {
  g_soundVolume = (idx < cfg::kVolumeCount) ? idx : cfg::kVolumeDefaultIdx;
}
bool cfg_g_sound_master() { return g_soundMaster; }
bool cfg_g_sound_chime() { return g_soundChime; }
bool cfg_g_sound_quiet() { return g_soundQuiet; }
uint8_t cfg_g_sound_volume() {
  return (g_soundVolume < cfg::kVolumeCount) ? g_soundVolume : cfg::kVolumeDefaultIdx;
}

namespace settings {

namespace {

// ---- 对比度（当前唯一一项）----

void applyContrast(uint8_t idx) {
  if (idx >= cfg::kContrastCount) {
    idx = cfg::kContrastDefaultIdx;
  }
  if (cfg_g_lcd_setContrast != nullptr) {
    cfg_g_lcd_setContrast(cfg::kContrastLevels[idx]);
  }
}

void formatContrast(uint8_t idx, char *buf, size_t cap) {
  if (idx >= cfg::kContrastCount) {
    idx = cfg::kContrastDefaultIdx;
  }
  snprintf(buf, cap, "%u/%u (%u)", (unsigned)(idx + 1), (unsigned)cfg::kContrastCount,
           (unsigned)cfg::kContrastLevels[idx]);
}

// ---- 声音三项（v0.1.5，docs/adr/0022）----
//
// 三项都是二态（开/关），apply 一律只记状态、不出声：
// 出声由 main.cpp 看到 cfg_g_sound_changed 标志再驱动 sound::playChime()
// （ADR-0022 决策 §4）。这样 loadAll() 逐项 apply 时不会上电就叫。
//
// 为什么 apply 不直接调 sound::begin()：注册表这一层刻意不认识显示层，
// 也不该认识音频层（settings.cpp 头注释里的同一条理由）。

void applyMaster(uint8_t idx) { cfg_g_sound_setMaster(idx == cfg::kSwitchOn); }
void applyChime(uint8_t idx) { cfg_g_sound_setChime(idx == cfg::kSwitchOn); }
void applyQuiet(uint8_t idx) { cfg_g_sound_setQuiet(idx == cfg::kSwitchOn); }

// 音量（v0.1.5 后续，docs/adr/0023）：五档，apply 只记档位。
// 真正写进 DAC 的时机在 sound.cpp 的 openStream() —— 那里每次开流后都要写一遍，
// 因为 es8311 的 suspend() 会把 DAC_REG32 清成静音值（见 config.h 的注释）。
// 所以这里改档位不需要「立即推给 codec」，下一次出声自然带上新档位。
void applyVolume(uint8_t idx) { cfg_g_sound_setVolume(idx); }

// 与 formatContrast 同构：档位/总档 + 档位表里的实际值。
// 两行数字落在同一个 x 位置，一眼能对比（ui.cpp 右对齐的用意）。
void formatVolume(uint8_t idx, char *buf, size_t cap) {
  if (idx >= cfg::kVolumeCount) {
    idx = cfg::kVolumeDefaultIdx;
  }
  snprintf(buf, cap, "%u/%u (%u)", (unsigned)(idx + 1), (unsigned)cfg::kVolumeCount,
           (unsigned)cfg::kVolumeLevels[idx]);
}

// 三行共用一套「开/关」文案。夜间静音的「开」指「开启夜间静音」，
// 与另两项方向相反，但标签本身是完整主语，屏上读得通（v0.1.5 已就此确认）。
void formatSwitch(uint8_t idx, char *buf, size_t cap) {
  snprintf(buf, cap, "%s", (idx == cfg::kSwitchOn) ? "\xE5\xBC\x80" : "\xE5\x85\xB3");
}

// label 用 UTF-8 转义写，不写中文原文：源文件编码在别处被转换过之后，
// 直接写中文会和字库对不上（fontSelfTest 里星期表头同一套做法）。
//
// 顺序有意义：对比度在最上（v0.1.1 起就在这），声音总闸紧跟其后，
// 音量紧挨总闸（它是总闸的量纲，分开放在列表两头说不通），两个子项在最后。
// 「音量」是 v0.1.5 后续加的第 5 项（docs/adr/0023）—— 它撞上版面上限，
// 所以 drawSettings() 同时改成了固定窗口滚动 + 标题右侧位置指示。
const Item kTable[] = {
    {"\xE5\xAF\xB9\xE6\xAF\x94\xE5\xBA\xA6", cfg::kContrastCount, cfg::kContrastDefaultIdx,
     "contrast", applyContrast, formatContrast},
    {"\xE5\xA3\xB0\xE9\x9F\xB3", cfg::kSoundCount, cfg::kSwitchOn, "sound", applyMaster,
     formatSwitch},
    {"\xE9\x9F\xB3\xE9\x87\x8F", cfg::kVolumeCount, cfg::kVolumeDefaultIdx, "volume", applyVolume,
     formatVolume},
    {"\xE6\x95\xB4\xE7\x82\xB9\xE6\x8A\xA5\xE6\x97\xB6", cfg::kSoundCount, cfg::kSwitchOn, "chime",
     applyChime, formatSwitch},
    {"\xE5\xA4\x9C\xE9\x97\xB4\xE9\x9D\x99\xE9\x9F\xB3", cfg::kSoundCount, cfg::kSwitchOn, "quiet",
     applyQuiet, formatSwitch},
};

constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

State g_state[kCount];
Src g_src[kCount];
size_t g_sel = 0;

Preferences g_prefs;
bool g_prefsOpen = false;

// 打开 namespace。失败不致命：设备照常能用，只是设置不跨重启。
// 串口会说明，屏上不显示 —— 屏上没有「持久化坏了」这一行，
// 而设置本身仍然是好的（ADR-0017 的取舍：看得见比省一行重要）。
bool ensureOpen(bool create) {
  if (g_prefsOpen) {
    return true;
  }
  g_prefsOpen = g_prefs.begin(cfg::kSettingsNamespace, create);
  return g_prefsOpen;
}

// 哨兵值：Preferences 的 getUChar 在 key 不存在时返回 defaultValue，
// 用一个不可能合法的档位当哨兵，才能区分「没有 key」与「key 存在但越界」。
constexpr uint8_t kMissing = 0xFF;

const char *srcName(Src s) {
  switch (s) {
    case Src::Nvs:            return "nvs";
    case Src::NvsOutOfRange:  return "default (nvs out of range)";
    case Src::Default:
    default:                  return "default";
  }
}

}  // namespace

const Item *table() { return kTable; }
size_t count() { return kCount; }

// 按 nvsKey 反查行号。存在的理由：main.cpp 要判断「刚刚切的是不是声音三项」，
// 用硬编码的下标（1/2/3）会在以后插入新项时静默失效，而按 key 查是自证的。
// 查不到返回 kCount（调用方按「不是声音项」处理）。
size_t indexOfNvsKey(const char *key) {
  if (key == nullptr) {
    return kCount;
  }
  for (size_t i = 0; i < kCount; ++i) {
    if (kTable[i].nvsKey != nullptr && strcmp(kTable[i].nvsKey, key) == 0) {
      return i;
    }
  }
  return kCount;
}

uint8_t indexOf(size_t i) {
  return (i < kCount) ? g_state[i].index : kTable[0].defaultIndex;
}

uint8_t &indexRef(size_t i) {
  // 越界时的落点：借用第 0 项的槽位，而不是一份 static —— 写进去至少是
  // 一个合法值，且下次 indexOf(0) 读到的就是它，不会出现「written nowhere」。
  if (i >= kCount) {
    return g_state[0].index;
  }
  return g_state[i].index;
}

// 选中行在注册表里的位置。只有一项时 selectNext() 会把它绕回 0，
// 于是「按下去看起来没动」——这是刻意选的（ADR-0017：单选循环），
// 比「按了没反应、行为未定义」好，也不该在屏上多打一行说明。
size_t selected() { return g_sel; }

void selectNext() { g_sel = (g_sel + 1) % kCount; }

uint8_t cycleCurrent() {
  const size_t i = g_sel;
  if (i >= kCount) {
    return 0;
  }
  uint8_t &idx = g_state[i].index;
  idx = (uint8_t)((idx + 1) % kTable[i].optionCount);
  if (kTable[i].apply != nullptr) {
    kTable[i].apply(idx);
  }
  save(i);
  return idx;
}

void loadAll() {
  const bool ok = ensureOpen(false);
  for (size_t i = 0; i < kCount; ++i) {
    g_state[i].index = kTable[i].defaultIndex;
    g_src[i] = Src::Default;
    if (!ok || kTable[i].nvsKey == nullptr) {
      continue;
    }
    const uint8_t raw = g_prefs.getUChar(kTable[i].nvsKey, kMissing);
    if (raw == kMissing) {
      // v0.1.0 升级后第一次开机必然走到这里。不警告：那是一次性事件，
      // 打警告只会把串口刷成噪音。src=default 就是说明。
      continue;
    }
    if (raw >= kTable[i].optionCount) {
      // 越界只可能来自固件升级（档位表变短）或 NVS 损坏。回默认档而不是
      // 夹到最近合法档：夹取会静默给出一个用户没选过的值，而回默认档
      // 可预期、可自证。
      g_state[i].index = kTable[i].defaultIndex;
      g_src[i] = Src::NvsOutOfRange;
      continue;
    }
    g_state[i].index = raw;
    g_src[i] = Src::Nvs;
  }
  // 读完之后逐项生效一次：开机报告打的值必须就是屏上的值。
  // 只对第 0 项调 apply 是 v0.1.1 的写法（那时只有对比度，而对比度必须
  // 开机就生效）。四项之后每一项都得走 —— 声音三项的 apply 只是记状态、
  // 不出声（见 ADR-0022 决策 §4：出声由 main.cpp 的标志位驱动），
  // 所以这里不会因为上电就把喇叭叫起来。
  for (size_t i = 0; i < kCount; ++i) {
    if (kTable[i].apply != nullptr) {
      kTable[i].apply(g_state[i].index);
    }
  }
}

void save(size_t i) {
  if (i >= kCount || kTable[i].nvsKey == nullptr) {
    return;
  }
  if (!ensureOpen(true)) {
    return;
  }
  g_prefs.putUChar(kTable[i].nvsKey, g_state[i].index);
}

// 把选中项写进 NVS（设置版面路径）。热键路径知道自己在改第几项，用 save(i)。
void saveCurrent() { save(g_sel); }

Src srcOf(size_t i) {
  return (i < kCount) ? g_src[i] : Src::Default;
}

const char *srcNameOf(size_t i) {
  if (i >= kCount) {
    return "default";
  }
  return srcName(g_src[i]);
}

const char *currentLabel() {
  return (g_sel < kCount) ? kTable[g_sel].label : "";
}

void formatCurrent(char *buf, size_t cap) {
  if (g_sel >= kCount || kTable[g_sel].format == nullptr) {
    buf[0] = '\0';
    return;
  }
  kTable[g_sel].format(g_state[g_sel].index, buf, cap);
}

}  // namespace settings
