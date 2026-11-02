#pragma once
#include <ctime>
#include <cstdio>
#include <cstring>
#include "WeatherData.h"

constexpr int kRainSlots=56;
struct RainForecast {
  uint32_t station=58333;
  int64_t fetchedMinute=0,probabilityMinute=0,currentProbabilityMinute=0;
  uint8_t count=0;
  int16_t currentProbability=-1;
  int64_t minute[kRainSlots]={};
  int16_t code[kRainSlots]={},probability[kRainSlots]={};
  RainForecast(){for(auto &p:probability)p=-1;}
};
inline bool validRainForecast(const RainForecast &r) {
  if(r.station!=58333 || !r.count || r.count>kRainSlots || r.fetchedMinute<28401120 || r.fetchedMinute>=68374080 ||
     r.probabilityMinute<0 || r.probabilityMinute>=68374080 || r.currentProbability<-1 || r.currentProbability>100 ||
     r.currentProbabilityMinute<0 || r.currentProbabilityMinute>=68374080)return false;
  for(int i=0;i<r.count;++i)
    if(r.minute[i]<28401120 || r.minute[i]>=68374080 || (i && r.minute[i]!=r.minute[i-1]+180) ||
       r.probability[i]<-1 || r.probability[i]>100 ||
       (r.code[i]!=-1 && std::strcmp(weatherLabel(r.code[i]),"未知天气")==0))return false;
  return true;
}
struct RainReadout {char label[48]={},time[32]={},probability[48]={};};
inline RainReadout rainReadout(const RainForecast &r,const tm *local,int weatherCode,bool cached) {
  RainReadout text;
  std::strcpy(text.label,"降雨时段暂无");std::strcpy(text.probability,"概率暂无");
  if(!local || !validRainForecast(r))return text;
  tm copy=*local;const int64_t now=std::mktime(&copy)/60;
  int probability=-1;bool found=false,unknown=false;
  if(isRainCode(weatherCode)) {
    std::strcpy(text.label,cached?"缓存降雨中":"正在降雨");
    if(now>=r.currentProbabilityMinute && now<r.currentProbabilityMinute+60)
      probability=r.currentProbability;
    found=true;
  } else {
    int remaining=0;
    for(int i=0;i<r.count;++i) {
      if(r.minute[i]<now)continue;
      ++remaining;unknown|=r.code[i]==-1;
      if(!isRainCode(r.code[i]))continue;
      if(unknown)return text;
      std::strcpy(text.label,cached?"缓存降雨":"下次降雨");
      const time_t stamp=time_t(r.minute[i]*60);tm rain={};
#ifdef _WIN32
      localtime_s(&rain,&stamp);
#else
      localtime_r(&stamp,&rain);
#endif
      // Compare Beijing calendar dates, not elapsed 24-hour periods.
      tm today=*local,rainDay=rain;
      today.tm_hour=today.tm_min=today.tm_sec=0;
      rainDay.tm_hour=rainDay.tm_min=rainDay.tm_sec=0;
      const int days=int((std::mktime(&rainDay)-std::mktime(&today))/86400);
      if(days==0)std::snprintf(text.time,sizeof(text.time),"今天约%02d时",rain.tm_hour);
      else if(days==1)std::snprintf(text.time,sizeof(text.time),"明天约%02d时",rain.tm_hour);
      else std::snprintf(text.time,sizeof(text.time),"%d天后约%02d时",days,rain.tm_hour);
      probability=r.probability[i];found=true;break;
    }
    if(!found && remaining && !unknown) {
      const int hours=int((r.minute[r.count-1]-now)/60);
      if(hours>=24)std::snprintf(text.label,sizeof(text.label),"%s%d天暂无雨",cached?"缓存未来":"未来",hours/24);
      else std::snprintf(text.label,sizeof(text.label),"%s%d时暂无雨",cached?"缓存未来":"未来",hours);
    }
  }
  if(found && probability>=0)
    std::snprintf(text.probability,sizeof(text.probability),"概率%d%%%s",probability,
                  !cached && r.probabilityMinute<r.fetchedMinute?"旧":"");
  return text;
}
