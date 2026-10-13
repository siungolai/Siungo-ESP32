# blink3-test · 共阳 / 共阴硬件判定

临时判定项目：烧进去看哪一段灯全亮，判定完就可以删。判定结论写进 `blink3/README.md` 的「假设」一节。

## 判什么

手里的是**共阳 RGB 模块**还是**分立 LED 共阴接法**？

| 灯的表现 | 结论 |
|---|---|
| 只在 **A 段**（全 LOW）三灯全亮 | 共阳（公共端接 3V3，低电平点亮）→ `blink3/src/main.cpp` 原样可用 |
| 只在 **B 段**（全 HIGH）三灯全亮 | 共阴（公共端接 GND，高电平点亮）→ 把 `blink3/src/main.cpp` 里三处 `digitalWrite` 的 `HIGH`/`LOW` 对调 |
| 两段都不亮 | 不是接法问题：查线材、公共端有没有接好、电阻是否断路、灯珠是否损坏 |

## 程序行为

每秒切换一次电平，串口同步打印当前段：

```
TEST A: all LOW  (共阳接法此时应全亮 1s)
TEST B: all HIGH (共阴接法此时应全亮 1s)
```

三个 GPIO 4 / 5 / 6 与 `blink3` 完全一致，直接把 blink3 的接线搬过来即可。

## 编译烧录

```bash
cd blink3-test
pio run -t upload
pio device monitor   # 115200
```

板子配置与 `blink3`、`web-led` 相同（ESP32-S3-DevKitC-1 N16R8，原生 USB 串口）。
