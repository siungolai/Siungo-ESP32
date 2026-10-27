// rlcd-workbench / 本地提示音（v0.1.5，docs/adr/0022）
//
// 本板没有蜂鸣器，出声只有一条路：ES8311 -> NS4150B（CTRL=GPIO46）-> 外接喇叭。
// 这一层只做「把一段 PCM 送去喇叭」，不做任何时间判断、不认设置项 ——
// 「该不该响」由 main.cpp 决定（ADR-0022 决策 §0 那棵判定树），
// 这一层回答的是「能不能响」。
#pragma once

#include <stdint.h>

namespace sound {

// setup() 里调一次。失败不致命：设备照常能用，只是不响。
// 返回 false 时 reason() 给出 ASCII 原因，串口会打出来。
bool begin();

// codec 是否可用。false 时 playChime() 直接返回 false。
bool ready();

// 播一声「嘀嘀」（两声共 kChimeToneMs*2 + kChimeGapMs 毫秒）。
// 未 ready、或上一次还没放完时返回 false —— 不排队、不复读。
// 阻塞到播完为止（约 150 ms），调用方在 loop() 里调，不占中断。
bool playChime();

// 播一段方波，时长毫秒。kSoundSelfTest 用。
bool playTone(uint16_t hz, uint32_t ms);

// 上一次 begin() 失败的原因（ASCII）。成功时返回 "ok"。
const char *reason();

}  // namespace sound
