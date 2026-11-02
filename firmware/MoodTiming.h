#pragma once
#include <cstdint>

namespace moodTiming {
constexpr uint32_t syncInterval(bool showingMood){return showingMood?8000u:15000u;}
inline bool refreshDue(bool changed,bool hasFrame,uint32_t now,uint32_t last,uint32_t minimum=60000u){
  return changed&&(!hasFrame||uint32_t(now-last)>=minimum);
}
}
