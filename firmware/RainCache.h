#pragma once
#include <Preferences.h>
#include "RainForecast.h"
inline bool loadRainCache(RainForecast &data) {
  Preferences cache;RainForecast saved;
  if(!cache.begin("rain-cn-v1",true))return false;
  const bool read=cache.getBytesLength("last")==sizeof(saved) && cache.getBytes("last",&saved,sizeof(saved))==sizeof(saved);
  cache.end();if(!read || !validRainForecast(saved))return false;data=saved;return true;
}
inline bool saveRainCache(const RainForecast &data) {
  if(!validRainForecast(data))return false;Preferences cache;
  if(!cache.begin("rain-cn-v1",false))return false;
  const bool written=cache.putBytes("last",&data,sizeof(data))==sizeof(data);cache.end();return written;
}
