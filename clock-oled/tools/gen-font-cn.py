"""从系统黑体生成中文 16x16 点阵字模，输出 src/font_cn.h。

为什么只生成用到的字：
  一个 16x16 汉字 = 32 字节。全字库几千字要几百 KB Flash，没必要。
  本程序屏幕文案是固定的，把实际用到的三十来个字挑出来生成，
  总共约 1KB，且编译进固件后不占 RAM（放 flash，const）。

取模方向必须与 SSD1306 显存一致：
  1 字节 = 1 列中的 8 个垂直像素，bit0 = 页内最上一行。
  所以每个汉字是 2 页 × 16 列 = 32 字节，前 16 字节是上半页、后 16 是下半页。

用法：python tools/gen-font-cn.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---- 要生成的汉字（改这里就能加字，然后重跑本脚本）----
#
# 注意：只有**会显示在屏幕上**的字才需要在这里。
# 代码注释里的中文不需要字模，别加进来白占 Flash。
#
# 按用途分组，方便以后增删：
#   状态行：已 同 步 未 连 接 等 待 网 络
#   日期：  年 月 日
#   星期：  星 期 一 二 三 四 五 六 日
#   页脚：  信 号
#   备用：  时 分 秒 北 京 间 对
CHARS = (
    "已同步未连接等待网络"      # 状态行
    "年月日"                    # 日期
    "星期一二三四五六"          # 星期（"日" 已在日期组里，复用）
    "时分秒"                    # 时间后缀（备用）
    "信号北京间对"              # 页脚
)

FONT_PATH = r"C:\Windows\Fonts\simhei.ttf"  # 黑体：笔画均匀，点阵化后最清晰
SIZE = 16

OUT = os.path.join(os.path.dirname(__file__), "..", "src", "font_cn.h")


def glyph_bitmap(char, font):
    """把一个字渲染成 16x16 的黑白位图，返回 16 行、每行 16 个 bool。"""
    img = Image.new("1", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)
    # 用 anchor 居中：不同字的字形宽窄不一，居中后视觉最整齐
    draw.text((SIZE // 2, SIZE // 2), char, font=font, fill=1, anchor="mm")
    px = img.load()
    return [[bool(px[x, y]) for x in range(SIZE)] for y in range(SIZE)]


def to_bytes(rows):
    """把 16x16 位图转成 32 字节的 SSD1306 列格式。

    输出布局：前 16 字节 = 上半页（第 0~7 行），后 16 字节 = 下半页（第 8~15 行）。
    每个字节代表一列，bit0 是最上面那一行。
    """
    out = bytearray()
    for page in range(2):           # 上半页、下半页
        for x in range(SIZE):       # 16 列
            byte = 0
            for bit in range(8):    # 页内 8 行
                y = page * 8 + bit
                if rows[y][x]:
                    byte |= 1 << bit
            out.append(byte)
    return bytes(out)


def main():
    if not os.path.exists(FONT_PATH):
        sys.exit("找不到字体文件：{}".format(FONT_PATH))
    font = ImageFont.truetype(FONT_PATH, SIZE)

    # 去重但保持顺序，保证生成结果稳定（同样的输入永远得到同样的文件）
    seen = []
    for c in CHARS:
        if c not in seen:
            seen.append(c)

    lines = []
    lines.append("/**")
    lines.append(" * font_cn.h —— 16x16 中文点阵字模（自动生成，请勿手改）")
    lines.append(" *")
    lines.append(" * 由 tools/gen-font-cn.py 从 simhei.ttf 生成。")
    lines.append(" * 要增删汉字，改那个脚本里的 CHARS 再重跑，不要直接编辑本文件。")
    lines.append(" *")
    lines.append(" * 取模方向与 SSD1306 显存一致：1 字节 = 1 列 8 个垂直像素，")
    lines.append(" * bit0 = 页内最上一行。每个汉字 32 字节 = 上 16 列(页 A) + 下 16 列(页 B)。")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("// 汉字点阵：一个字 32 字节，前 16 字节上半页、后 16 字节下半页")
    lines.append("static const uint8_t kFontCn[][32] = {")
    for c in seen:
        data = to_bytes(glyph_bitmap(c, font))
        hexed = ", ".join("0x%02X" % b for b in data)
        lines.append("    {{{}}},  // {}".format(hexed, c))
    lines.append("};")
    lines.append("")
    lines.append("// 与 kFontCn 一一对应的汉字表（UTF-8），用于查表")
    lines.append('static const char kFontCnChars[] = "{}";'.format("".join(seen)))
    lines.append("")
    lines.append("static const uint16_t kFontCnCount = {};".format(len(seen)))
    lines.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("生成 {} 个汉字 -> {}".format(len(seen), os.path.normpath(OUT)))
    print("字表：" + "".join(seen))
    print("占用约 {} 字节 Flash".format(len(seen) * 32))


if __name__ == "__main__":
    main()
