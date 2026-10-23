// rlcd-workbench / 服务器指标快照
//
// 数据源是 status-oled3 早就跑着的 https://www.siungo.top/status/snapshot.json
// （契约见 status-oled3/docs/adr/0012）。本项目不改它的语义，只在本期议定把
// 字段数从 19 扩到 22（加 svc_select / svc_wb / svc_ma12，见 ADR-0014）。
//
// 三条硬规矩，跟 status-oled3 一致：
//   1. 解析走 jsonfield（扁平对象、手写取值、不引 ArduinoJson，ADR-0010）
//   2. 字段为 null 或缺失 = 「不可用」，**绝不当作 0 或失败** —— 服务器采不到和
//      服务器挂了是两件事，屏幕上必须长得不一样
//   3. 新鲜度用设备自己的单调时钟算，不信服务器给的 generated_at（那个只作信息）

#pragma once

#include <stdint.h>

#include "config.h"

namespace snapshot {

// 探针三态。缺一个 Unknown 的话，字段还没部署时那六个格子会全画成叉，
// 看起来像「服务器全挂了」，而实际只是「这一项还没接」。
enum class Probe : uint8_t {
  Unknown = 0,
  Ok = 1,
  Fail = 2,
};

struct Data {
  bool valid = false;       // 至少成功解析过一次
  uint32_t fetchedMs = 0;   // 最后一次成功时的 millis()

  // ---- 左栏四行 ----
  bool hasCpu = false;
  int cpuPct = 0;

  bool hasMem = false;
  int memPct = 0;
  char memDetail[28] = "";

  bool hasDisk = false;
  int diskPct = 0;
  char diskDetail[28] = "";

  bool hasLoad = false;
  float load1 = 0.0f;

  // ---- 右栏六行探针 ----
  Probe svcWeb = Probe::Unknown;
  Probe svcSelect = Probe::Unknown;
  Probe svcWb = Probe::Unknown;
  Probe svcApi = Probe::Unknown;
  Probe svcUpload = Probe::Unknown;
  Probe svcMa12 = Probe::Unknown;

  // ---- 证书 ----
  bool hasCert = false;
  int certDays = 0;

  // 诊断用
  int lastHttpCode = 0;
  uint32_t lastLen = 0;
  uint32_t pollCount = 0;
  uint32_t failCount = 0;
};

void begin();
void tick();  // 非阻塞：到点才发一次请求

const Data &data();

// 距上次成功多少毫秒；从没成功过返回 UINT32_MAX
uint32_t ageMs();
bool stale();   // 有数据但超过 kSnapshotStaleMs 没更新
bool fresh();   // 有数据且没过期

// 强制下一次 tick 立刻拉（BOOT 短按会用到）
void requestRefresh();

}  // namespace snapshot
