// rlcd-workbench / 版本号
//
// 语义化版本，v0.1.0 起步。改这个文件就是改"对外报的身份"，所以它单独一处，
// 不要在两个地方各写一份字符串字面量 —— 关于页与开机报告必须永远说同一个数。
//
// kDate 是版本发布日的上板日期，**不是**改代码的日子：v0.1.1 的代码
// 先合、板上验收再上，所以它填的是"实际烧进板子并验收通过的那一天"
// （当前 = 2026-10-24，A5 七条上板验收通过日）。
#pragma once

namespace ver {
constexpr const char *kVersion = "v0.1.1";
constexpr const char *kDate = "2026-10-24";
}  // namespace ver
