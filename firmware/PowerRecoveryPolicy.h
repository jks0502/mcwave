#pragma once
#include <stdint.h>
#include <limits.h>

namespace mcwavePower {
constexpr uint32_t kRtcTimeoutMs=12UL*60*1000;
constexpr uint32_t kCounterMagic=0x42545631;
constexpr unsigned kReasonCount=11; // Classic ESP32: ESP_RST_UNKNOWN .. ESP_RST_SDIO.
inline const char *reasonName(uint32_t reason) {
  static const char *const names[]={"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO"};
  return reason<kReasonCount?names[reason]:names[0];
}
// ESP32 ROM reset codes (esp32/rom/rtc.h), distinct from esp_reset_reason().
inline bool isRtcWatchdog(uint32_t raw) {return raw==9||raw==13||raw==16;}
inline bool rtcTicks(uint32_t timeoutMs,uint32_t periodQ19,uint32_t &ticks) {
  if(!timeoutMs||!periodQ19)return false;
  const uint64_t numerator=uint64_t(timeoutMs)*1000*(1ULL<<19);
  const uint64_t value=(numerator+periodQ19-1)/periodQ19;
  if(!value||value>UINT32_MAX)return false;
  ticks=uint32_t(value);return true;
}
inline void increment(uint32_t &value){if(value!=UINT32_MAX)++value;}
struct BootCounts {
  uint32_t magic=kCounterMagic,total=0,reason[kReasonCount]={},rwdt=0;
  uint32_t lastReason=0,lastRaw0=0,lastRaw1=0;
  void record(uint32_t why,uint32_t raw0,uint32_t raw1) {
    increment(total);increment(reason[why<kReasonCount?why:0]);
    if(isRtcWatchdog(raw0)||isRtcWatchdog(raw1))increment(rwdt);
    lastReason=why;lastRaw0=raw0;lastRaw1=raw1;
  }
};
}
