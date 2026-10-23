// rlcd-workbench / 版本号
//
// 语义化版本，v0.1.0 起步。改这个文件就是改"对外报的身份"，所以它单独一处，
// 不要在两个地方各写一份字符串字面量 —— 关于页与开机报告必须永远说同一个数。
#pragma once

namespace ver {
constexpr const char *kVersion = "v0.1.0";
constexpr const char *kDate = "2026-10-23";
}  // namespace ver