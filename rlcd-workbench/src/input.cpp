#include "input.h"

#include <Arduino.h>

#include "config.h"
#include "vendor/multi_button/multi_button.h"

namespace {

// multi_button 的 HAL：按 button_id 读电平，返回「按下 = 有效电平」
constexpr uint8_t kActiveLow = 0;

Button g_btn[2];

// 每个键最多缓存一次手势。主循环 drain 得比人按键快，
// 一个槽够用；真丢事件再加环形缓冲。
Gesture g_pending[2] = {Gesture::None, Gesture::None};

uint32_t g_bootSuppressUntil = 0;

uint8_t readLevel(uint8_t id) {
  const int pin = (id == 0) ? cfg::kKeyBoot : cfg::kKeyUser;
  return (uint8_t)digitalRead(pin);
}

// 上电窗口内 BOOT 的事件全部吞掉
bool suppressed(uint8_t id) {
  return id == 0 && (int32_t)(millis() - g_bootSuppressUntil) < 0;
}

void onEvent(Button *b) {
  const uint8_t id = b->button_id;
  if (suppressed(id)) {
    return;
  }
  // multi_button 在回调里把本次事件写回 handle->event
  switch (b->event) {
    case BTN_SINGLE_CLICK:
      g_pending[id] = Gesture::Short;
      break;
    case BTN_DOUBLE_CLICK:
      g_pending[id] = Gesture::Double;
      break;
    case BTN_LONG_PRESS_START:
      g_pending[id] = Gesture::Long;
      break;
    default:
      break;
  }
}

}  // namespace

namespace input {

void begin() {
  pinMode(cfg::kKeyBoot, INPUT_PULLUP);
  pinMode(cfg::kKeyUser, INPUT_PULLUP);

  button_init(&g_btn[0], readLevel, kActiveLow, 0);
  button_init(&g_btn[1], readLevel, kActiveLow, 1);

  button_attach(&g_btn[0], BTN_SINGLE_CLICK, onEvent);
  button_attach(&g_btn[0], BTN_DOUBLE_CLICK, onEvent);
  button_attach(&g_btn[0], BTN_LONG_PRESS_START, onEvent);

  button_attach(&g_btn[1], BTN_SINGLE_CLICK, onEvent);
  button_attach(&g_btn[1], BTN_DOUBLE_CLICK, onEvent);
  button_attach(&g_btn[1], BTN_LONG_PRESS_START, onEvent);

  button_start(&g_btn[0]);
  button_start(&g_btn[1]);
}

void tick() { button_ticks(); }

Gesture drain(Key k) {
  const int i = (k == Key::Boot) ? 0 : 1;
  const Gesture g = g_pending[i];
  g_pending[i] = Gesture::None;
  return g;
}

void suppressBootFor(uint32_t ms) { g_bootSuppressUntil = millis() + ms; }

}  // namespace input
