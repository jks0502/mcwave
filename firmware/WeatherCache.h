#pragma once
#include <Preferences.h>
#include "WeatherData.h"

struct StoredWeather {
  uint32_t station=101190104;
  WeatherData data;
};
inline bool validWeatherCache(const WeatherData &data) {
  int64_t minute=0;
  if(!data.valid || !std::isfinite(data.temperature) || data.temperature<-100 || data.temperature>70 ||
     data.humidity<0 || data.humidity>100 || (data.code!=-1 && std::strcmp(weatherLabel(data.code),"未知天气")==0) ||
     !std::memchr(data.dataTime,0,sizeof(data.dataTime)) || !weatherMinute(data.dataTime,minute) ||
     data.forecastCount>8 || (data.forecastStepMinutes!=60 && data.forecastStepMinutes!=180))return false;
  for(int i=0;i<data.forecastCount;++i)
    if(!std::memchr(data.forecastTime[i],0,sizeof(data.forecastTime[i])) || !weatherMinute(data.forecastTime[i],minute) ||
       std::strcmp(weatherLabel(data.forecastCode[i]),"未知天气")==0)return false;
  return true;
}
inline bool loadWeatherCache(WeatherData &data) {
  Preferences cache;StoredWeather saved;
  if(!cache.begin("weather-cn-v1",true))return false;
  const bool read=cache.getBytesLength("last")==sizeof(saved) && cache.getBytes("last",&saved,sizeof(saved))==sizeof(saved);
  cache.end();
  if(!read || saved.station!=101190104 || !validWeatherCache(saved.data))return false;
  data=saved.data;return true;
}
inline bool saveWeatherCache(const WeatherData &data) {
  if(!validWeatherCache(data))return false;
  Preferences cache;StoredWeather saved;saved.data=data;
  if(!cache.begin("weather-cn-v1",false))return false;
  const bool written=cache.putBytes("last",&saved,sizeof(saved))==sizeof(saved);
  cache.end();return written;
}
