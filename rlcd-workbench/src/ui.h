// rlcd-workbench / 界面层
//
// M1 阶段的数据全部是假数据（main.cpp 里灌），本文件只负责「把状态画成 400x300
// 的 1bpp 位图」。版式数字全部来自 docs/策划案.md §4.2 / §4.3 / §4.4 的实测字宽，
// 不在这里现算。

#pragma once

#include <U8G2lib.h>

#include "config.h"

struct TodoItem {
  char text[32];
  char due[16];
};

// 探针三态。少了 Unknown，字段还没部署时六个格子会全画成叉 ——
// 「这一项还没接」和「这一项挂了」在屏上必须长得不一样。
enum ProbeState : uint8_t {
  PROBE_UNKNOWN = 0,  // 字段缺失或为 null：画空心框 + 问号
  PROBE_OK = 1,       // 黑底白勾
  PROBE_FAIL = 2,     // 空心框 + 叉
};

struct Probe {
  const char *label;
  ProbeState state;
};

enum WifiState : uint8_t {
  WIFI_OFF = 0,         // 断开：画叉
  WIFI_CONNECTING = 1,  // 连接中：画半格
  WIFI_ON = 2,          // 已连：画满格
};

struct AppState {
  int panel = 0;   // 0 = 本地，1 = 服务器
  bool about = false;

  int year = 2026, month = 10, day = 22;
  int weekday = 4;  // 1 = 周一 … 7 = 周日
  int hour = 10, minute = 22, second = 31;

  float tempC = 24.5f;
  float humi = 48.0f;

  int batteryPct = 87;
  WifiState wifi = WIFI_ON;

  int todoTotal = 6;
  int todoDone = 0;
  TodoItem todos[cfg::kTodoSlots] = {};

  int tripMonthKey = 202610;           // yyyymm，用来防「换月瞬间标错月份」
  uint8_t tripCount[32] = {};          // 索引 1..31

  Probe probes[7] = {};  // 面板二右栏：六个服务探针 + 证书单独占一行
  int probeCount = 0;
  int cpuPct = 23;
  int memPct = 71;
  int diskPct = 53;
  float load1 = 0.42f;
  char memDetail[32] = "1229M/453M/1682M";
  char diskDetail[32] = "21.4G/18.6G/40.0G";

  // 哪些行是真的有值。没置位的画「—」而不是画一个看起来像真数据的 0 ——
  // 「服务器还没回话」和「CPU 占用 0%」在屏上不能长成一个样。
  enum : uint8_t {
    KN_CPU = 1 << 0,
    KN_MEM = 1 << 1,
    KN_DISK = 1 << 2,
    KN_LOAD = 1 << 3,
    KN_CERT = 1 << 4,
  };
  uint8_t known = 0;
  int certDays = 74;

  // 服务器数据的新鲜度：页尾要能说出「数据是 42 秒前的」。
  bool serverHasData = false;
  bool serverStale = false;
  uint32_t serverAgeSec = 0;

  // 待办三态与服务器面板同构：「还没拿到」「拿到了」「拿到了但有坏行」必须
  // 分得开。没有 todoHasData 的话，首次上电会显示「无待办」——那不是未知，
  // 那是在断言一件还不知道的事。
  bool todoHasData = false;
  bool todoStale = false;
  bool todoBad = false;
};

namespace ui {

// 画一整帧。show_header_time=false 时页首中段留空（面板一时用，见 ADR-0002）。
void drawFrame(U8G2 *g, const AppState &s, bool showHeaderTime);

// 关于页是覆盖层，不是面板。
void drawAbout(U8G2 *g, const AppState &s);

// BOOT 长按切对比度时，页首角落临时显示「对比度 3/5」。
void drawContrastHint(U8G2 *g, int idx, int total);

// 页首角落的临时提示（预留槽等）。同一块位置，2 秒后由主循环擦掉。
void drawToast(U8G2 *g, const char *text);

// 面板一左栏的秒位：只重绘这一小块，不整帧重画（docs/策划案.md §4.7）。
void drawSeconds(U8G2 *g, const AppState &s);
void secondsRect(int *x, int *y, int *w, int *h);

// 面板二页尾中段要显示的那句话（「无数据」/「离线 42s」/「停更 1m」）。
// 正常时返回 false，页尾保持安静。抽成函数是为了让主循环能把同一句话打进
// 串口 —— 否则「屏上到底该显示什么」只能靠肉眼看屏，没法在自动化里证明。
bool serverFooterNote(const AppState &s, char *buf, size_t cap);

// 面板一页尾中段：拿不到 / 停更 / 有坏行 / 未完成超过 6 条（`+N`）。
// 正常时返回 false，页尾保持安静。
bool todoFooterNote(const AppState &s, char *buf, size_t cap);

// 把 UI 用到的那几个字库显式引用一遍，保证 linker 不会因为
// section 回收把没被直接点名的字库丢掉。
void touchFonts();

// 供字宽自检用：返回 UI 真正在用的那几张字库。自检和界面共用一个来源，
// 免得改了 ui.cpp 的字库之后策划案里的宽度数字悄悄失效。
const uint8_t *fontCn();
const uint8_t *fontBig();
const uint8_t *fontSec();
const uint8_t *fontMid();

}  // namespace ui
