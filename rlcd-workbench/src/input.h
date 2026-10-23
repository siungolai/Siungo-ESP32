// rlcd-workbench / 输入层
//
// 板载两个按键：BOOT(GPIO0) 与 KEY(GPIO18)，都低有效、内部上拉。
// 手势判定用官方例程同一份 multi_button（见 src/vendor/multi_button/README.md），
// 六个手势槽的归属见 docs/adr/0003。

#pragma once

#include <stdint.h>

enum class Key : uint8_t { Boot = 0, User = 1 };

enum class Gesture : uint8_t { None = 0, Short, Double, Long };

namespace input {

// 建按键、挂事件、启动 5 ms 扫描。
void begin();

// 必须每 5 ms 调一次，内部转给 multi_button 的 button_ticks()。
void tick();

// 取走这个键最近一次手势（取走即清空，没有则返回 None）。
Gesture drain(Key k);

// 上电 1 秒内丢弃 BOOT 事件：按住 BOOT 上电会进下载模式，
// 那一下按键不该被当成「刷新」。
void suppressBootFor(uint32_t ms);

}  // namespace input
