#pragma once

#include <Arduino.h>

// WiFi 连接管理：编译期写死凭据（src/secrets.h，gitignored），自带断线重连。
//
// 为什么不直接用 Arduino 的 WiFi.setAutoReconnect()：
// 它内部的重试节奏我们看不见也改不了，而且和「我们自己再调一次 begin()」会打架。
// 这里关掉框架的自动重连，自己跑一个三阶段小状态机，每次尝试有明确超时、
// 失败之间退避，状态变化只打一行日志。屏幕上那个三态图标就是它的投影。
namespace netwifi {

// 对外只暴露三态，与 ui.h 的 WifiState 一一对应（不多不少）：
//   NotConfigured / Connected / Connecting
// 刻意没有「断开」这个第四态：只要还在重试，屏幕上就该是「连接中」而不是叉号——
// 画叉会让人以为网络死了，实际上它正在重连。
enum class State : uint8_t {
  NotConfigured = 0,  // secrets.h 里没填 SSID -> 画叉
  Connecting = 1,     // 正在连，或失败后等下一次重试 -> 画半格
  Connected = 2,      // -> 画满格
};

void begin();  // setup() 里调一次
void tick();   // 主循环每 kWifiTickMs 调一次

State state();
const char *stateName();  // 串口用，ASCII

const char *ssid();
int rssi();             // 仅 Connected 时有效，否则 0
const char *ipText();   // 静态缓冲；未连上返回 ""

uint32_t connectCount();  // 成功连上过几次
uint32_t dropCount();     // 掉线过几次

// bringup 诊断用：模拟一次链路中断，走的是和真掉线同一条状态机路径。
// 只有 cfg::kWifiDropTestMs 非 0 时才会被调用。
void debugDropLink();

}  // namespace netwifi
