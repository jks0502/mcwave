#pragma once
#include "ClockConfig.h"
#include "SportsData.h"
#include "WeatherData.h"

inline uint32_t sportsRefreshInterval(const SportsData &data) {
  return data.valid && data.hasLive()?kSportsLiveIntervalMs:kSportsIdleIntervalMs;
}

// ISO Beijing timestamps compare in chronological order. Keep only the next
// kickoff; rebuild it when data arrives, rather than scanning on every loop.
class SportsRefresh {
  uint32_t completedMs_=0;
  bool requested_=false,pending_=false,forced_=true;
  char checkedThrough_[20]={},nextStart_[20]={};
  static void stamp(const tm &local,char *out){std::strftime(out,20,"%Y-%m-%d %H:%M:%S",&local);}
  static bool validStart(const char *start) {
    if(std::strlen(start)!=19 || start[10]!=' ' || start[16]!=':' || start[17]<'0' || start[17]>'5' || start[18]<'0' || start[18]>'9')return false;
    char minute[17];std::memcpy(minute,start,16);minute[10]='T';minute[16]=0;
    int64_t unused;return weatherMinute(minute,unused);
  }
 public:
  void force(){forced_=true;}
  uint32_t interval(const SportsData &data) const {
    return sportsRefreshInterval(data);
  }
  bool due(const SportsData &data,uint32_t nowMs,const tm &local) const {
    if(pending_)return false;
    if(forced_ || !requested_ || uint32_t(nowMs-completedMs_)>=interval(data))return true;
    char now[20];stamp(local,now);
    return *nextStart_ && std::strcmp(now,nextStart_)>=0;
  }
  void requested(const tm &local){stamp(local,checkedThrough_);requested_=pending_=true;forced_=false;}
  void received(const SportsData &data,uint32_t nowMs,bool fresh) {
    // A startup-cache delivery does not complete an active network request.
    if(fresh && pending_){pending_=false;completedMs_=nowMs;}
    nextStart_[0]=0;
    if(!data.valid)return;
    for(int i=0;i<data.count;++i) {
      const auto &e=data.events[i];
      if(e.live || std::strcmp(e.status,"未开始") || !validStart(e.start) || std::strcmp(e.start,checkedThrough_)<=0)continue;
      if(!*nextStart_ || std::strcmp(e.start,nextStart_)<0)std::strcpy(nextStart_,e.start);
    }
  }
  const char *nextStart() const{return nextStart_;}
  bool pending() const{return pending_;}
};
