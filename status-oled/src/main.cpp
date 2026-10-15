/**
 * main.cpp —— status-oled 主程序
 *
 * 单屏幕显示 ESP32 自身状态，四个页面由 GPIO7 上的轻触按钮循环切换，
 * 不自动轮播。数值每 1 秒刷新一次，翻页立即重绘不等节拍。
 *
 * 版面（每页 8 行中的第 1 行页眉 + 5 行数据，底部留 2 行空白）：
 *   P1 OVERVIEW  现在怎么样：uptime / heap / CPU / 复位原因 / 总线
 *   P2 IDENTITY  板级身份：芯片 / 内存 / MAC / SDK / 复位计数
 *   P3 HEAP      堆详情：总量 / 空闲 / 开机以来最低 / 最大可分配 / 已用
 *   P4 BUS       总线详情：引脚 / 频率 / 地址 / 扫描结果 / 距上次扫描
 *
 * 主循环范式照抄 uptime-oled/src/main.cpp：纯 millis() 非阻塞，
 * `lastRefresh += REFRESH_MS` 累加避免漂移；屏幕掉线时每 5 秒重试 begin()。
 *
 * 串口日志沿用 uptime-oled 的每秒一行格式，另追加按键与翻页事件行。
 */
#include <Arduino.h>
#include <Wire.h>
#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string.h>

#include "config.h"
#include "ssd1306_i2c.h"

// ------------------------------------------------------------------ 复位计数
// 必须用 RTC_NOINIT_ATTR（.rtc_noinit，NOLOAD）：软复位 / 看门狗复位后保留，
// 且启动时不会被 flash 映像里的初值覆盖。RTC_DATA_ATTR 不行——它落在
// .rtc.data，每次上电都按 flash 初值重载，计数永远归零。
// 详见 config.h 的注释与 sections.ld 里 .rtc.data / .rtc_noinit 两个 section。
// 代价：真拔电即丢；首次烧录后 RTC 是垃圾值，所以必须 magic 校验。
RTC_NOINIT_ATTR static uint32_t rtcMagic;
RTC_NOINIT_ATTR static uint32_t rtcBootCount;
RTC_NOINIT_ATTR static uint32_t rtcResetCount;

// ------------------------------------------------------------------ 全局状态
static Ssd1306I2c display(config::I2C_SCL_PIN, config::I2C_SDA_PIN,
                          config::OLED_ADDR_PRIMARY, config::I2C_HZ,
                          config::OLED_ADDR_ALT);

static uint8_t page = 0;          // 当前页 0..PAGE_COUNT-1
static uint8_t keyCount = 0;      // 按键累计次数，只出现在串口日志，不上屏
static uint32_t lastRefresh = 0;  // 上一次数值刷新的时刻
static uint32_t lastRescan = 0;   // 上一次总线扫描的时刻
static uint32_t lastRetry = 0;    // 上一次尝试重连屏幕的时刻
static uint32_t bootMs = 0;       // setup() 收尾的时刻，用于算 uptime

// 屏幕实际生效的 7 位地址：主地址无应答时驱动会回退到 0x3D，
// 这里记下来供 P1/P4 显示，避免屏上写 0x3C 而实际是 0x3D。
static uint8_t oledAddr = config::OLED_ADDR_PRIMARY;

// 总线扫描结果：最多记 8 个有应答的地址，够本项目单屏场景。
static const uint8_t kMaxScanAddrs = 8;
static uint8_t scanAddrs[kMaxScanAddrs];
static uint8_t scanCount = 0;
static uint32_t lastScanAgeSec = 0;  // 距上次扫描多少秒

// 开机时读一次的常量，避免每帧查 eFuse / flash ID。
static char gChipModel[24];
static uint8_t gChipRevision = 0;
static uint32_t gFlashBytes = 0;
static uint32_t gPsramBytes = 0;
static uint64_t gEfuseMac = 0;
static const char* gSdkVersion = "";
// 当前运行分区（app0）的容量，用于算固件占用率。开机读一次。
static uint32_t gAppPartitionBytes = 0;

// 本程序自己的 uptime，不从 millis() 直接除，避免把 setup() 里的等待算进去。
static uint32_t uptimeSec() {
  return (millis() - bootMs) / 1000UL;
}

// ------------------------------------------------------------------ 小工具
// 把字节数格式化成人类可读的 K/M，例如 16777216 -> "16M"、327680 -> "320K"。
// 整数除，不带小数：P2 身份页要在一行里塞下 Flash + PSRAM 两个数。
static void fmtBytes(uint32_t bytes, char* out, size_t outLen) {
  if (bytes >= 1048576UL) {
    snprintf(out, outLen, "%luM",
             static_cast<unsigned long>(bytes / 1048576UL));
  } else {
    snprintf(out, outLen, "%luK",
             static_cast<unsigned long>(bytes / 1024UL));
  }
}

// 一位小数的 K：bytes/1024 的十分位 = (bytes*10/1024) % 10。
// 用整数算不引入浮点，AVR 上省事，ESP32 上主要是行为确定。
static void fmtK1(uint32_t bytes, char* out, size_t outLen) {
  const uint32_t whole = bytes / 1024UL;
  const uint32_t frac = (bytes * 10UL / 1024UL) % 10UL;
  snprintf(out, outLen, "%lu.%luK", static_cast<unsigned long>(whole),
           static_cast<unsigned long>(frac));
}

// 一行同时给出 K 值和占用百分比，例如 "283.7K 13%"。
// 单独一个函数而不是先 fmtK1 再拼 %s：两步拼接时 gcc 看不到 fmtK1 的
// 输出上限，会报 -Wformat-truncation；一次成型它就算得出最坏长度。
static void fmtK1Pct(uint32_t bytes, uint32_t pct, char* out, size_t outLen) {
  const uint32_t whole = bytes / 1024UL;
  const uint32_t frac = (bytes * 10UL / 1024UL) % 10UL;
  snprintf(out, outLen, "%lu.%luK %lu%%", static_cast<unsigned long>(whole),
           static_cast<unsigned long>(frac),
           static_cast<unsigned long>(pct));
}

// 整数百分比，whole 为 0 时返回 0（避免除零）。
static uint32_t pctOf(uint32_t part, uint32_t whole) {
  return (whole > 0) ? (part * 100UL / whole) : 0;
}

// 把 esp_reset_reason_t 映射成屏上用的短名，最长 11 字符（DEEP SLEEP / SW RESTART），
// 保证塞得进 17 字符的数值列。原始枚举名照旧进串口日志，方便 grep。
static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT PIN";
    case ESP_RST_SW: return "SW RESTART";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "?";
  }
}

// ------------------------------------------------------------------ USB 复位补判
// 本机实测的坑（决策依据见 README「设计取舍」节）：ESP32-S3 上凡是用板载原生 USB 复位
// （esptool 烧录后 hard reset、串口工具拉 DTR/RTS），rom 层的复位原因是
// RESET_REASON_CORE_USB_UART = 0x15 或 RESET_REASON_CORE_USB_JTAG = 0x16。
// 但 IDF 的 esp_reset_reason_init() 内部 switch 只映射到 0x12 为止，
// 0x13~0x17 全部落进 default 分支变成 ESP_RST_UNKNOWN。
// 于是屏上永远显示 UNKNOWN，看着像固件坏了，其实只是这张映射表缺了几行。
// 这里直接读 rom 的原始原因，把 USB 那两种补回来。
static const char* resetReasonNameRaw() {
  // cpu_no = 0：读 PRO CPU（ESP32-S3 主核）的复位原因。
  const int raw = static_cast<int>(esp_rom_get_reset_reason(0));
  switch (raw) {
    case 0x15: return "USB UART";
    case 0x16: return "USB JTAG";
    case 0x13: return "CLK GLITCH";
    case 0x14: return "EFUSE CRC";
    case 0x17: return "PWR GLITCH";
    default:
      // 其余交给 IDF 的映射表（它能正确处理的那些）。
      return resetReasonName(esp_reset_reason());
  }
}

// ------------------------------------------------------------------ 复位计数
// 返回本次上电的复位原因，同时更新 RTC 计数。
static esp_reset_reason_t beginResetCounters() {
  if (rtcMagic != config::RTC_MAGIC) {
    // 首次上电或 RTC 是垃圾值：从零开始。
    rtcMagic = config::RTC_MAGIC;
    rtcBootCount = 0;
    rtcResetCount = 0;
  }
  ++rtcBootCount;

  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_POWERON) {
    ++rtcResetCount;
  }
  return reason;
}

// ------------------------------------------------------------------ 总线扫描
// 扫 0x08..0x77，跑在总线自身速率上，不切时钟。
// 从机不在线时每次 endTransmission 会等满 50ms 超时，112 个地址最坏约 5.6 秒，
// 所以只在启动时和之后每 RESCAN_INTERVAL_MS 做一次，且不触发重绘。
static void scanBus() {
  scanCount = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (scanCount < kMaxScanAddrs) {
        scanAddrs[scanCount++] = addr;
      }
    }
  }
  lastRescan = millis();
  lastScanAgeSec = 0;
}

// ------------------------------------------------------------------ 绘制
// 版式：标签左对齐贴列 0，数值右对齐贴列 20（MAX_COLS-1）。
//
// 数值起始列 = MAX_COLS - len(value)，所以长数值左移、短数值右移，
// 四页翻页时所有数值的右边缘对齐成一条直线。
//
// 特例：MAC 行是全屏唯一的 21 字符行（数值本身 17 字符），右对齐会让
// 起始列 = 21-17 = 4，正好紧贴标签末尾读成 "MAC12:34:56:78:9A:BC"。
// 所以 MAC 用 drawRowLeft()，数值从第 5 列起，这一行刻意不对齐。
static void drawRow(uint8_t row, const char* label, const char* value) {
  // 缓冲区按最坏情况给足：标签 %-4s 最多输出 5 列（超 4 字符的标签不截断）
  // + 补白最多 17 列 + 数值最多 17 列 + NUL。
  char line[64];
  const char* text = value;
  // 数值串允许自带一个前导空格（" %luK/..." 这种），它只用于左对齐版式的
  // 留白。右对齐下这格会挤掉别的列，所以这里剥掉它再算宽度——
  // 否则 FLASH 行 = 5(标签) + 3(补白) + 14(含前导空格的数值) = 22 列，溢出。
  if (*text == ' ') {
    ++text;
  }
  const size_t vlen = strlen(text);
  // 右对齐起始列；数值超长时退化为左对齐，不越界。
  const int start = static_cast<int>(config::MAX_COLS) - static_cast<int>(vlen);
  const int vcol = (start > static_cast<int>(config::LABEL_COLS))
                       ? start
                       : static_cast<int>(config::LABEL_COLS);
  // 补白列数 = 数值起始列 - 标签字段实际占的列数。
  // 标签字段由 "%-4s" 输出，宽度取 max(strlen(label), 4)：短标签被补到 4 列，
  // 超 4 字符的标签（FLASH）原样输出不截断。所以这里必须按字段宽度算，
  // 不能按 strlen(label) 算——按后者会多推 (4 - strlen(label)) 列，短标签行溢出。
  // 另外 %*s 吃的是"这个字段自己的输出宽度"，它后面还跟着一个 %s（数值），
  // 所以宽度只能是补白数本身，绝不能加上 vlen。
  const size_t labelCols = strlen(label);
  const int labelField = (labelCols < config::LABEL_COLS) ? config::LABEL_COLS
                                                          : static_cast<int>(labelCols);
  const int pad = vcol - labelField;
  snprintf(line, sizeof(line), "%-4s%*s%s", label, (pad > 0) ? pad : 0, "",
           text);
  display.drawText5x7(0, row, line);
}

// 左对齐版本，供 MAC 这种"右对齐会粘标签"的长数值行使用。
static void drawRowLeft(uint8_t row, const char* label, const char* value) {
  char line[64];
  snprintf(line, sizeof(line), "%-4s%s", label, value);
  display.drawText5x7(0, row, line);
}

static void drawHeader() {
  static const char* const kPageNames[config::PAGE_COUNT] = {
      "OVERVIEW", "IDENTITY", "HEAP", "BUS"};
  char line[32];
  snprintf(line, sizeof(line), "P%u/%u %s", page + 1, config::PAGE_COUNT,
           kPageNames[page]);
  display.drawText5x7(0, config::ROW_PAGE, line);
}

// 右下角水印 + 左下角版本号。第 7 页（底部 8 像素）四页都不放数据，整行空闲。
// 复用普通 5x7 字模：左端写版本号（列 0 起），右端右对齐写 SIUNGOLAI（列 74 起）。
// 曾试过只取字模低 4 位的"半高"画法，但 5x7 字模的辨识特征集中在下半部，
// 砍掉后 O 与 G、S 与 C、U 与 N 会互相混淆，故放弃。
static void drawLogo() {
  display.drawText5x7(0, config::ROW_LOGO, config::VERSION_TEXT);
  display.drawText5x7(config::LOGO_X, config::ROW_LOGO, config::LOGO_TEXT);
}

static void renderPage() {
  display.clear();
  drawHeader();
  drawLogo();

  char value[32];

  switch (page) {
    case 0: {
      // ---- P1 总览：现在怎么样 ----
      const uint32_t up = uptimeSec();
      snprintf(value, sizeof(value), "%02lu:%02lu:%02lu",
               static_cast<unsigned long>(up / 3600UL),
               static_cast<unsigned long>((up / 60UL) % 60UL),
               static_cast<unsigned long>(up % 60UL));
      drawRow(config::ROW_FIRST + 0, "UP", value);

      snprintf(value, sizeof(value), "%luK MIN %luK",
               static_cast<unsigned long>(ESP.getFreeHeap() / 1024UL),
               static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024UL));
      drawRow(config::ROW_FIRST + 1, "HEAP", value);

      snprintf(value, sizeof(value), "%lu MHz",
               static_cast<unsigned long>(getCpuFrequencyMhz()));
      drawRow(config::ROW_FIRST + 2, "CPU", value);

      drawRow(config::ROW_FIRST + 3, "RST", resetReasonNameRaw());

      // 总线态用屏幕自己的应答状态：它不在线才是真·掉屏。
      if (display.ready()) {
        snprintf(value, sizeof(value), "OK 0x%02X",
                 static_cast<unsigned int>(oledAddr));
      } else {
        snprintf(value, sizeof(value), "ERR");
      }
      drawRow(config::ROW_FIRST + 4, "I2C", value);
      break;
    }

    case 1: {
      // ---- P2 身份：板级身份 + 复位计数 ----
      snprintf(value, sizeof(value), "%s r%u", gChipModel, gChipRevision);
      drawRow(config::ROW_FIRST + 0, "CHIP", value);

      char flashStr[8];
      char psramStr[8];
      fmtBytes(gFlashBytes, flashStr, sizeof(flashStr));
      fmtBytes(gPsramBytes, psramStr, sizeof(psramStr));
      snprintf(value, sizeof(value), "%s+%s", flashStr, psramStr);
      drawRow(config::ROW_FIRST + 1, "MEM", value);

      const uint8_t* m = reinterpret_cast<const uint8_t*>(&gEfuseMac);
      snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1],
               m[2], m[3], m[4], m[5]);
      // 这行刻意用左对齐版：MAC 数值 17 字符，右对齐会紧贴标签（见 drawRow 注释）。
      drawRowLeft(config::ROW_FIRST + 2, "MAC", value);

      // SDK 版本可能带 git hash，截断到 17 字符保证不溢出。
      char sdk[18];
      strncpy(sdk, gSdkVersion, sizeof(sdk) - 1);
      sdk[sizeof(sdk) - 1] = '\0';
      drawRow(config::ROW_FIRST + 3, "SDK", sdk);

      snprintf(value, sizeof(value), "%03lu RST %03lu",
               static_cast<unsigned long>(rtcBootCount),
               static_cast<unsigned long>(rtcResetCount));
      drawRow(config::ROW_FIRST + 4, "BOOT", value);

      // 固件占用率：当前固件大小 / app 分区大小。
      // 这是个恒定值（编译期就定了，运行期不会变），摆在这里是为了
      // "这个固件把 Flash 用了多少"有个直观数字——OOM 之前先撞的是它。
      // 分子分母都给出来（281K/3264K 8%），只给百分比看不出基数。
      // 注意分母口径是 app0 分区，不是芯片的 16MB：
      // 用芯片容量当分母会得到 1.7% 这种毫无意义的数。
      if (gAppPartitionBytes > 0) {
        const uint32_t fw = ESP.getSketchSize();
        const uint32_t pctFw = fw * 100UL / gAppPartitionBytes;
        // 开头的空格是刻意的：FLASH 是 5 字符标签，占满 0-4 列，
        // 不留这一格数值就会紧贴标签（FLASH281K...），与 4 字符行不一致。
        snprintf(value, sizeof(value), " %luK/%luK %lu%%",
                 static_cast<unsigned long>(fw / 1024UL),
                 static_cast<unsigned long>(gAppPartitionBytes / 1024UL),
                 static_cast<unsigned long>(pctFw));
      } else {
        snprintf(value, sizeof(value), "?");
      }
      drawRow(config::ROW_FIRST + 5, "FLASH", value);
      break;
    }

    case 2: {
      // ---- P3 堆详情：单位 K，一位小数 ----
      const uint32_t total = ESP.getHeapSize();
      const uint32_t freeH = ESP.getFreeHeap();
      const uint32_t minH = ESP.getMinFreeHeap();
      const uint32_t maxH = ESP.getMaxAllocHeap();
      const uint32_t used = (total > freeH) ? (total - freeH) : 0;

      // DRAM：已用 / 总量，同一行给出分子分母（原独立的 USED 行被吸收）。
      // 分母恒为 382K，分子每秒微跳——这是唯一的动态项。
      char usedStr[12];
      char totalStr[12];
      fmtK1(used, usedStr, sizeof(usedStr));
      fmtK1(total, totalStr, sizeof(totalStr));
      snprintf(value, sizeof(value), "%s/%s", usedStr, totalStr);
      drawRow(config::ROW_FIRST + 0, "DRAM", value);

      // FREE / MIN / MAX 的百分比一律以 DRAM 为分母，全页同参照系。
      fmtK1Pct(freeH, pctOf(freeH, total), value, sizeof(value));
      drawRow(config::ROW_FIRST + 1, "FREE", value);

      fmtK1Pct(minH, pctOf(minH, total), value, sizeof(value));
      drawRow(config::ROW_FIRST + 2, "MIN", value);

      fmtK1Pct(maxH, pctOf(maxH, total), value, sizeof(value));
      drawRow(config::ROW_FIRST + 3, "MAX", value);

      // FRAG：碎片率 = (DRAM - MAX) / DRAM。
      // 分母取 DRAM 而非 FREE，是为了与前四行同参照系，一行行读下来不换基准。
      // 语义是"不能被单个大块覆盖的占比"：FREE 里刨去 USED 才是真正可用的，
      // 而 MAX 是其中最大的连续段，两者差距越大越碎。
      const uint32_t frag = pctOf(total - maxH, total);
      snprintf(value, sizeof(value), "%lu%%",
               static_cast<unsigned long>(frag));
      drawRow(config::ROW_FIRST + 4, "FRAG", value);
      break;
    }

    case 3:
    default: {
      // ---- P4 总线详情 ----
      snprintf(value, sizeof(value), "SCL%u SDA%u", config::I2C_SCL_PIN,
               config::I2C_SDA_PIN);
      drawRow(config::ROW_FIRST + 0, "I2C", value);

      snprintf(value, sizeof(value), "%lu kHz",
               static_cast<unsigned long>(config::I2C_HZ / 1000UL));
      drawRow(config::ROW_FIRST + 1, "FREQ", value);

      if (display.ready()) {
        snprintf(value, sizeof(value), "0x%02X SSD1306",
                 static_cast<unsigned int>(oledAddr));
      } else {
        snprintf(value, sizeof(value), "no reply");
      }
      drawRow(config::ROW_FIRST + 2, "ADDR", value);

      if (scanCount == 0) {
        snprintf(value, sizeof(value), "none");
      } else {
        // 把所有有应答的地址列出来，但受数值列宽度限制：VALUE_COLS = 17。
        // 每个地址占 5 字符（"0x3C"），两个之间 1 个空格，所以第 i 个之后
        // 总长是 5i + (i-1)。守卫按列宽算而不是按 sizeof(value)，否则
        // 第 4 个地址起就超出 21 列的屏幕宽度，被 drawText5x7 静默丢掉。
        value[0] = '\0';
        size_t pos = 0;
        for (uint8_t i = 0; i < scanCount; ++i) {
          const size_t need = static_cast<size_t>((i == 0) ? 5 : 6);
          if (pos + need > config::VALUE_COLS) {
            break;  // 放不下就停手，宁可少列一个也不画半截
          }
          pos += static_cast<size_t>(
              snprintf(value + pos, sizeof(value) - pos, "%s0x%02X",
                       (i == 0) ? "" : " ",
                       static_cast<unsigned int>(scanAddrs[i])));
        }
      }
      drawRow(config::ROW_FIRST + 3, "SCAN", value);

      snprintf(value, sizeof(value), "%lu s ago",
               static_cast<unsigned long>(lastScanAgeSec));
      drawRow(config::ROW_FIRST + 4, "LAST", value);
      break;
    }
  }

  display.flush();
}

// ------------------------------------------------------------------ 按钮
// INPUT_PULLUP + 按下接地（低有效）。30ms 软件消抖，状态机不阻塞。
// 每按一次翻一页，第 4 页再按回第 1 页；按下立即重绘，不等 1 秒节拍。
static void pollButton() {
  static bool lastStable = true;  // true = 松开（内部上拉默认高）
  static bool lastRaw = true;
  static uint32_t lastChange = 0;

  const bool raw = (digitalRead(config::PIN_BUTTON_PAGE) == LOW);
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = millis();
  }

  if (millis() - lastChange < config::BUTTON_DEBOUNCE_MS) {
    return;  // 抖动窗口内不表态
  }
  if (raw == lastStable) {
    return;  // 没有新的稳定电平
  }
  lastStable = raw;

  if (!raw) {
    return;  // 松开沿不翻页
  }

  // 按下沿：翻页 + 立即重绘
  ++keyCount;
  page = static_cast<uint8_t>((page + 1) % config::PAGE_COUNT);
  Serial.printf("[btn] pressed: page -> P%u/%u  (K=%u)\n", page + 1,
                config::PAGE_COUNT, static_cast<unsigned>(keyCount));
  renderPage();
}

// ------------------------------------------------------------------ setup
void setup() {  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.printf("=== status-oled %s ===\n", config::VERSION_TEXT);

  // 按钮脚要在读它之前配好
  pinMode(config::PIN_BUTTON_PAGE,
          config::BUTTON_PULLUP ? INPUT_PULLUP : INPUT);

  // 板级常量：读一次存起来，避免每帧查 eFuse / flash ID
  strncpy(gChipModel, ESP.getChipModel(), sizeof(gChipModel) - 1);
  gChipModel[sizeof(gChipModel) - 1] = '\0';
  gChipRevision = ESP.getChipRevision();
  gFlashBytes = ESP.getFlashChipSize();
  gPsramBytes = ESP.getPsramSize();
  gEfuseMac = ESP.getEfuseMac();
  gSdkVersion = ESP.getSdkVersion();

  // app 分区容量：固件占用率的分母。用当前运行分区而不是写死 3342336，
  // 这样以后换分区表（比如 OTA 双槽变单槽）屏上会自动跟着变。
  {
    const esp_partition_t* running = esp_ota_get_running_partition();
    gAppPartitionBytes = running ? running->size : 0;
  }

  const esp_reset_reason_t reason = beginResetCounters();

  const uint8_t* m = reinterpret_cast<const uint8_t*>(&gEfuseMac);
  Serial.printf("chip %s rev%u  flash %lu bytes  psram %lu bytes\n",
                gChipModel, gChipRevision,
                static_cast<unsigned long>(gFlashBytes),
                static_cast<unsigned long>(gPsramBytes));
  Serial.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3],
                m[4], m[5]);
  Serial.printf("SDK %s\n", gSdkVersion);
  // 固件占用率的分母与分子都报出来，方便不上手就能核对屏上 FLASH 行。
  Serial.printf("app partition %lu bytes  sketch %lu bytes\n",
                static_cast<unsigned long>(gAppPartitionBytes),
                static_cast<unsigned long>(ESP.getSketchSize()));
  Serial.printf("reset reason %d (%s)  boot#%lu  rst#%lu\n",
                static_cast<int>(reason), resetReasonNameRaw(),
                static_cast<unsigned long>(rtcBootCount),
                static_cast<unsigned long>(rtcResetCount));

  // 上电时按住按钮 >= 1.5s 才跑字模方向自检（Q20-C）：
  // 画两条相隔 14 行的 1x4 参照竖条，停 6 秒后自动清屏。
  // 常态是上电即显，自检是诊断动作，所以只在按住时发生。
  //
  // 注意顺序：这段等待必须放在 display.begin() 之后。放在前面的话，
  // 屏幕还没初始化就先卡在 delay(10) 循环里最多 1.5 秒；而且下面
  // fontDirProbe() 会阻塞 6 秒，屏幕若没上线就是白白空等 6 秒、
  // 屏上什么都不会画。所以是否自检要同时看"按住了"和"屏在线"。
  bool fontProbe = false;

  if (!display.begin()) {
    // 不卡死：屏幕后面会每 5 秒重试，串口继续输出，方便插拔定位问题。
    Serial.println("OLED 0x3C/0x3D no reply, keep retrying in background");
    Serial.println("check: SCL=GPIO4 SDA=GPIO5, VCC=3V3, wires not swapped");
    // 首次重试要从"现在"起算 5 秒，不能等 millis() 自己爬到 RETRY_MS。
    lastRetry = millis();
  } else {
    oledAddr = display.address();
    Serial.printf("OLED online at 0x%02X\n", oledAddr);

    // 屏已就绪，这时才判要不要跑自检：按住到 1.5s 即认定用户想要自检。
    const uint32_t heldStart = millis();
    while (digitalRead(config::PIN_BUTTON_PAGE) == LOW &&
           millis() - heldStart < config::FONT_DIR_HOLD_MS) {
      delay(10);
    }
    fontProbe = (digitalRead(config::PIN_BUTTON_PAGE) == LOW);
  }

  scanBus();

  bootMs = millis();
  lastRefresh = bootMs;
  renderPage();

  if (fontProbe) {
    Serial.println("[font] direction probe: two 1x4 bars, 14 rows apart");
    display.fontDirProbe();
    renderPage();  // 自检清屏后把正常页面画回来
  }

  Serial.printf("startup done, uptime base %lu ms\n",
                static_cast<unsigned long>(bootMs));
}

// ------------------------------------------------------------------ loop
void loop() {
  pollButton();

  const uint32_t now = millis();

  // 屏幕掉线：每 5 秒重试一次 begin()（内部会从主地址重新探测 0x3C / 0x3D）。
  // 注意：掉线时**不** return——总线扫描是独立的诊断手段，不依赖 OLED 在线，
  // 若在这里提前返回，10 秒重扫与 lastScanAgeSec 更新都会被跳过，
  // 屏上 LAST 会永久冻结在掉线前那一刻，反而丢掉了排查掉线最需要的现场。
  if (!display.ready()) {
    if (now - lastRetry >= config::RETRY_MS) {
      lastRetry = now;
      Serial.println("[oled] offline, retrying begin()");
      if (display.begin()) {
        oledAddr = display.address();
        Serial.printf("[oled] back online at 0x%02X\n", oledAddr);
        // 重连后把刷新基准对齐到当下：掉线期间 lastRefresh 会落后很多，
        // 累加式一次只加 REFRESH_MS，不重置就会连续几十轮全速重绘 + flush。
        lastRefresh = now;
        renderPage();
      }
    }
    // 掉线期间只保留"重试 + 重扫"两件事，不跑到下面的渲染 / 串口每秒行。
    if (now - lastRescan >= config::RESCAN_INTERVAL_MS) {
      scanBus();
      Serial.printf("[bus] rescan: %u addr(s)\n", scanCount);
    }
    // 同上面的理由：scanBus() 会把 lastRescan 推到扫描结束后，
    // 这里的 now 已经过期，必须重新采样才不会回绕。
    lastScanAgeSec = (millis() - lastRescan) / 1000UL;
    return;
  }

  if (now - lastRefresh < config::REFRESH_MS) {
    return;
  }
  lastRefresh += config::REFRESH_MS;

  // 到 10 秒就静默重扫一次总线：只更新数据，不触发重绘
  if (now - lastRescan >= config::RESCAN_INTERVAL_MS) {
    scanBus();
    Serial.printf("[bus] rescan: %u addr(s)\n", scanCount);
  }
  // 用当下的 millis() 而不是上面那个 now：scanBus() 内部会把 lastRescan
  // 写成扫描结束时刻，比 now 晚（扫描本身最长可达数秒）。用 now 减它会
  // 发生 uint32_t 回绕，屏上 LAST 会显示约 4294966 s ago 这种垃圾值。
  lastScanAgeSec = (millis() - lastRescan) / 1000UL;

  renderPage();

  // 串口与屏上内容保持同源：屏上画什么，这里就报什么
  const uint32_t up = uptimeSec();
  Serial.printf(
      "P%u  UP %02lu:%02lu:%02lu  HEAP %lu/%luK  CPU %luMHz  RST %s\n", page + 1,
      static_cast<unsigned long>(up / 3600UL),
      static_cast<unsigned long>((up / 60UL) % 60UL),
      static_cast<unsigned long>(up % 60UL),
      static_cast<unsigned long>(ESP.getFreeHeap() / 1024UL),
      static_cast<unsigned long>(ESP.getHeapSize() / 1024UL),
      static_cast<unsigned long>(getCpuFrequencyMhz()),
      resetReasonNameRaw());

  // P2 的两个计数只画在屏上，串口每分钟补报一次，方便不上手就能核对
  // RTC_NOINIT 是否真的跨复位保留（ADR 0003 的验证点）。
  // K= 是按键累计次数，页眉已不再显示，只在这里留个计数。
  if (up % 60UL == 0UL) {
    Serial.printf("      boot#%lu rst#%lu K=%u\n",
                  static_cast<unsigned long>(rtcBootCount),
                  static_cast<unsigned long>(rtcResetCount),
                  static_cast<unsigned>(keyCount));
  }
}
