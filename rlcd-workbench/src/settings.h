// rlcd-workbench / 设置注册表（v0.1.1，docs/adr/0017）
//
// 为什么是表驱动：v0.1.0 的对比度是 main.cpp 里的一个全局变量加一个
// cycleContrast()。设置版面落地之后，每一项设置都要同时出现在三个地方 ——
// 手势分发（BOOT 长按切哪一项）、绘制（行内文案）、持久化（NVS key）。
// 硬编码会让这三处同时长分支；表驱动之后「加一项设置」只加一个描述符。
//
// 三条职责边界（都是 docs/adr/0017 的决策，不是实现习惯）：
//   1. 本模块只负责「值 + 生效 + 持久化」，不弹 toast、不置脏、不画屏。
//      设置版面里整行反显就是反馈，toast 会盖住它；面板上的对比度热键
//      要 toast，那由 main.cpp 在热键路径自己弹。
//   2. 注册表里只放已实现的项。留一个「屏上有、按不动」的空壳桩就是
//      ADR-0003 反对的行为未定义 —— 参照 ADR-0011 对 ai.h 的规矩
//      （只写空头文件加注释说明动手顺序，不造假接口）。
//   3. 写入时机是「每次变更立即写」，不是退出设置才写 —— 对比度必须
//      当场看见效果，将来声音之类的设置同样成立。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace settings {

// 一项设置的运行期视图。注册表是 const 描述符数组，index 是唯一的可变状态，
// 单独放在一个 POD 里，避免描述符表被改成运行期结构。
struct State {
  uint8_t index = 0;  // 当前选中第几个选项
};

// 注册表里每一项的描述符。apply/format 由具体设置项提供，缺一个都画不出
// 那一行、也按不动那一行。
struct Item {
  const char *label;                              // 中文标签（行首）
  uint8_t optionCount;                            // 选项总数
  const char *nvsKey;                             // NVS key；nullptr = 不持久化
  void (*apply)(uint8_t idx);                     // 立即生效
  void (*format)(uint8_t idx, char *buf, size_t cap);  // 行内文案（如 "3/5 (60)"）
};

// 注册表本体。顺序即屏上顺序（首行 = 选中行初始位置）。
const Item *table();
size_t count();

// ---- 运行期状态 ----
// 每一项一个槽位，按项索引取。越界时 indexOf 回默认档 —— 调用方
// （main.cpp 的热键路径与 ui.cpp 的绘制）因此不需要自己判越界。
uint8_t indexOf(size_t i);
// 可变引用：热键路径「+1 后落盘」需要直接改值，所以开放引用而不是只给 setter。
uint8_t &indexRef(size_t i);
// 把指定项写进 NVS（变更即写）。热键路径与设置版面共用。
void save(size_t i);

// ---- 导航（设置版面用）----
// 选中行切到下一项，循环（ADR-0017 决策：单选循环，头尾相接）。
// 只有一项时按下去「看起来没动」而不是没反应 —— 后者是行为未定义。
void selectNext();
// 当前选中行是第几项（0 = 首行）。ui.cpp 画反显时用。
size_t selected();
// 当前选中项的选项切到下一个，循环，并立即 apply + 落盘。
// 返回新的 index。
uint8_t cycleCurrent();

// ---- 持久化（Preferences / NVS，namespace 见 .cpp）----
// setup() 里调一次：读回每一项，越界或无 key 的一律回默认档并打串口说明。
// 不夹到最近合法档 —— 那会让屏上出现一个用户没选过的值，比回默认档更难排查。
void loadAll();
// 把指定项写进 NVS（变更即写）。见上面 save(size_t) 的注释。
void saveCurrent();

// 开机报告与串口用的三态来源：这一项的值是 NVS 读回的、默认档、
// 还是 NVS 越界回退的。屏上不显示来源，屏上只显示值本身。
enum class Src : uint8_t { Default = 0, Nvs, NvsOutOfRange };
Src srcOf(size_t i);
// 上面那个枚举的 ASCII 串（串口日志不打中文，见策划案 §5 M2）。
const char *srcNameOf(size_t i);

// 设置版面用的两个取词器：选中项的中文标签、以及把选中项格式化成行内文案
// （"3/5 (60)"）。绘制逻辑留在 ui.cpp，这里只提供「每一项自己的说法」。
const char *currentLabel();
void formatCurrent(char *buf, size_t cap);

}  // namespace settings
