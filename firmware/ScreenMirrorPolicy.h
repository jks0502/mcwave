#pragma once
#include <cstdint>
#include <cstring>
// No frame allocation: the owner supplies stable bytes until invalidate().
// All access is serialized by the existing NetworkGuard.
struct ScreenMirrorPolicy {
  const uint8_t *frame=nullptr;
  char hash[17]={},mode[12]={},lastHash[17]={};
  uint32_t attemptMs=0;bool pending=false,attempted=false;
  void invalidate(){pending=false;frame=nullptr;}
  void rendered(const uint8_t *pixels,const char *digest,const char *view){
    frame=pixels;std::strncpy(hash,digest,16);hash[16]=0;
    std::strncpy(mode,view,11);mode[11]=0;pending=true;
  }
  bool due(uint32_t now)const{return pending&&frame&&std::strcmp(hash,lastHash)&&(!attempted||uint32_t(now-attemptMs)>=60000);}
  void attempt(uint32_t now){pending=false;attempted=true;attemptMs=now;std::strcpy(lastHash,hash);}
};
