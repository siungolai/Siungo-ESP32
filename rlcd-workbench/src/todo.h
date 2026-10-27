// rlcd-workbench / 待办与行程（M8，ADR-0020）
//
// 数据来自 https://<数据源主机>/api/todo-device —— 网站侧的「派生只读投影」，
// 与待办表同源但只读（契约见 Siungo-Website/.doc/architecture/待办摘要设备接口.md，
// 固件侧要点见同目录的 ESP32待办摘要接入清单.md）。
//
// 三条与「看起来可以简化」相反的约定：
//
//   1. 屏上六行恒为未完成条目。契约里没有逐条 done 标记（服务端只发 pending），
//      所以勾选框永远画空 —— 硬要画「第几行已完成」就是编造设备没有的事实。
//
//   2. dueDate 是纯日期（YYYY-MM-DD），不是时间戳；「今天」的权威来源是响应里的
//      today 字段，不是本机 RTC。RTC 漂一格，整列「今天/明天/逾期」全错一格。
//      today 缺失时才退回本机日期（宽进：服务端还没上线这一格的过渡期也能跑）。
//
//   3. trips.days 是扁平天号数组、重复即多点（[22,24,27,27,27] = 22 号 1 个点、
//      24 号 1 个点、27 号 3 个点）。不去重，也不要求有序。
//
// 「宽进」是这一版的既定策略：today / stats.undated / stats.outOfWindow / trips
// 都是服务端下一版才有的字段，缺了要能降级跑，不能判失败 —— 否则网站上线前后
// 屏上会一直停在「等待数据」。

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace todo {

constexpr int kSlots = 6;
// 标题原文缓冲：12 个字符最坏是 12 个 4 字节 UTF-8 = 48 B，96 留一倍余量。
// v0.1.3 这里的 32 B 对「12 个汉字 = 36 B」本来就不够，会切出半个汉字。
constexpr int kTextCap = 96;
constexpr int kTitleRunes = 12;  // 屏上最多画 12 个字符（宽 149 px 的由来）
constexpr int kDueCap = 16;      // "逾期31天" 最多 11 B
constexpr int kTodayCap = 12;    // "YYYY-MM-DD"
constexpr int kDotsPerDay = 3;   // 每个格子最多画 3 个点

struct Data {
  bool valid = false;
  uint32_t fetchedMs = 0;

  // stats：服务端在全量待办上算的四个数，与 items 的筛选/截断无关。
  int pending = 0;       // 全部未完成（含无日期、含窗口外）—— 页首那个数
  int overdue = 0;       // 未完成且 dueDate < today
  int todayDue = 0;      // 未完成且 dueDate == today
  int earlier = 0;       // 未完成且 dueDate < 本周周一
  // 下面两个是下一版契约才有的字段，-1 表示「服务端没发」，不是「0 条」。
  int undated = -1;      // 未完成但完全没有日期
  int outOfWindow = -1;  // 未完成、有日期、但落在窗口外

  int remaining = 0;     // 截断后还剩多少条没塞进 items
  bool truncated = false;

  int slots = 0;  // 真正有内容的槽数，0..6

  char text[kSlots][kTextCap] = {};  // 标题原文（服务端不截断，设备自己截）
  char due[kSlots][kDueCap] = {};    // 纯日期 "YYYY-MM-DD"；空串 = 该条没有期限
  char today[kTodayCap] = {};        // 服务端的 today；空串 = 没发，用本机日期

  char tripsMonth[8] = {};       // "YYYY-MM"
  uint8_t tripCount[32] = {};    // 索引 1..31，当月每天几条行程

  int badRows = 0;  // 被跳过的非法条目数（页尾据此说「待办数据异常」）

  // 状态位。它们不是「这次请求失败了」，而是「得有人管」——
  // 屏上要说出到底哪一格不对（密钥 / 端点 / 版本），否则只剩一句「异常」。
  bool upgradeRequired = false;  // schemaVersion != 1
  bool keyRejected = false;      // 401
  bool endpointMissing = false;  // 404（线上还没部署到含此端点的版本）

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

// 把纯日期到期串换算成右栏那个相对描述。
//
// today 是服务端给的业务日期（"YYYY-MM-DD"）；为空时退回 (year, month, day)。
// 六档：逾期 →「逾期N天」、0 天→「今天」、1 天→「明天」、≤7 天→「N天后」、
// 更远→「MM-DD」。成功返回 true 并写入 out；dueDate 格式非法返回 false
// （调用方据此计入 badRows）。
bool relativeDue(const char *dueDate, const char *today, int year, int month, int day,
                 char *out, size_t cap);

// 标题不在这一层截：数据层留原文（kTextCap 字节的安全上界），
// 真正的裁切由 ui 的 textClipped() 按可用像素做 —— 汉字和 ASCII 一样宽才怪。

}  // namespace todo
