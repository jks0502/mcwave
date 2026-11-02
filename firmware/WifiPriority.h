#pragma once
#include <cstdint>

// Ordered profiles: NJIT, moyimi. Never preempt a ready connection.
class WifiPriority {
  uint8_t tried_=0;
  int current_=-1;
  bool ready_=false,authWaiting_=false,waiting_=false;
  uint32_t attempt_=0,authSince_=0,pause_=0;
 public:
  bool selectionDue(bool linked,bool ready,uint32_t now) {
    if(linked&&ready){ready_=true;authWaiting_=false;return false;}
    if(ready_&&!linked){tried_=0;current_=-1;ready_=false;waiting_=false;}
    if(linked){
      if(!authWaiting_){authWaiting_=true;authSince_=now;}
      return uint32_t(now-authSince_)>=120000;
    }
    authWaiting_=false;
    return current_>=0?uint32_t(now-attempt_)>=30000:!waiting_||uint32_t(now-pause_)>=30000;
  }
  int select(uint8_t visible,uint32_t now,uint8_t eligible=3) {
    ready_=false;authWaiting_=false;waiting_=false;
    // Prefer visible targets; if no untried target is visible, use a directed
    // SSID attempt (hidden APs and stale scans must not prevent connection).
    for(int pass=0;pass<2;++pass)for(int i=0;i<2;++i)
      if((eligible&(1U<<i))&&!(tried_&(1U<<i))&&(pass||visible&(1U<<i))){
        tried_|=uint8_t(1U<<i);current_=i;attempt_=now;return i;
      }
    // Exhausted one pass. Back off, then restart from the highest priority.
    tried_=0;current_=-1;waiting_=true;pause_=now;return -1;
  }
};
