// rlcd-workbench / 界面层实现（M1）
//
// 坐标系：400 宽 x 300 高，左上角原点。
//   页首 0..24，分隔线 24；内容 25..277；分隔线 277；页尾 278..299。
//   两栏等宽：左 0..200，右 200..400，竖分隔线画在 x=200。

#include "ui.h"

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

void hline(U8G2 *g, int x, int y, int w) { g->drawHLine(x, y, w); }
void vline(U8G2 *g, int x, int y, int h) { g->drawVLine(x, y, h); }
void box(U8G2 *g, int x, int y, int w, int h) { g->drawFrame(x, y, w, h); }
void fill(U8G2 *g, int x, int y, int w, int h) { g->drawBox(x, y, w, h); }

// 虚线框（宠物插件占位）
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

// 电池：17x9 外框 + 右侧 2 px 凸点，内部留 1 px 白边
void battery(U8G2 *g, int x, int y, int pct) {
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

  // 右：待办未完成数 + 电池 + WiFi，从右边缘往左排
  g->setFont(F_CN);
  int x = cfg::kMarginR;
  wifi(g, x - 13, 8, s.wifi);
  x -= 13 + 6;
  battery(g, x - 19, 8, s.batteryPct);
  x -= 19 + 6;
  int undone = s.todoTotal - s.todoDone;
  if (undone < 0) undone = 0;
  snprintf(buf, sizeof(buf), "待办%d", undone);
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
  if (s.panel != 1) {
    return;
  }
  char mid[24];
  if (footerNote(s, mid, sizeof(mid))) {
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

  if (s.todoTotal == 0) {
    text(g, cfg::kCalLeft, cfg::kTodoTop + 11, "无待办");
    return;
  }

  for (int i = 0; i < cfg::kTodoSlots; ++i) {
    const int y0 = cfg::kTodoTop + i * cfg::kTodoPitch;
    // 勾选框 7x7
    const bool done = i < s.todoDone;
    box(g, cfg::kCalLeft, y0 + 1, 7, 7);
    if (done) fill(g, cfg::kCalLeft + 2, y0 + 3, 3, 3);

    text(g, cfg::kCalLeft + 12, y0 + 10, s.todos[i].text);
    if (s.todos[i].due[0] != '\0') {
      textRight(g, cfg::kMarginR, y0 + 10, s.todos[i].due);
    }
  }
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

  // 宠物插件占位（84x107），只画虚线框
  dashedBox(g, 4, 159, 84, 107);
  g->setFont(F_CN);
  textCenter(g, 4 + 42, 196, "宠物");
  textCenter(g, 4 + 42, 212, "占位");

  // 温湿度右对齐到 196，只有数字和单位，无中文标签
  //
  // 注意左边那个宠物占位框：它的右边缘在 x=88，温度串一旦超过 104 px 就会压上去。
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
  const char *kRows[] = {
      "目标板  Waveshare ESP32-S3-RLCD-4.2",
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
  const char *kGestures[] = {
      "KEY  短按  下一页 / 下一面板",
      "KEY  长按  回主页并清状态",
      "KEY  双击  关于页（本页）",
      "BOOT 短按  立即拉数据并重绘",
      "BOOT 长按  对比度 5 档循环",
      "BOOT 双击  预留，未绑定",
  };
  for (const char *row : kGestures) {
    text(g, 8, y, row);
    y += 16;
  }

  textCenter(g, cfg::kLcdWidth / 2, 290, "无背光，请放在有光处");
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
