# multi_button

本目录下的 `multi_button.c` / `multi_button.h` **不是本项目写的**，是从官方
ESP32-S3-RLCD 资料归档里的 `07_Audio_Test` 例程原样拷过来的：

```
.doc/ESP32-S3-RLCD/reference-source/examples/arduino/examples/07_Audio_Test/src/ExternLib/button/
```

上游是 **0x1abin/MultiButton**（MIT）。官方例程用的就是它，参数是
`TICKS_INTERVAL 5` / `DEBOUNCE_TICKS 3` / `SHORT_TICKS 300/5` /
`LONG_TICKS 1000/5`，出单击 / 双击 / 长按开始三种事件。

**为什么不自己写**（见 `ADR-0003`，`docs/` 暂不入库）：
`status-oled3/src/button.cpp` 只有「短按 / 长按」两态，**没有双击**，而本项目的
六个手势槽里有两个是双击。手搓一个「单击 vs 双击 vs 长按」的时序判定器，
边界条件（两次点击之间多久算双击、双击的第二下又按住算不算长按）很容易写错，
不如直接复用官方例程已经在用的这份。

**改动**：无。原样拷贝，未做任何修改。将来若要升级，直接替换这两个文件即可。
