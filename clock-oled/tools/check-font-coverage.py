"""检查"屏幕上会显示的中文"是否都在字模表里。

为什么单独做这件事：
  字模表只有 29 个字，故意做小（Flash 不浪费）。
  但漏一个字的后果是屏幕上出现一个空心方框——
  编译不报错、串口日志正常，只有盯着屏幕才看得出来。
  所以用静态检查兜住这一类错误。

难点：注释里的中文也会被正则扫到，但它们不需要字模。
  这里先剥掉注释，再只取字符串字面量。

退出码：0 = 全部有字模；1 = 有缺失（可以挂到 CI 或 pre-commit 上）。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

CJK = r"\u4e00-\u9fff"


def strip_comments(text):
    """去掉 C/C++ 注释，避免把注释里的中文误判成要显示的文字。

    按字符扫描而不是用正则：要正确处理字符串里的 // 和注释里的引号。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":      # 行注释
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":    # 块注释
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c == '"':                                          # 字符串字面量
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":                             # 转义
                    out.append(text[i])
                    i += 1
                if i < n:
                    out.append(text[i])
                    i += 1
            if i < n:
                out.append(text[i])
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    hdr = open(os.path.join(SRC, "font_cn.h"), encoding="utf-8").read()
    m = re.search(r'kFontCnChars\[\]\s*=\s*"([^"]+)"', hdr)
    if not m:
        print("[ERR] font_cn.h 里找不到 kFontCnChars，格式变了？")
        return 1
    have = m.group(1)

    # 只扫代码里会画到屏幕上的中文字符串
    sources = []
    for name in ("main.cpp", "ssd1306_i2c.cpp", "ssd1306_i2c.h"):
        p = os.path.join(SRC, name)
        if os.path.exists(p):
            sources.append((name, strip_comments(
                open(p, encoding="utf-8").read())))

    used = {}   # 汉字 -> 首次出现的文件名
    for name, code in sources:
        # 只看字符串字面量；注释已在上一步剥掉
        for lit in re.findall(r'"([^"\n]*)"', code):
            for ch in re.findall("[%s]" % CJK, lit):
                if ch not in used:
                    used[ch] = name

    # 星期表是数组里逐个给出的，不是完整字符串，单独补进来
    for ch in "一二三四五六日":
        used.setdefault(ch, "main.cpp:kWeekdayCn")

    missing = {c: f for c, f in used.items() if c not in have}

    print("字模表 {} 字：{}".format(len(have), have))
    print("代码里会显示的中文 {} 字：{}".format(len(used), "".join(sorted(used))))
    if missing:
        print("")
        print("[缺失] 下列汉字会显示成空心方框，请加进 tools/gen-font-cn.py 的 CHARS：")
        for c, f in sorted(missing.items()):
            print("   {}  <- {}".format(c, f))
        return 1

    # 字模表允许比实际用到的多一些（预留几字，改文案时不用重新生成字模）。
    # 这不是问题，所以只报个数，不当告警。
    unused = len(have) - len(used)
    print("")
    print("[OK] 所有会显示的中文都有字模（另有 {} 字为预留）。".format(unused))
    return 0


if __name__ == "__main__":
    sys.exit(main())
