#pragma once
#include "ClockConfig.h"

namespace mcwaveRecovery {
constexpr uint32_t kLoopTimeoutMs = 600UL * 1000;
constexpr uint32_t kSyncTimeoutMs = 60UL * 60 * 1000;
inline bool syncExpired(uint32_t now, uint32_t lastSuccess, bool otaBusy) {
  return !otaBusy && elapsed(now, lastSuccess, kSyncTimeoutMs);
}
}
