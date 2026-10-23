#include "clock.h"

namespace workclock {

namespace {
tu::DT g_now;
}

void begin(const tu::DT &t) { g_now = t; }

void advance(uint32_t seconds) {
  if (seconds > 86400u) seconds = 86400u;
  for (uint32_t i = 0; i < seconds; ++i) {
    tu::addOneSecond(g_now);
  }
}

const tu::DT &now() { return g_now; }

}  // namespace workclock
