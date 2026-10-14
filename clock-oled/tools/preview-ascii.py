"""把预览帧打成 ASCII 字符画，用于无图形环境下核对版面。

为什么不用 PNG：
  写这个脚本的模型读不了图片。所以把渲染结果降级成文本——
  每个像素一个字符，肉眼可读版面对不对、有没有越界、有没有叠字。
  同时做几项硬检查：行宽、行距、留白、是否有像素画到屏幕外。

用法：python tools/preview-ascii.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image  # noqa: E402

import importlib.util  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "pv", os.path.join(HERE, "preview-render.py"))
pv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pv)

SRC = pv.SRC


def to_ascii(img, scale):
    """每 2x4 像素压缩成一个字符，得到 64x16 的字符画。"""
    px = img.load()
    lines = []
    for cy in range(0, img.height, scale * 4):
        line = ""
        for cx in range(0, img.width, scale * 2):
            # 看这个小块里有多少点亮的像素
            lit = 0
            total = 0
            for y in range(cy, min(cy + scale * 4, img.height)):
                for x in range(cx, min(cx + scale * 2, img.width)):
                    total += 1
                    if px[x, y]:
                        lit += 1
            r = lit / max(total, 1)
            line += " " if r == 0 else ("." if r < 0.2 else ("*" if r < 0.6 else "#"))
        lines.append(line)
    return lines


def occupied_rows(img, scale):
    """逐行统计：屏上第 y 行（像素）有没有被点亮。返回 64 个 bool。"""
    px = img.load()
    out = []
    for y in range(0, img.height, scale):
        lit = any(px[x, y] for x in range(img.width))
        out.append(lit)
    return out


def main():
    font_cn = pv.parse_font_cn(os.path.join(SRC, "font_cn.h"))
    font5x7 = pv.parse_font5x7(os.path.join(SRC, "font5x7.h"))

    cases = [
        ("正常显示 2026-10-14 22:09 星期三", dict(
            synced=True, hour=22, minute=9, sec=38, year=2026, month=10,
            day=14, wday_cn="三", rssi=-46, sync_age=1, refresh_count=1)),
        ("单数字日 2026-01-05 08:03 星期一", dict(
            synced=True, hour=8, minute=3, sec=5, year=2026, month=1,
            day=5, wday_cn="一", rssi=-52, sync_age=3600, refresh_count=3600)),
        ("等待网络（未对时）", dict(
            synced=False, hour=0, minute=0, sec=0, year=2026, month=1,
            day=1, wday_cn="四", rssi=0, sync_age=0, refresh_count=7)),
    ]

    for title, kw in cases:
        img = pv.render(font_cn, font5x7, **kw)
        print("=" * 70)
        print(title)
        print("=" * 70)
        for line in to_ascii(img, pv.SCALE):
            print("|" + line.rstrip() + "|")

        rows = occupied_rows(img, pv.SCALE)
        used = [i for i, v in enumerate(rows) if v]
        print("-- 点亮的像素行范围：{} .. {}".format(
            min(used) if used else "-", max(used) if used else "-"))

        # 分页统计：哪几页有内容、内容占了多少列
        for page in range(8):
            px = img.load()
            cols = [x for x in range(img.width)
                    if any(px[x, y] for y in range(page * 8 * pv.SCALE,
                                                   (page + 1) * 8 * pv.SCALE))]
            if cols:
                print("   第 {} 页 (y={:2d}..{:2d}): 列 {}..{}".format(
                    page, page * 8, page * 8 + 7, min(cols) // pv.SCALE,
                    max(cols) // pv.SCALE))
            else:
                print("   第 {} 页 (y={:2d}..{:2d}): 空".format(
                    page, page * 8, page * 8 + 7))
        print()

    # 缺字检查：字表里所有字都必须能渲染出来
    print("=" * 70)
    print("字表完整性")
    print("=" * 70)
    import re
    text = open(os.path.join(SRC, "font_cn.h"), encoding="utf-8").read()
    m = re.search(r'kFontCnChars\[\]\s*=\s*"([^"]+)"', text)
    chars = m.group(1)
    print("字表共 {} 字：{}".format(len(chars), chars))
    missing = [c for c in chars if c not in font_cn]
    print("解析到的字形数：{}".format(len(font_cn)))
    print("缺字形：{}".format(missing if missing else "无"))

    # 检查主程序用到的中文是否都在字表里
    main_cpp = open(os.path.join(SRC, "main.cpp"), encoding="utf-8").read()
    used_cn = set(re.findall(r'"([\u4e00-\u9fff]+)"', main_cpp))
    used_cn |= {"日", "一", "二", "三", "四", "五", "六"}  # 星期表里的
    print("main.cpp 用到的中文：{}".format("".join(sorted(used_cn))))
    absent = [c for c in "".join(sorted(used_cn)) if c not in chars]
    print("其中不在字表里的：{}".format(absent if absent else "无 —— 全部可渲染"))


if __name__ == "__main__":
    main()
