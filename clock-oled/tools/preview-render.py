"""把 font_cn.h / font5x7.h 的点阵还原成一张 PNG，用来预览屏幕效果。

为什么需要这个：
  串口日志只能证明"程序在跑"，证明不了"字画对了"。
  字模取模方向错一个 bit，屏幕上就是一团乱码，而日志完全正常。
  所以这里用和固件完全相同的数据源、完全相同的贴图规则，
  在电脑上把那一帧重画出来，先肉眼确认，再上真机。

用法：python tools/preview-render.py
输出：tools/preview.png
"""
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

W, H = 128, 64
SCALE = 5  # 放大倍数，方便肉眼看


def parse_font_cn(path):
    """解析 font_cn.h，返回 {汉字: 32 字节}。"""
    text = open(path, encoding="utf-8").read()
    glyphs = {}
    # 每行形如：  {0x00, 0x01, ...},  // 已
    for m in re.finditer(r"\{([^}]+)\},\s*//\s*(\S)", text):
        data = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        if len(data) == 32:
            glyphs[m.group(2)] = data
    return glyphs


def parse_font5x7(path):
    """解析 font5x7.h，返回 {ASCII字符: 5 字节}。

    字库内容形如 {0x00,0x00,...}, 每行 95 个字符里的一个，
    顺序就是 ASCII 32..126。
    """
    text = open(path, encoding="utf-8").read()
    glyphs = {}
    rows = re.findall(r"\{([^}]+)\}", text)
    idx = 0
    for r in rows:
        data = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", r))
        if len(data) == 5:
            ch = chr(32 + idx)
            glyphs[ch] = data
            idx += 1
    return glyphs


class Screen:
    """按 SSD1306 的显存布局复刻一块 128x64 画布。

    显存：8 页，每页 128 字节；1 字节 = 1 列里 8 个垂直像素，bit0 在最上面。
    """

    def __init__(self):
        self.buf = bytearray(W * 8)  # 页优先：buf[page*128 + x]

    def clear(self):
        self.buf = bytearray(W * 8)

    def _row(self, page):
        return page * W

    def draw_ascii(self, x, page, s, font):
        if page >= 8:
            return x
        for ch in s:
            g = font.get(ch)
            if g is None:
                g = font.get("?")
            for i in range(5):
                if x + i < W:
                    self.buf[self._row(page) + x + i] = g[i]
            if x + 5 < W:
                self.buf[self._row(page) + x + 5] = 0x00  # 字间距
            x += 6
        return x

    def draw_ascii_2x(self, x, page, s, font):
        """2 倍放大：与固件 drawText5x7Scaled2x 同样的展开规则。"""
        if page + 1 >= 8:
            return x
        for ch in s:
            g = font.get(ch) or font.get("?")
            up = self._row(page)
            lo = self._row(page + 1)
            for i in range(5):
                col = g[i]
                # 源列 i -> 目标列 2i, 2i+1
                for dx in (0, 1):
                    tx = x + i * 2 + dx
                    if tx >= W:
                        continue
                    low = 0
                    high = 0
                    for b in range(4):      # 低 4 位 -> 上页 bit 2b, 2b+1
                        if col & (1 << b):
                            low |= (1 << (2 * b)) | (1 << (2 * b + 1))
                    for b in range(4, 8):   # 高 4 位 -> 下页
                        if col & (1 << b):
                            high |= (1 << (2 * (b - 4))) | (1 << (2 * (b - 4) + 1))
                    self.buf[up + tx] = low
                    self.buf[lo + tx] = high
            # 字间距占 2 列
            for dx in range(10, 12):
                if x + dx < W:
                    self.buf[up + x + dx] = 0
                    self.buf[lo + x + dx] = 0
            x += 12
        return x

    def draw_cn(self, x, page, s, font_cn):
        """16x16 汉字：前 16 字节上半页、后 16 字节下半页，与固件一致。"""
        if page + 1 >= 8:
            return x
        for ch in s:
            g = font_cn.get(ch)
            if g is None:
                # 缺字：画空心方框（与固件占位符一致）
                for i in range(16):
                    if x + i >= W:
                        break
                    edge = i in (0, 15)
                    self.buf[self._row(page) + x + i] = 0xFF if edge else 0x81
                    self.buf[self._row(page + 1) + x + i] = 0xFF if edge else 0x81
                x += 16
                continue
            for i in range(16):
                if x + i >= W:
                    break
                self.buf[self._row(page) + x + i] = g[i]
                self.buf[self._row(page + 1) + x + i] = g[16 + i]
            x += 16
        return x

    def fill_span(self, x0, x1, page, pattern):
        for x in range(x0, min(x1 + 1, W)):
            self.buf[self._row(page) + x] = pattern

    def cn_width(self, s):
        return len(s) * 16

    def to_image(self):
        img = Image.new("1", (W, H), 0)
        px = img.load()
        for page in range(8):
            for x in range(W):
                b = self.buf[self._row(page) + x]
                for bit in range(8):
                    if b & (1 << bit):
                        px[x, page * 8 + bit] = 1
        return img.resize((W * SCALE, H * SCALE), Image.NEAREST)


def render(font_cn, font5x7, *, synced, hour, minute, sec, year, month, day,
           wday_cn, rssi, sync_age, refresh_count):
    """按 main.cpp 的 render() 逐句复刻，保证和固件画的是同一帧。"""
    s = Screen()
    s.clear()

    # 第 0 页：状态行
    if synced:
        x = s.draw_cn(0, 0, "已同步", font_cn)
        s.draw_ascii(x, 0, " %ds" % sync_age, font5x7)
    else:
        s.draw_cn(0, 0, "等待网络", font_cn)
    cnt = "#%d" % refresh_count
    s.draw_ascii(W - len(cnt) * 6, 0, cnt, font5x7)

    # 第 1 页：分隔线
    s.fill_span(0, W - 1, 1, 0x18)

    if not synced:
        s.draw_ascii(0, 2, "--:--", font5x7)
        s.draw_ascii(0, 4, "waiting for NTP", font5x7)
        s.draw_ascii(0, 5, "check WiFi in", font5x7)
        s.draw_ascii(0, 6, "src/secrets.h", font5x7)
        return s.to_image()

    # 第 2~3 页：大号 HH:MM
    s.draw_ascii_2x(0, 2, "%02d:%02d" % (hour, minute), font5x7)
    # 右侧秒
    s.draw_ascii(W - 24, 2, ":%02d" % sec, font5x7)

    # 第 5 页：日期
    dx = s.draw_ascii(0, 5, "%04d" % year, font5x7)
    dx = s.draw_cn(dx, 5, "年", font_cn)
    dx = s.draw_ascii(dx, 5, "%02d" % month, font5x7)
    dx = s.draw_cn(dx, 5, "月", font_cn)
    dx = s.draw_ascii(dx, 5, "%02d" % day, font5x7)
    dx = s.draw_cn(dx, 5, "日", font_cn)

    # 第 6 页：星期
    week = "星期" + wday_cn
    w = s.cn_width(week)
    s.draw_cn((W - w) // 2, 6, week, font_cn)

    # 第 7 页：页脚
    x = s.draw_cn(0, 7, "信号", font_cn)
    s.draw_ascii(x, 7, " %d" % rssi, font5x7)

    return s.to_image()


def main():
    font_cn = parse_font_cn(os.path.join(SRC, "font_cn.h"))
    font5x7 = parse_font5x7(os.path.join(SRC, "font5x7.h"))
    print("字库：中文 %d 字，ASCII %d 字" % (len(font_cn), len(font5x7)))

    # 与刚才串口日志里的一致：2026-10-14 22:09:38 星期三
    img = render(font_cn, font5x7, synced=True, hour=22, minute=9, sec=38,
                 year=2026, month=10, day=14, wday_cn="三", rssi=-46,
                 sync_age=1, refresh_count=1)
    out = os.path.join(HERE, "preview.png")
    img.save(out)
    print("预览图 -> " + os.path.normpath(out))

    # 顺带看一眼没对上时间时的样子
    img2 = render(font_cn, font5x7, synced=False, hour=0, minute=0, sec=0,
                  year=2026, month=1, day=1, wday_cn="四", rssi=0,
                  sync_age=0, refresh_count=7)
    out2 = os.path.join(HERE, "preview-waiting.png")
    img2.save(out2)
    print("等待态 -> " + os.path.normpath(out2))


if __name__ == "__main__":
    main()
