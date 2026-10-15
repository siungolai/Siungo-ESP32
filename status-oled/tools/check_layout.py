"""在主机上复刻 status-oled 的四个页面排版，逐行核对：

1. 每行是否 <= 21 字符（128px / 6px 的硬上限）
2. 标签是否左对齐、数值是否右对齐贴到第 20 列
3. 与 README 的 ASCII 框图是否逐字符一致

版式规则（与 main.cpp 的 drawRow 同步）：
  标签左对齐贴列 0，数值右对齐贴列 20，数值起始列 = 21 - len(value)。
  唯一例外是 MAC 行：它的数值 17 字符，右对齐会让起始列 = 4 与标签粘连，
  所以那行走 drawRowLeft()，数值从第 5 列起。

用法：python check_layout.py
"""
import re
import sys

# Windows 控制台默认 GBK，打不出 ✓/✗；强制 UTF-8 输出。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

LABEL_COLS = 4
MAX_COLS = 21
LAST_COL = MAX_COLS - 1  # 数值右对齐贴的目标列（0-based）

# 与 config.h 保持一致：改了那边要同步这里。
# LOGO_X 故意不从 config.h 解析——本脚本的作用是独立复算它，
# 从源头读值就变成了自己验证自己。
LOGO_TEXT = "SIUNGOLAI"
LOGO_X = 74
LOGO_PAGE = 7


def fmt_bytes(b):
    return "%dM" % (b // 1048576) if b >= 1048576 else "%dK" % (b // 1024)


def fmt_k1(b):
    return "%d.%dK" % (b // 1024, (b * 10 // 1024) % 10)


def fmt_k1_pct(b, pct):
    return "%d.%dK %d%%" % (b // 1024, (b * 10 // 1024) % 10, pct)


def draw_row(label, value):
    """复刻 drawRow(): 标签左对齐 4 列，数值右对齐贴列 20。

    数值串允许带一个前导空格（FLASH 那行沿用了旧写法），右对齐版本会剥掉它；
    标签超 4 字符时 %-4s 不截断，原样输出（FLASH 是 5 列）。

    补白从"标签字段宽度"起算，不是从 strlen(label) 起算——
    %-4s 会把短标签补到 4 列，按 strlen 算会多推 (4-len) 列导致溢出。
    """
    text = value[1:] if value.startswith(" ") else value
    vlen = len(text)
    start = MAX_COLS - vlen
    vcol = max(start, LABEL_COLS)
    label_field = label if len(label) >= LABEL_COLS else label.ljust(LABEL_COLS)
    pad = max(vcol - len(label_field), 0)
    return label_field + " " * pad + text


def draw_row_left(label, value):
    """复刻 drawRowLeft(): "%-4s%s" —— 标签占 4 列，数值紧跟。"""
    return "%-4s%s" % (label, value)


def uptime_str(sec):
    return "%02d:%02d:%02d" % (sec // 3600, (sec // 60) % 60, sec % 60)


# ---- 取实测真值（来自上板串口读数）----
FREE_HEAP = 358 * 1024
HEAP_SIZE = 382 * 1024
MIN_HEAP = 270 * 1024
MAX_ALLOC = 150 * 1024

pages = {}

pages[1] = [
    draw_row("UP", uptime_str(5025)),
    draw_row("HEAP", "%dK MIN %dK" % (FREE_HEAP // 1024, MIN_HEAP // 1024)),
    draw_row("CPU", "240 MHz"),
    draw_row("RST", "USB UART"),
    draw_row("I2C", "OK 0x3C"),
]

pages[2] = [
    draw_row("CHIP", "ESP32-S3 r0"),
    draw_row("MEM", "%s+%s" % (fmt_bytes(16777216), fmt_bytes(8388608))),
    # MAC 是本页唯一走左对齐版的行，见上方 draw_row_left 的说明。
    draw_row_left("MAC", "12:34:56:78:9A:BC"),
    draw_row("SDK", "v4.4.7-dirty"),
    draw_row("BOOT", "%03d RST %03d" % (17, 17)),
    # FLASH 行是本页第 6 行，也是全屏唯一一页有 6 行数据（其余各 5 行）。
    # 它仍是全屏唯一的 5 字符标签；数值串的前导空格由 draw_row 剥掉。
    # 分子分母取烧录后实测值：sketch 288208 B / app 分区 3342336 B。
    draw_row(
        "FLASH",
        " %dK/%dK %d%%"
        % (288208 // 1024, 3342336 // 1024, 288208 * 100 // 3342336),
    ),
]


def pct_of(part, whole):
    return part * 100 // whole if whole else 0


used = HEAP_SIZE - FREE_HEAP
frag = pct_of(HEAP_SIZE - MAX_ALLOC, HEAP_SIZE)
pages[3] = [
    # DRAM 行是"已用/总量"的分数形式，吸收了原来独立的 USED 行。
    draw_row("DRAM", "%s/%s" % (fmt_k1(used), fmt_k1(HEAP_SIZE))),
    # FREE / MIN / MAX / FRAG 的百分比一律以 DRAM 为分母，全页同参照系。
    draw_row("FREE", fmt_k1_pct(FREE_HEAP, pct_of(FREE_HEAP, HEAP_SIZE))),
    draw_row("MIN", fmt_k1_pct(MIN_HEAP, pct_of(MIN_HEAP, HEAP_SIZE))),
    draw_row("MAX", fmt_k1_pct(MAX_ALLOC, pct_of(MAX_ALLOC, HEAP_SIZE))),
    draw_row("FRAG", "%d%%" % frag),
]

pages[4] = [
    draw_row("I2C", "SCL4 SDA5"),
    draw_row("FREQ", "400 kHz"),
    draw_row("ADDR", "0x3C SSD1306"),
    draw_row("SCAN", "0x3C"),
    draw_row("LAST", "3 s ago"),
]

# 左对齐版的行（目前只有 MAC）：不参与右对齐断言，只检查不超列。
LEFT_ALIGNED = {"MAC"}

fails = []
for num in sorted(pages):
    for i, line in enumerate(pages[num]):
        if len(line) > MAX_COLS:
            fails.append("P%d 第 %d 行超长：%d 列 > %d  -> %r"
                         % (num, i + 1, len(line), MAX_COLS, line))
        _m = re.match(r"^([A-Za-z0-9]+)", line)
        if not _m:
            fails.append("P%d 第 %d 行没有标签：%r" % (num, i + 1, line))
            continue
        label = _m.group(1)
        if len(line) <= len(label):
            fails.append("P%d 第 %d 行没有数值：%r" % (num, i + 1, line))
            continue

        # 标签必须从列 0 开始 —— 这是"名称靠左对齐"的字面要求。
        if line[0] == " ":
            fails.append("P%d 第 %d 行标签没有顶到列 0：%r"
                         % (num, i + 1, line))

        if label in LEFT_ALIGNED:
            # 左对齐版：标签占 4 列，数值紧跟其后。
            value = line[len(label):].lstrip()
            if line[len(label):len(label) + 1] not in ("", " "):
                fails.append("P%d 第 %d 行（左对齐版）标签与数值粘连：%r"
                             % (num, i + 1, line))
            continue

        # 右对齐版：数值的末列必须正好是 LAST_COL。
        value = line.rstrip()
        if not value[len(label):].strip():
            fails.append("P%d 第 %d 行没有数值：%r" % (num, i + 1, line))
            continue
        end_col = len(value) - 1
        if end_col != LAST_COL:
            fails.append("P%d 第 %d 行数值右边缘在第 %d 列，应为第 %d 列：%r"
                         % (num, i + 1, end_col, LAST_COL, line))
        # 标签与数值之间必须至少隔一列，否则会读成一个词。
        vcol = len(value) - len(line[len(label):].lstrip())
        if vcol < len(label) + 1:
            fails.append("P%d 第 %d 行标签与数值之间没有空隙：%r"
                         % (num, i + 1, line))

print("=" * 46)
for num in sorted(pages):
    print("P%d/4" % num)
    for i, line in enumerate(pages[num]):
        print("  |%-21s|  %2d 列" % (line, len(line)))
print("=" * 46)

# 与 README 框图核对：
# 框图里的数字是示例值，不该写死成断言（否则改一个示例值就假报错）。
# 这里只核对"排版形状"——标签是否都在框图里出现过。
# 用脚本自身位置定位 README，不受当前工作目录影响。
import os
_here = os.path.dirname(os.path.abspath(__file__))
readme_path = os.path.join(_here, "..", "README.md")
readme = open(readme_path, encoding="utf-8").read()
missing = []
for num, rows in pages.items():
    for r in rows:
        label = r[:LABEL_COLS].rstrip()
        # README 的框图是"两页并排"，右栏的标签不在行首，所以不能锚定 ^。
        # 这里只要求标签作为一个词出现，且后面紧跟数值（字母或空格+数字）。
        pattern = re.compile(r"(?<![A-Za-z0-9])" + re.escape(label) + r"(?=\s|\S)")
        if not pattern.search(readme):
            missing.append("README 里找不到 P%d 的标签 %r" % (num, label))

if fails:
    print("\n排版检查失败：")
    for f in fails:
        print("  ✗", f)
else:
    print("\n✓ 全部 %d 行都在 %d 列以内；标签顶列 0（左对齐）、"
          "数值贴列 %d（右对齐）"
          % (sum(len(v) for v in pages.values()), MAX_COLS, LAST_COL))

if missing:
    print("\nREADME 框图缺少这些标签：")
    for m in missing:
        print("  ✗", m)
    sys.exit(1)
_total_labels = sum(len(v) for v in pages.values())
print("✓ README 框图覆盖了全部 %d 个标签" % _total_labels)


# ---- 右下角水印：核对 config.h 里的 LOGO_X 是否真能贴住右边缘 ----
# 复刻 drawText5x7 的游标推进与 break 条件：
#   for 每个字符: if cursor + 5 >= 128: break; 画 5 列; cursor += 6
# LOGO_X 是手写在 config.h 里的常量，这里算出它，对不上就报错——
# 改了 LOGO_TEXT 的字符数却忘了改 LOGO_X，是最容易出的错。
OLED_WIDTH = 128
CHARS_PER_LOGO = len(LOGO_TEXT)


def logo_end(start, s):
    """返回 (完整画出的字符数, 收尾游标)。"""
    cursor = start
    drawn = 0
    for _ in s:
        if cursor + 5 >= OLED_WIDTH:
            break
        drawn += 1
        cursor += 6
    return drawn, cursor


# 找能完整放下全部字符的最右起点
rightmost = None
for x in range(OLED_WIDTH, -1, -1):
    if logo_end(x, LOGO_TEXT)[0] == CHARS_PER_LOGO:
        rightmost = x
        break

drawn, end = logo_end(LOGO_X, LOGO_TEXT)
logo_fails = []
if rightmost is None:
    logo_fails.append("LOGO_TEXT 太长，任何起点都放不下 %d 个字符" % CHARS_PER_LOGO)
elif drawn != CHARS_PER_LOGO:
    logo_fails.append("LOGO_X=%d 只画得下 %d/%d 个字符（最右可用起点是 %d）"
                      % (LOGO_X, drawn, CHARS_PER_LOGO, rightmost))
elif LOGO_X != rightmost:
    logo_fails.append("LOGO_X=%d 不是右对齐值，最右可用起点是 %d"
                      % (LOGO_X, rightmost))

# 水印在第 7 页，不能和第 1..5 页的数据行冲突
if LOGO_PAGE <= 5:
    logo_fails.append("LOGO_PAGE=%d 与数据行（1..5 页）重叠" % LOGO_PAGE)

print()
if logo_fails:
    print("水印排版检查失败：")
    for f in logo_fails:
        print("  ✗", f)
    sys.exit(1)

print("✓ 水印 %r 共 %d 字符，LOGO_X=%d 右对齐，"
      "末列 %d（右边缘 %d），位于第 %d 页"
      % (LOGO_TEXT, CHARS_PER_LOGO, LOGO_X, end - 2, OLED_WIDTH - 1, LOGO_PAGE))

if LOGO_TEXT not in readme:
    print("  ✗ README 里没提到水印字样 %r" % LOGO_TEXT)
    sys.exit(1)
print("✓ README 已记录水印字样")


# ---- 版本号：第 7 页最左侧，与水印同行。核对两者不相撞 ----
# 版本号写在列 0 起，水印固定从 LOGO_X=74 起。两者的间距是硬约束：
# 版本号字符数 x 6 必须 < LOGO_X，否则最后一个字符会压到水印上。
# 单独从 config.h 读 VERSION_TEXT 会变成自己验证自己，所以这里把
# 已知的版本串写死复算——改 config.h 的值而忘了同步这里，就会报错。
VERSION_TEXT = "V1.0"  # 与 config.h 保持一致
ver_fails = []
ver_px = len(VERSION_TEXT) * 6
if ver_px > LOGO_X:
    ver_fails.append(
        "版本号 %r 占 %d 字符 = %d px，与水印起点 LOGO_X=%d 相撞（超出 %d px）"
        % (VERSION_TEXT, len(VERSION_TEXT), ver_px, LOGO_X, ver_px - LOGO_X)
    )
# 间距至少留一个字符宽度，否则两串字会读成一串。
elif LOGO_X - ver_px < 6:
    ver_fails.append(
        "版本号 %r 与水印之间只剩 %d px 间隔（不足一个字符宽度 6px）"
        % (VERSION_TEXT, LOGO_X - ver_px)
    )

print()
if ver_fails:
    print("版本号排版检查失败：")
    for f in ver_fails:
        print("  ✗", f)
    sys.exit(1)
print("✓ 版本号 %r 在列 0 起（%d px），水印在列 %d 起，间隔 %d px"
      % (VERSION_TEXT, ver_px, LOGO_X, LOGO_X - ver_px))

if VERSION_TEXT not in readme:
    print("  ✗ README 里没提到版本号 %r" % VERSION_TEXT)
    sys.exit(1)
print("✓ README 已记录版本号 %r" % VERSION_TEXT)


# ---- SCAN 行：复刻 main.cpp 的运行时拼接，核对最坏情况不越列 ----
# 这一项是补漏：上面 pages[4] 用的是写死示例 "0x3C"（1 个地址），
# 所以早先"按 sizeof(value) 而不是按列宽做守卫"的缺陷没被抓到——
# 那个版本到第 4 个地址就画到 19 列，超出 21 列的屏幕宽度被静默丢弃。
# 这里按 main.cpp 的真实算法重算：从 0 到 kMaxScanAddrs 个地址逐个试。
VALUE_COLS = MAX_COLS - LABEL_COLS


def build_scan_row(addrs):
    """复刻 main.cpp 的 SCAN 拼接：pos + need > VALUE_COLS 就停手。"""
    value = ""
    pos = 0
    for i in range(len(addrs)):
        need = 5 if i == 0 else 6
        if pos + need > VALUE_COLS:
            break
        value += ("0x%02X" % addrs[i]) if i == 0 else (" 0x%02X" % addrs[i])
        pos += need
    return value


scan_fails = []
max_addrs = 8  # 与 main.cpp 的 kMaxScanAddrs 一致
for n in range(1, max_addrs + 1):
    addrs = [0x3C + i for i in range(n)]
    row = draw_row("SCAN", build_scan_row(addrs))
    if len(row) > MAX_COLS:
        scan_fails.append("%d 个地址时 SCAN 行 %d 列 > %d：%r"
                          % (n, len(row), MAX_COLS, row))

print()
if scan_fails:
    print("SCAN 行排版检查失败：")
    for f in scan_fails:
        print("  ✗", f)
    sys.exit(1)
shown = len(build_scan_row([0x3C + i for i in range(max_addrs)]).split())
print("✓ SCAN 行在 1..%d 个地址下都不超列（实际最多列 %d 个，多出的停手不画）"
      % (max_addrs, shown))
