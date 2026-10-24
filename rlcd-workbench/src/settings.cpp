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

#include "config.h"

// cfg_g_lcd_setContrast 的定义（extern 声明在 config.h）。
//
// 为什么不在 config.h 里定义：放头文件里会让每个 include config.h 的
// 翻译单元各拿一份，链接时报 multiple definition（M4 实测撞过一次）。
// 为什么不在匿名 namespace 里：main.cpp 要写它，必须是全局具名符号。
// 所以放在这一层 —— settings 之外、全局之内。副作用：任何 TU 都能改它，
// 因此接钩子的时机只在 main.cpp 的 setup() 里，之后不再有人碰。
void (*cfg_g_lcd_setContrast)(uint8_t) = nullptr;

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

// label 用 UTF-8 转义写，不写中文原文：源文件编码在别处被转换过之后，
// 直接写中文会和字库对不上（fontSelfTest 里星期表头同一套做法）。
const Item kTable[] = {
    {"\xE5\xAF\xB9\xE6\xAF\x94\xE5\xBA\xA6", cfg::kContrastCount, "contrast", applyContrast,
     formatContrast},
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

uint8_t indexOf(size_t i) {
  return (i < kCount) ? g_state[i].index : cfg::kContrastDefaultIdx;
}

uint8_t &indexRef(size_t i) {
  static uint8_t kZero = cfg::kContrastDefaultIdx;
  if (i >= kCount) {
    return kZero;
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
    g_state[i].index = cfg::kContrastDefaultIdx;
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
      g_state[i].index = cfg::kContrastDefaultIdx;
      g_src[i] = Src::NvsOutOfRange;
      continue;
    }
    g_state[i].index = raw;
    g_src[i] = Src::Nvs;
  }
  // 读完之后立刻生效一次：开机报告打的值必须就是屏上的值。
  if (kTable[0].apply != nullptr) {
    kTable[0].apply(g_state[0].index);
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
