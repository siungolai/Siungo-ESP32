// rlcd-workbench / 界面层实现（M1）
//
// 坐标系：400 宽 x 300 高，左上角原点。
//   页首 0..24，分隔线 24；内容 25..277；分隔线 277；页尾 278..299。
//   两栏等宽：左 0..200，右 200..400，竖分隔线画在 x=200。

#include "ui.h"

#include "goose_frames.h"
#include "settings.h"
#include "version.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "timeutil.h"

namespace {

// u8g2 的 ST7305 里 1 = 黑（发色），0 = 白。
constexpr uint8_t INK = 1;
constexpr uint8_t PAPER = 0;

const uint8_t *F_CN = u8g2_font_wqy12_t_gb2312;
// HH:MM 用 58 不用 62：右上板实测，62 的 "10:22" 是 192 px —— 正好等于左栏内宽，
// 一点空隙都没有（离线那把尺子当年量成 179，是偏小的）。58 实测 177 px，
// 左右各留 7.5 px，才是「横向占满 + 留一点点空隙」。
const uint8_t *F_BIG = u8g2_font_logisoso58_tn;
const uint8_t *F_SEC = u8g2_font_logisoso20_tn;  // 秒，实测 26~28 px
const uint8_t *F_MID = u8g2_font_logisoso32_tf;  // 温湿度，带 °C %
const uint8_t *F_VAL = u8g2_font_logisoso28_tf;  // 面板二数值

// u8g2 的 drawStr 基线在原点上方的 ascent 处，这里统一按「基线 y」调用。
int tw(U8G2 *g, const char *s) { return (int)g->getUTF8Width(s); }

void text(U8G2 *g, int x, int baseline, const char *s) {
  g->drawUTF8(x, baseline, s);
}

// 按 pen 宽度居中。注意：u8g2 不暴露每个字形的左侧 bearing，
// 所以字形带 bearing 时（例如 logisoso62 的 '1'）墨迹会比 pen 盒偏右几像素。
// 预览图是按墨迹居中的，上板后要核对这一差异（§7 待验证第 4 项）。
void textCenter(U8G2 *g, int cx, int baseline, const char *s) {
  g->drawUTF8(cx - tw(g, s) / 2, baseline, s);
}

void textRight(U8G2 *g, int xRight, int baseline, const char *s) {
  g->drawUTF8(xRight - tw(g, s), baseline, s);
}

// 按像素宽裁一行 UTF-8：只画放得下的完整字符，切在字符边界上。
// 为什么归渲染层管：可用宽度取决于这一行有没有到期栏，而到期栏是
// 「今天 / 明天 / 逾期6天 / 11-05」，长短不一 —— 数据层算不出这个宽度。
// 不补省略号（v0.1.4 流程文档 §0 铁律三）：凭空多一个字符会让人以为原文如此。
void textClipped(U8G2 *g, int x, int baseline, const char *s, int maxW) {
  if (s == nullptr || s[0] == '\0' || maxW <= 0) return;
  char acc[64];
  size_t o = 0;
  size_t fit = 0;
  const char *p = s;
  while (*p != '\0') {
    const unsigned char c = (unsigned char)*p;
    int len = 1;
    if ((c & 0xF8) == 0xF0) {
      len = 4;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
    }
    if (o + (size_t)len + 1 > sizeof(acc)) break;
    // 数据层是按字节截的，可能在多字节序列中间断开。续字节读到 '\0' 就说明
    // 这个字符不完整 —— 画半截汉字出来等于凭空造一个不存在的字，宁可不画。
    bool whole = true;
    for (int i = 0; i < len; ++i) {
      if (p[i] == '\0') {
        whole = false;
        break;
      }
    }
    if (!whole) break;
    memcpy(acc + o, p, (size_t)len);
    o += (size_t)len;
    acc[o] = '\0';
    if (tw(g, acc) > maxW) {
      acc[fit] = '\0';
      break;
    }
    fit = o;
    p += len;
  }
  if (acc[0] != '\0') text(g, x, baseline, acc);
}

void hline(U8G2 *g, int x, int y, int w) { g->drawHLine(x, y, w); }
void vline(U8G2 *g, int x, int y, int h) { g->drawVLine(x, y, h); }
void box(U8G2 *g, int x, int y, int w, int h) { g->drawFrame(x, y, w, h); }
void fill(U8G2 *g, int x, int y, int w, int h) { g->drawBox(x, y, w, h); }

// 虚线框：「还没拿到」这条语言本身（页首电池未知态在用）。
// 它曾经也是宠物插件占位框的画法，v0.1.3 起那块地方画真的鹅（ADR-0019）。
void dashedBox(U8G2 *g, int x, int y, int w, int h) {
  for (int i = 0; i < w; i += 4) {
    g->drawPixel(x + i, y);
    g->drawPixel(x + w - 1 - i, y + h - 1);
  }
  for (int i = 0; i < h; i += 4) {
    g->drawPixel(x, y + h - 1 - i);
    g->drawPixel(x + w - 1, y + i);
  }
}

// ---- 图标（手绘，u8g2 内置图标字体尺寸不合用）----

// 电池：17x9 外框 + 右侧 2 px 凸点，内部留 1 px 白边。
// pct < 0 = 还没有读数：虚线框、不填内部。
// 实线框 + 空电池就是「0%」的样子，不能拿来表示「不知道」。
// 页上表达「还没拿到」本来就有虚线这条语言（chipUnknown 的隔点框也是它），
// 这里复用 dashedBox，不再手写一套点距。
void battery(U8G2 *g, int x, int y, int pct) {
  if (pct < 0) {
    dashedBox(g, x, y, 17, 9);
    g->drawPixel(x + 17, y + 4);  // 凸点也只剩轮廓
    g->drawPixel(x + 18, y + 4);
    return;
  }
  box(g, x, y, 17, 9);
  fill(g, x + 17, y + 3, 2, 3);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const int innerW = 13;
  const int w = innerW * pct / 100;
  if (w > 0) fill(g, x + 2, y + 2, w, 5);
}

// WiFi 三态。满格 / 半格 / 叉
void wifi(U8G2 *g, int x, int y, WifiState s) {
  if (s == WIFI_ON) {
    // 三道弧用三条递增的横线近似（1bpp 下画弧不划算）
    hline(g, x + 4, y, 5);
    hline(g, x + 2, y + 3, 9);
    hline(g, x + 0, y + 6, 13);
  } else if (s == WIFI_CONNECTING) {
    hline(g, x + 4, y, 5);
    hline(g, x + 2, y + 3, 9);
    g->drawPixel(x + 6, y + 7);
  } else {
    hline(g, x + 0, y + 6, 13);
    for (int i = 0; i < 9; ++i) {
      g->drawPixel(x + i + 2, y + i);
      g->drawPixel(x + 10 - i, y + i);
    }
  }
}

// 状态块：黑底白勾（20x17），1 px 细勾在 1bpp 下看不见，所以做成实心块
void chipOk(U8G2 *g, int x, int y) {
  fill(g, x, y, 20, 17);
  g->setDrawColor(PAPER);
  // 勾的拐点必须在**下方**：左短臂从左上斜下来，右长臂从拐点斜上去。
  // 之前写反了 —— 拐点在上、两端在下，画出来是个「Λ」，一上板就能看出来。
  const int vx = x + 8, vy = y + 11;
  for (int t = 0; t < 2; ++t) {
    for (int i = 0; i < 4; ++i) g->drawPixel(vx - 4 + i, vy - 4 + i + t);  // 左上 → 拐点
    for (int i = 0; i < 8; ++i) g->drawPixel(vx + i, vy - i + t);          // 拐点 → 右上
  }
  g->setDrawColor(INK);
}

void chipUnknown(U8G2 *g, int x, int y) {
  // 虚线框 + 中间一个问号：明确表达「这一项还没接」，不是「坏了」。
  for (int i = 0; i < 20; i += 2) {
    g->drawPixel(x + i, y);
    g->drawPixel(x + i, y + 16);
  }
  for (int i = 0; i < 17; i += 2) {
    g->drawPixel(x, y + i);
    g->drawPixel(x + 19, y + i);
  }
  g->setFont(F_CN);
  g->drawStr(x + 7, y + 13, "?");
}

void chipFail(U8G2 *g, int x, int y) {
  box(g, x, y, 20, 17);
  for (int t = 0; t < 2; ++t) {
    for (int i = 0; i < 9; ++i) {
      g->drawPixel(x + 6 + i, y + 5 + i + t);
      g->drawPixel(x + 14 - i, y + 5 + i + t);
    }
  }
}

// ---- 日期 ----
//
// 闰年判定 / 每月天数 / 跨月进退位都在 timeutil.h 里。月历进位与时间进位
// 必须共用同一份天数表，否则跨月那天两边会算出不同的日期。

// ---- 页首 / 页尾 ----

void drawHeader(U8G2 *g, const AppState &s, bool showTime) {
  g->setFont(F_CN);
  g->setDrawColor(INK);

  // 左：日期 + 星期（实测 45 + 22 = 67 px）
  static const char *kWd[] = {"一", "二", "三", "四", "五", "六", "日"};
  char buf[32];
  int wd = s.weekday;
  if (wd < 1 || wd > 7) wd = 1;
  snprintf(buf, sizeof(buf), "%d月%d日周%s", s.month, s.day, kWd[wd - 1]);
  text(g, cfg::kMarginL, 18, buf);

  // 中：时间（面板一隐藏，因为面板一左栏已经有一个巨大的时钟）
  if (showTime) {
    g->setFont(F_SEC);
    snprintf(buf, sizeof(buf), "%02d:%02d", s.hour, s.minute);
    textCenter(g, cfg::kLcdWidth / 2, 22, buf);
  }

  // 右：待办未完成数 + 电量百分比 + 电池 + WiFi，从右边缘往左排
  g->setFont(F_CN);
  int x = cfg::kMarginR;
  wifi(g, x - 13, 8, s.wifi);
  x -= 13 + 6;
  battery(g, x - 19, 8, s.batteryPct);
  x -= 19 + 6;

  // 百分比数字（v0.1.2）。宽度必须用 tw() 实测：wqy12 里数字和 '%' 不等宽，
  // "--%"（3 字符）与 "100%"（4 字符）差好几个像素，写死 x 会错位。
  //
  // 图标和数字不是冗余：「还剩多少」看填充条一眼就有，读数字才需要停一下。
  // 两者是不同速度档位的信息。
  if (s.batteryPct < 0) {
    snprintf(buf, sizeof(buf), "--%%");  // 还不知道，不是 0%
  } else {
    int pct = s.batteryPct;
    if (pct > 100) pct = 100;
    snprintf(buf, sizeof(buf), "%d%%", pct);
  }
  textRight(g, x, 18, buf);
  x -= tw(g, buf) + 6;

  if (!s.todoHasData) {
    // 「还没拿到」不能画成「0 条待办」——那是在断言一件还不知道的事。
    snprintf(buf, sizeof(buf), "待办—");
  } else {
    // stats.pending：全部未完成（含无截止日期、含窗口外）。不是「总数−已完成」，
    // 因为新契约的下发里根本没有「已完成」这个概念（ADR-0020）。
    snprintf(buf, sizeof(buf), "待办%d", s.todoPending);
  }
  textRight(g, x, 18, buf);

  hline(g, 0, cfg::kHeaderH, cfg::kLcdWidth);
}

bool footerNote(const AppState &s, char *buf, size_t cap) {
  const char *note = nullptr;
  if (!s.serverHasData) {
    note = "无数据";
  } else if (s.wifi != WIFI_ON) {
    note = "离线";
  } else if (s.serverStale) {
    note = "停更";
  }
  if (note == nullptr) {
    return false;
  }
  const uint32_t a = s.serverAgeSec;
  if (!s.serverHasData) {
    snprintf(buf, cap, "%s", note);
  } else if (a < 60) {
    snprintf(buf, cap, "%s %us", note, (unsigned)a);
  } else {
    snprintf(buf, cap, "%s %um", note, (unsigned)(a / 60u));
  }
  return true;
}

bool todoNote(const AppState &s, char *buf, size_t cap) {
  // 顺序 = 「越可能是人为配置错，越先说」。网络类的「无数据/停更」放最后：
  // 它最常见，也最不需要人动手。三种非网络失败各自一句话 —— 混成一句
  // 「待办数据异常」等于指着一个要去改配置的人说「你网不好」。
  if (s.todoKeyRejected) {
    snprintf(buf, cap, "密钥未配置");
    return true;
  }
  if (s.todoEndpointMissing) {
    snprintf(buf, cap, "端点未部署");
    return true;
  }
  if (s.todoUpgrade) {
    snprintf(buf, cap, "需升级");
    return true;
  }
  if (!s.todoHasData) {
    // 「一次都没拿到过」和「拿到了但停更」要分开说：前者是还没开始，
    // 后者是开始了又断了。流程文档（v0.1.4）点名的也是这两个词。
    snprintf(buf, cap, "等待数据");
    return true;
  }
  if (s.todoBad) {
    snprintf(buf, cap, "待办数据异常");
    return true;
  }
  if (s.todoStale) {
    snprintf(buf, cap, "停更");
    return true;
  }
  // 页首是 stats.pending（全量未完成），屏上最多 6 行。差额必须解释：
  // 「待办12」配 6 行会看起来像丢了 6 条，而那 6 条只是没有截止日期
  // 或者落在窗口外 —— 它们确实存在，只是排不进这 6 个格子（ADR-0020）。
  // 口径是「页首数 − 屏上实际行数」，不是「− 6」：服务端少给几条时，
  // 少掉的那几条同样要能被页尾解释掉（A8 第 ④ 条）。
  int rows = 0;
  for (int i = 0; i < cfg::kTodoSlots; ++i) {
    if (s.todos[i].text[0] != '\0') ++rows;
  }
  if (s.todoPending > rows) {
    snprintf(buf, cap, "+%d", s.todoPending - rows);
    return true;
  }
  return false;
}

void drawFooter(U8G2 *g, const AppState &s) {
  hline(g, 0, cfg::kContentBottom, cfg::kLcdWidth);
  g->setFont(F_CN);
  g->setDrawColor(INK);
  text(g, cfg::kMarginL, cfg::kContentBottom + 16, s.panel == 0 ? "本地" : "服务器");
  char buf[16];
  snprintf(buf, sizeof(buf), "%d/2", s.panel + 1);
  textRight(g, cfg::kMarginR, cfg::kContentBottom + 16, buf);

  // 中段只在「需要解释」的时候才出声：没数据 / 停更 / 断网。
  // 正常时页尾保持安静 —— 一个每秒都在变的页脚会让人以为出了什么事。
  // 龄期用设备单调时钟算（ADR-0009），不用快照里的 generated_at。
  char mid[24];
  const bool has = (s.panel == 0) ? todoNote(s, mid, sizeof(mid))
                                  : footerNote(s, mid, sizeof(mid));
  if (has) {
    textCenter(g, cfg::kLcdWidth / 2, cfg::kContentBottom + 16, mid);
  }
}

// ---- 面板一：本地 ----

void drawCalendar(U8G2 *g, const AppState &s) {
  g->setFont(F_CN);

  // 星期表头：一二三四五六日，实测 81 px
  static const char *kWd[] = {"一", "二", "三", "四", "五", "六", "日"};
  for (int c = 0; c < 7; ++c) {
    textCenter(g, cfg::kCalLeft + c * cfg::kCalColPitch + cfg::kCalCellW / 2,
               cfg::kCalTop - 2, kWd[c]);
  }

  // 窗口起点 = 本周一 - 7 天（行 0 上周、行 1 本周、行 2 下周）
  int wy = s.year, wm = s.month, wdd = s.day;
  tu::addDays(wy, wm, wdd, -((s.weekday - 1) + 7));

  char buf[8];
  for (int r = 0; r < cfg::kCalRows; ++r) {
    const int y0 = cfg::kCalTop + r * cfg::kCalRowPitch;
    for (int c = 0; c < 7; ++c) {
      const int x0 = cfg::kCalLeft + c * cfg::kCalColPitch;
      const bool inMonth = (wy == s.year && wm == s.month);
      const bool today = (wy == s.year && wm == s.month && wdd == s.day);

      // 非本月日期不画外框（§4.3，七方案里选的 B）
      if (inMonth) {
        if (today) {
          fill(g, x0, y0, cfg::kCalCellW, cfg::kCalRowH);
        } else {
          box(g, x0, y0, cfg::kCalCellW, cfg::kCalRowH);
        }
      }

      snprintf(buf, sizeof(buf), "%d", wdd);
      g->setDrawColor(today ? PAPER : INK);
      textCenter(g, x0 + cfg::kCalCellW / 2, y0 + 14, buf);

      // 行程计数点：1..3 个 3x3，只标本月，当天用白点
      if (inMonth && s.tripMonthKey == s.year * 100 + s.month) {
        int n = (wdd >= 1 && wdd <= 31) ? s.tripCount[wdd] : 0;
        if (n > 3) n = 3;
        if (n > 0) {
          g->setDrawColor(today ? PAPER : INK);
          const int dotW = 3, gap = 2;
          const int total = n * dotW + (n - 1) * gap;
          int dx = x0 + (cfg::kCalCellW - total) / 2;
          for (int i = 0; i < n; ++i) {
            fill(g, dx, y0 + 15, dotW, 3);
            dx += dotW + gap;
          }
        }
      }
      g->setDrawColor(INK);

      tu::addDays(wy, wm, wdd, 1);
    }
  }
}

void drawTodos(U8G2 *g, const AppState &s) {
  g->setFont(F_CN);
  g->setDrawColor(INK);

  // 还没拿到数据时不能写「无待办」——那是把「不知道」说成了「没有」。
  if (!s.todoHasData) {
    text(g, cfg::kCalLeft, cfg::kTodoTop + 11, "—");
    return;
  }
  if (s.todoPending <= 0) {
    text(g, cfg::kCalLeft, cfg::kTodoTop + 11, "无待办");
    return;
  }

  for (int i = 0; i < cfg::kTodoSlots; ++i) {
    if (s.todos[i].text[0] == '\0') {
      continue;  // 空槽不占行
    }
    const int y0 = cfg::kTodoTop + i * cfg::kTodoPitch;
    // 勾选框恒为空：新契约不下发逐条完成状态，槽位里装的全是未完成条目，
    // 画满格等于编造一个设备根本没有的事实（ADR-0020）。
    box(g, cfg::kCalLeft, y0 + 1, 7, 7);

    // 正文可用宽度现场算：先去把到期栏画在右边，再拿它左边的空隙当边界。
    int right = cfg::kMarginR;
    if (s.todos[i].due[0] != '\0') {
      textRight(g, cfg::kMarginR, y0 + 10, s.todos[i].due);
      right = cfg::kMarginR - tw(g, s.todos[i].due) - 4;
    }
    textClipped(g, cfg::kCalLeft + 12, y0 + 10, s.todos[i].text,
                right - (cfg::kCalLeft + 12));
  }
}

// ---- 宠物（鹅）：逐帧点阵（v0.1.3，docs/adr/0019 决定 2 / 决定 3）----
//
// 用户明确要求：每一帧都是手工排的点阵，不用任何几何图元（m00723）。
// 帧由 .pio/goose_frames.py 手工编辑（40x56，'#'=黑、'o'=挖出白、'.'=背景），
// 再由 .pio/make_frames_h.py 生成 src/goose_frames.h（u8g2 XBMP 位序）。
// 帧号由 pet::View::frame 给出，ui 只负责取出来画，不推断「该画哪张」。

void drawGoose(U8G2 *g, int x, int y, int w, int h, const pet::View &v) {
  (void)w;
  (void)h;
  // 40x56 的点阵放在 84x107 区域的中间偏下（左 22 px、顶 30 px），
  // 让鹅站在区域底部附近，头顶留出活动空间。
  const unsigned char *bmp =
      (const unsigned char *)pgm_read_ptr(&goosebmp::kFrames[v.frame]);
  g->drawXBMP((u8g2_uint_t)(x + 22), (u8g2_uint_t)(y + 30), goosebmp::kW, goosebmp::kH, bmp);
}


void drawLocal(U8G2 *g, const AppState &s) {
  vline(g, cfg::kSplitX, cfg::kContentTop, cfg::kContentBottom - cfg::kContentTop);
  g->setDrawColor(INK);

  // ---- 左栏 ----
  char buf[16];
  g->setFont(F_BIG);
  snprintf(buf, sizeof(buf), "%02d:%02d", s.hour, s.minute);
  // 基线 109：字高 62，墨迹顶落在 y=47，与右栏星期几基线齐平（§4.3）
  textCenter(g, 100, 109, buf);

  g->setFont(F_SEC);
  snprintf(buf, sizeof(buf), "%02d", s.second);
  textCenter(g, 100, 138, buf);

  // 宠物（鹅）：严格 84x107（x 4..87 / y 159..265）。虚线占位框退场
  // （v0.1.3，docs/adr/0019 决定 1 / 决定 2）。
  drawGoose(g, 4, 159, 84, 107, s.petView);

  // 温湿度右对齐到 196，只有数字和单位，无中文标签
  //
  // 注意左边那只鹅（区域 x 4..87）：温度串一旦超过 104 px 就会压到它身上。
  // 上板实测 "24.5°C" 是 103 px（刚好挤进），但 "-12.3°C" 是 125 px —— 冬天或者
  // 把板子放到空调出风口就会撞框。所以先量宽度再选字号，别赌读数永远正数。
  snprintf(buf, sizeof(buf), "%.1f°C", s.tempC);
  g->setFont(F_MID);
  if ((int)g->getUTF8Width(buf) > 104) {
    g->setFont(u8g2_font_logisoso24_tf);
  }
  textRight(g, 196, 208, buf);
  g->setFont(F_MID);
  snprintf(buf, sizeof(buf), "%.0f%%", s.humi);
  textRight(g, 196, 252, buf);

  // ---- 右栏 ----
  drawCalendar(g, s);
  drawTodos(g, s);
}

// ---- 面板二：服务器 ----

void drawServer(U8G2 *g, const AppState &s) {
  vline(g, cfg::kSplitX, cfg::kContentTop, cfg::kContentBottom - cfg::kContentTop);
  g->setDrawColor(INK);

  char buf[48];

  // ---- 左栏：4 行，行距 62 ----
  struct Row {
    const char *label;
    const char *detail;
    char value[16];
    int pct;  // <0 表示不画进度条
  };
  Row rows[4];
  const bool kCpu = (s.known & AppState::KN_CPU) != 0;
  const bool kMem = (s.known & AppState::KN_MEM) != 0;
  const bool kDisk = (s.known & AppState::KN_DISK) != 0;
  const bool kLoad = (s.known & AppState::KN_LOAD) != 0;

  rows[0] = {"CPU", nullptr, {0}, kCpu ? s.cpuPct : -1};
  if (kCpu) {
    snprintf(rows[0].value, sizeof(rows[0].value), "%d%%", s.cpuPct);
  } else {
    snprintf(rows[0].value, sizeof(rows[0].value), "--");
  }

  rows[1] = {"内存", kMem ? s.memDetail : nullptr, {0}, kMem ? s.memPct : -1};
  if (kMem) {
    snprintf(rows[1].value, sizeof(rows[1].value), "%d%%", s.memPct);
  } else {
    snprintf(rows[1].value, sizeof(rows[1].value), "--");
  }

  rows[2] = {"磁盘", kDisk ? s.diskDetail : nullptr, {0}, kDisk ? s.diskPct : -1};
  if (kDisk) {
    snprintf(rows[2].value, sizeof(rows[2].value), "%d%%", s.diskPct);
  } else {
    snprintf(rows[2].value, sizeof(rows[2].value), "--");
  }

  rows[3] = {"负载", nullptr, {0}, -1};
  if (kLoad) {
    snprintf(rows[3].value, sizeof(rows[3].value), "%.2f", (double)s.load1);
  } else {
    snprintf(rows[3].value, sizeof(rows[3].value), "--");
  }

  for (int i = 0; i < 4; ++i) {
    const int y0 = 32 + i * 62;
    g->setFont(F_CN);
    text(g, cfg::kMarginL, y0 + 11, rows[i].label);
    if (rows[i].detail != nullptr) {
      text(g, cfg::kMarginL + 28, y0 + 40, rows[i].detail);
    }
    g->setFont(F_VAL);
    textRight(g, 196, y0 + 40, rows[i].value);
    if (rows[i].pct >= 0) {
      const int bw = 192, bh = 5;
      const int by = y0 + 46;
      box(g, cfg::kMarginL, by, bw, bh);
      fill(g, cfg::kMarginL + 1, by + 1, (bw - 2) * rows[i].pct / 100, bh - 2);
    }
  }

  // ---- 右栏：7 行（六个探针 + 证书），行距 34 ----
  for (int i = 0; i < 6 && i < s.probeCount; ++i) {
    const int y0 = 32 + i * 34;
    g->setFont(F_CN);
    text(g, cfg::kCalLeft, y0 + 11, s.probes[i].label);
    switch (s.probes[i].state) {
      case PROBE_OK:
        chipOk(g, cfg::kMarginR - 19, y0 - 1);
        break;
      case PROBE_FAIL:
        chipFail(g, cfg::kMarginR - 19, y0 - 1);
        break;
      default:
        chipUnknown(g, cfg::kMarginR - 19, y0 - 1);
        break;
    }
  }

  // 证书天数：74 天
  const int y0 = 32 + 6 * 34;
  g->setFont(F_CN);
  text(g, cfg::kCalLeft, y0 + 11, "证书");
  g->setFont(F_VAL);
  if ((s.known & AppState::KN_CERT) != 0) {
    snprintf(buf, sizeof(buf), "%d", s.certDays);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  textRight(g, cfg::kMarginR - 13, y0 + 28, buf);
  g->setFont(F_CN);
  textRight(g, cfg::kMarginR, y0 + 28, "天");
}

}  // namespace

namespace ui {

bool serverFooterNote(const AppState &s, char *buf, size_t cap) {
  return footerNote(s, buf, cap);
}

bool todoFooterNote(const AppState &s, char *buf, size_t cap) {
  return todoNote(s, buf, cap);
}

void touchFonts() {
  volatile const uint8_t *p[] = {F_CN, F_BIG, F_SEC, F_MID, F_VAL};
  (void)p;
}

const uint8_t *fontCn() { return F_CN; }
const uint8_t *fontBig() { return F_BIG; }
const uint8_t *fontSec() { return F_SEC; }
const uint8_t *fontMid() { return F_MID; }

void drawFrame(U8G2 *g, const AppState &s, bool showHeaderTime) {
  g->clearBuffer();
  g->setDrawColor(INK);
  drawHeader(g, s, showHeaderTime);
  drawFooter(g, s);
  if (s.panel == 0) {
    drawLocal(g, s);
  } else {
    drawServer(g, s);
  }
}

void drawAbout(U8G2 *g, const AppState &s) {
  (void)s;
  g->clearBuffer();
  g->setDrawColor(INK);

  // 黑条标题
  fill(g, 0, 0, cfg::kLcdWidth, 26);
  g->setDrawColor(PAPER);
  g->setFont(F_CN);
  text(g, 8, 18, "关于这台工作台");
  g->setDrawColor(INK);

  g->setFont(F_CN);
  // 数据源主机来自 src/secrets.h，所以这一行必须运行时拼 —— 不能写成字符串字面量，
  // 否则真实域名又回到公开仓库里了。
  static char s_dataRow[48];
  snprintf(s_dataRow, sizeof(s_dataRow), "数据源  %s/status/", cfg::kDataHost);
  // 版本号从 version.h 来，不写字面量 —— 关于页与开机报告必须说同一个数。
  static char s_verRow[40];
  snprintf(s_verRow, sizeof(s_verRow), "固件    %s (%s)", ver::kVersion, ver::kDate);
  const char *kRows[] = {
      "目标板  Waveshare ESP32-S3-RLCD-4.2",
      s_verRow,
      "屏幕    4.2 寸 400x300 全反射黑白",
      "时间源  PCF85063 RTC + NTP 校正",
      s_dataRow,
      "密钥    X-Monitor-Key（src/secrets.h）",
  };
  int y = 44;
  for (const char *row : kRows) {
    text(g, 8, y, row);
    y += 17;
  }

  hline(g, 8, y + 2, cfg::kLcdWidth - 16);
  y += 14;

  // 下半区两版互斥：教程版面 / 设置版面（v0.1.1，docs/adr/0017）。
  // 设置版面不是第二张覆盖层，只替换这一块 —— 上半的设备信息始终在场，
  // 所以用户在改设置时也看得见自己改的是哪块板、哪个固件。
  // 两版共用同一批固定行：上电 1 秒 BOOT 限制与底部「无背光」提示 ——
  // 它们在设置版面里同样重要，所以画在 drawAbout 尾部而不是任一半区里。
  if (s.aboutSettings) {
    drawSettings(g, s);
  } else {
    const char *kGestures[] = {
        "KEY  短按  下一页 / 下一面板",
        "KEY  长按  回主页并清状态",
        "KEY  双击  关于页（本页）",
        "BOOT 短按  教程页 / 设置页",
        "BOOT 长按  设置页内切换选项",
        "BOOT 双击  预留，未绑定",
    };
    for (const char *row : kGestures) {
      text(g, 8, y, row);
      y += 16;
    }
  }

  // 上电 1 秒内按 BOOT 是下载模式，不是我们的手势 —— 这一条写在屏上比写在
  // 文档里有用：见过的人不会以为"按键坏了"。
  text(g, 8, 282, "上电 1 秒内 BOOT 键无效");
  textCenter(g, cfg::kLcdWidth / 2, 298, "无背光，请放在有光处");
}

// 设置版面（关于页下半区，docs/adr/0017；滚动窗口见 docs/adr/0023）。
//
// 版面预算（y=160..300）：
//   160  小标题「设置」+ 右侧位置指示「1-4/5」
//   178  设置行（窗口第 0 行，选中行整行反显）
//   196  设置行（窗口第 1 行）
//   214  设置行（窗口第 2 行）
//   232  设置行（窗口第 3 行）        ← 窗口 4 行，y > 240 截断
//   250  BOOT 短按  选择行
//   266  BOOT 长按  切换
//   282  上电 1 秒内 BOOT 键无效（drawAbout 统一画）
//   298  无背光，请放在有光处（drawAbout 统一画）
//
// 第 5 项出现后（音量，ADR-0023）用**固定窗口**滚动：窗口永远是 4 行，
// 只有选中行滚出可见区时才整窗上滚一行。两行固定键位提示不参与滚动 ——
// 「暗处看不见」与「上电 1 秒 BOOT 无效」都是「按键/屏幕看起来坏了」的直接
// 答案，比多一行设置值更该留在屏上。
//
// 位置指示画在标题行右侧的空白处（y=160 右边约 300 px 没人用），零版面成本。
// 它显示的**不是选中项**而是窗口覆盖范围：稳态 `1-4/5`、滚一格后 `2-5/5`。
// 只显示 `3/5` 的话用户看不出还没看到的项在上面还是下面。
void drawSettings(U8G2 *g, const AppState &s) {
  (void)s;
  g->setFont(F_CN);

  // 小标题。黑条标题仍写「关于这台工作台」—— 上半区还是设备信息，
  // 写「设置」会撒谎；「我现在在下半区」用一行小标签比改黑条便宜。
  text(g, 8, 160, "设置");

  const size_t n = settings::count();
  const size_t sel = settings::selected();
  const int kTop = 178;
  const int kPitch = 18;
  const int kRowH = 16;
  const size_t kVisible = 4;

  // 固定窗口的起点：保证选中行落在 [first, first+kVisible) 里。
  // 窗口只在选中行越界时才挪动，且一次只挪一格 —— 这比「窗口居中选中行」
  // 稳定：居中会让滚到第 3 项时窗口就开始动，屏幕一直在抖。
  size_t first = 0;
  if (n > kVisible) {
    if (sel >= kVisible) {
      first = sel - kVisible + 1;
    }
    // 上面这个式子已经保证 sel < first + kVisible；再兜一次边界。
    if (first + kVisible > n) {
      first = n - kVisible;
    }
  }

  // 位置指示：窗口覆盖范围。只在窗口真的不满一屏时不画（n <= kVisible）。
  if (n > kVisible) {
    char pos[16];
    snprintf(pos, sizeof(pos), "%u-%u/%u", (unsigned)(first + 1),
             (unsigned)(first + kVisible), (unsigned)n);
    textRight(g, cfg::kLcdWidth - 8, 160, pos);
  }

  for (size_t i = first; i < n && i < first + kVisible; ++i) {
    const int y = kTop + (int)(i - first) * kPitch;
    if (y > 240) {
      break;  // 版面上限，见上面的预算表
    }
    const settings::Item &it = settings::table()[i];
    char val[24];
    it.format(settings::indexOf(i), val, sizeof(val));
    // 行内容 = 标签 + 右对齐的当前值。右对齐让多行值的个位对齐，
    // 切档时只有数字部分在动，标签不动 —— 1bpp 上跳动越少越不糊。
    if (i == sel) {
      // 选中行整行反显。选它而不是箭头/外框：月历「今天」整格填黑同一套
      // 视觉语言，而且调对比度时黑块的深浅变化最明显 —— 它正好是
      // 「反差够不够」的自检尺。
      g->setDrawColor(INK);
      fill(g, 4, y - kRowH + 3, cfg::kLcdWidth - 8, kRowH);
      g->setDrawColor(PAPER);
      text(g, 8, y, it.label);
      textRight(g, cfg::kLcdWidth - 8, y, val);
      g->setDrawColor(INK);
    } else {
      text(g, 8, y, it.label);
      textRight(g, cfg::kLcdWidth - 8, y, val);
    }
  }

  // 键位说明必须留在设置版面里：教程版面被替换掉了，而这里恰恰是最需要
  // 键位说明的地方（ADR-0003：不能出现「按了没反应」）。
  // 上电 1 秒限制与底部提示由 drawAbout 统一画，这里不重复画。
  text(g, 8, 250, "BOOT 短按  选择行");
  text(g, 8, 266, "BOOT 长按  切换");
}

void drawContrastHint(U8G2 *g, int idx, int total) {
  char buf[24];
  snprintf(buf, sizeof(buf), "对比度%d/%d", idx + 1, total);
  drawToast(g, buf);
}

void drawToast(U8G2 *g, const char *text) {
  g->setFont(F_CN);
  g->setDrawColor(PAPER);
  fill(g, cfg::kLcdWidth - 88, 2, 84, 20);
  g->setDrawColor(INK);
  textRight(g, cfg::kLcdWidth - 8, 18, text);
}

// 秒位：logisoso20 的 "31" 实测 24 px，居中在 x=100，墨迹顶 y=117、底 y=138。
// 取 80..120 / 116..140 这一块（40x24）只为把它连同边距一起擦干净。
void secondsRect(int *x, int *y, int *w, int *h) {
  *x = 80;
  *y = 116;
  *w = 40;
  *h = 24;
}

void drawSeconds(U8G2 *g, const AppState &s) {
  int rx, ry, rw, rh;
  secondsRect(&rx, &ry, &rw, &rh);
  g->setDrawColor(PAPER);
  fill(g, rx, ry, rw, rh);
  g->setDrawColor(INK);
  g->setFont(F_SEC);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d", s.second);
  textCenter(g, 100, 138, buf);
}

}  // namespace ui
