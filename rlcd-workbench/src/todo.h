// rlcd-workbench / 待办与行程（M3）
//
// 数据来自 /status/todo.json，由服务器侧的 status-todo.py 归一后原子发布
// （字段契约见 status-oled3/docs/adr/0012，语义见 ADR-0006）。
//
// 三条与「看起来可以简化」相反的约定：
//
//   1. 槽位里只放「未完成」的条目。服务器按 due 升序排、已完成沉底，
//      我们只取前 6 条未完成。屏上那 6 行因此恒为待办，勾选框永远是空的 ——
//      契约里没有逐条的 done 标记，只有总数，硬要画「第几行已完成」就是编造。
//
//   2. itemN_due 原样透传，服务器不校验格式。设备必须自己容错：坏掉的那条
//      只跳过它自己，其余照常显示，页尾标「待办数据异常」。这条路径是 A3
//      断言要求的，服务器侧洗掉数据就永远测不到了。
//
//   3. days_month 与 days 是两个字段而不是一个。服务器每 5 分钟重生成，跨月
//      瞬间（23:58 生成十月、00:01 设备已进十一月）会把上月的日子标到新月份。
//      月份对不上就一个点都不标 —— 错一整个月的行程点比少标一天更糟。

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace todo {

constexpr int kSlots = 6;
constexpr int kTextCap = 32;
constexpr int kDueCap = 20;

struct Data {
  bool valid = false;
  uint32_t fetchedMs = 0;

  int total = 0;  // 有效待办总数（含已完成），页首显示 total - done
  int done = 0;
  int slots = 0;  // 真正有内容的槽数，0..6

  char text[kSlots][kTextCap] = {};
  char due[kSlots][kDueCap] = {};  // 绝对时间串，原样；空串表示该条没有期限

  char daysMonth[8] = {};        // "YYYY-MM"
  uint8_t tripCount[32] = {};    // 索引 1..31，当月每天几条行程

  int badRows = 0;  // 被跳过的非法条目数（A3 要求页尾能说出「异常」）

  int lastHttpCode = 0;
  uint32_t lastLen = 0;
  uint32_t pollCount = 0;
  uint32_t failCount = 0;
};

void begin();
void tick();
const Data &data();

uint32_t ageMs();
bool stale();   // 超过 kTodoStaleMs 没更新
bool fresh();
void requestRefresh();

// 把绝对到期串换算成右栏那个 3 字符以内的相对描述。
// 成功返回 true 并写入 out；格式非法返回 false（调用方据此计入 badRows）。
//
// 宽度是被版面卡死的：到期栏预算 23 px（wqy12），"明天"=23、"10-28" 要 28 px
// 会挤掉正文，所以超过明天的一律写 "N日"（当月）或 "N月"（跨月），最多 3 字。
bool relativeDue(const char *due, int year, int month, int day, int hour, int minute,
                 char *out, size_t cap);

}  // namespace todo
