#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include "cJSON.h"

struct RemoteCommand {
  uint32_t revision=0; bool sports=false; uint8_t page=0;
  bool album=false,paused=false,random=false; uint16_t interval=60;
  char photoId[65]={}; bool automatic=false;
  bool mood=false; // mcwave 6.0.0
};
inline bool validPhotoId(const char *id) {
  if(!id||std::strlen(id)!=64)return false;
  for(unsigned i=0;i<64;++i)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
  return true;
}
inline bool parseRemoteCommand(const char *json,RemoteCommand &command) {
  cJSON *root=cJSON_Parse(json);if(!root)return false;
  const cJSON *rev=cJSON_GetObjectItemCaseSensitive(root,"revision");
  const cJSON *mode=cJSON_GetObjectItemCaseSensitive(root,"mode");
  const cJSON *page=cJSON_GetObjectItemCaseSensitive(root,"page");
  const bool valid=cJSON_IsNumber(rev)&&rev->valuedouble>=0&&rev->valuedouble<=4294967295.0&&std::floor(rev->valuedouble)==rev->valuedouble&&
    cJSON_IsString(mode)&&(!std::strcmp(mode->valuestring,"weather")||!std::strcmp(mode->valuestring,"sports")||!std::strcmp(mode->valuestring,"album")||!std::strcmp(mode->valuestring,"mood"))&&
    cJSON_IsNumber(page)&&page->valuedouble>=0&&page->valuedouble<64&&std::floor(page->valuedouble)==page->valuedouble&&
    (!std::strcmp(mode->valuestring,"sports")||page->valuedouble==0);
  bool accepted=valid;
  RemoteCommand parsed;
  if(valid) {
    parsed={uint32_t(rev->valuedouble),!std::strcmp(mode->valuestring,"sports"),uint8_t(page->valuedouble)};
    parsed.album=!std::strcmp(mode->valuestring,"album");
    parsed.mood=!std::strcmp(mode->valuestring,"mood");
    if(parsed.album) {
      const auto *id=cJSON_GetObjectItemCaseSensitive(root,"photoId"),*pause=cJSON_GetObjectItemCaseSensitive(root,"paused"),
        *random=cJSON_GetObjectItemCaseSensitive(root,"random"),*interval=cJSON_GetObjectItemCaseSensitive(root,"interval");
      accepted=cJSON_IsString(id)&&validPhotoId(id->valuestring)&&cJSON_IsBool(pause)&&cJSON_IsBool(random)&&cJSON_IsNumber(interval)&&
        (interval->valuedouble==60||interval->valuedouble==300||interval->valuedouble==600);
      if(accepted){std::strcpy(parsed.photoId,id->valuestring);parsed.paused=cJSON_IsTrue(pause);parsed.random=cJSON_IsTrue(random);parsed.interval=uint16_t(interval->valuedouble);}
    }
  }
  if(accepted)command=parsed;
  cJSON_Delete(root);return accepted;
}
inline bool applyRemoteCommand(const RemoteCommand &command,uint32_t applied,uint8_t pageCount,bool &sports,uint8_t &page) {
  if(command.revision<=applied)return false;
  sports=command.sports;page=sports&&pageCount?uint8_t(command.page<pageCount?command.page:pageCount-1):0;
  return true;
}
