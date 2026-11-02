#include "MoodCache.h"
#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <SPIFFS.h>
#include <freertos/semphr.h>
#include "cJSON.h"
#include "RemoteIdentity.h"
#include "CampusCertificates.h"
#include "CooperativeSecureClient.h"
#include "TlsTrust.h"
#include "FixedHttpClient.h"

// A file is the 4-byte version followed by the half bitmap. Version 0 means
// that person has not chosen yet (no file).
namespace {
constexpr size_t kFileBytes=4+kMoodHalfBytes;
SemaphoreHandle_t moodLock(){static SemaphoreHandle_t lock=xSemaphoreCreateMutex();return lock;}
uint32_t offered[2]={},stored[2]={};bool haveOffer=false,known[2]={};
volatile uint32_t generation=0;
void pathFor(uint8_t side,char *path){std::snprintf(path,16,"/mood%u.bin",unsigned(side));}
uint32_t fileVersion(uint8_t side){
  char path[16];pathFor(side,path);File f=SPIFFS.open(path,"r");uint32_t v=0;
  if(!f||f.size()!=kFileBytes||f.read(reinterpret_cast<uint8_t *>(&v),4)!=4)v=0;
  return v;
}
bool download(uint8_t side,uint32_t version){
  File file=SPIFFS.open("/mood.tmp","w");bool ok=false;
  if(file&&file.write(reinterpret_cast<const uint8_t *>(&version),4)==4){
    String url=kRemoteUrl;url=url.substring(0,url.lastIndexOf('/'))+"/mood/"+side;
    CooperativeSecureClient client;configureTlsTrust(client,url.c_str());client.setHandshakeTimeout(12);
    FixedHttpClient http;http.setConnectTimeout(10000);http.setTimeout(15000);http.setReuse(false);
    if(http.begin(client,url)){
      http.addHeader("Authorization",String("Bearer ")+kRemoteToken);
      const int status=http.GET();
      const int received=status==200&&http.getSize()==int(kMoodHalfBytes)?http.writeToStream(&file):-1;
      http.end();ok=received==int(kMoodHalfBytes);
      Serial.printf("[mood] side=%u version=%lu HTTP=%d bytes=%d heap=%u largest=%u\n",side,version,status,received,ESP.getFreeHeap(),ESP.getMaxAllocHeap());
    }
  }
  file.close();
  if(ok){
    ok=false;char path[16];pathFor(side,path);
    if(xSemaphoreTake(moodLock(),pdMS_TO_TICKS(2000))==pdTRUE){SPIFFS.remove(path);ok=SPIFFS.rename("/mood.tmp",path);xSemaphoreGive(moodLock());}
  }
  if(!ok)SPIFFS.remove("/mood.tmp");
  return ok;
}
}
void moodVersionsOffered(const char *json){
  cJSON *root=cJSON_Parse(json);const cJSON *list=root?cJSON_GetObjectItemCaseSensitive(root,"mood"):nullptr;
  if(cJSON_IsArray(list)&&cJSON_GetArraySize(list)==2){
    const cJSON *a=cJSON_GetArrayItem(list,0),*b=cJSON_GetArrayItem(list,1);
    if(cJSON_IsNumber(a)&&cJSON_IsNumber(b)&&a->valuedouble>=0&&b->valuedouble>=0&&a->valuedouble<4294967295.0&&b->valuedouble<4294967295.0){
      offered[0]=uint32_t(a->valuedouble);offered[1]=uint32_t(b->valuedouble);haveOffer=true;
    }
  }
  cJSON_Delete(root);
}
bool moodDownloadPending(){
  if(!haveOffer)return false;
  for(uint8_t side=0;side<2;++side){
    if(!known[side]){stored[side]=fileVersion(side);known[side]=true;}
    if(offered[side]!=stored[side])return true;
  }
  return false;
}
void moodDownload(){
  if(!haveOffer)return;
  for(uint8_t side=0;side<2;++side){
    if(!known[side]){stored[side]=fileVersion(side);known[side]=true;}
    if(offered[side]==stored[side])continue;
    if(!offered[side]){
      char path[16];pathFor(side,path);
      if(xSemaphoreTake(moodLock(),pdMS_TO_TICKS(2000))!=pdTRUE)continue;
      SPIFFS.remove(path);xSemaphoreGive(moodLock());
    } else if(!download(side,offered[side]))continue;
    stored[side]=offered[side];++generation;
  }
}
bool loadMoodHalf(uint8_t side,uint8_t *pixels){
  if(side>1||xSemaphoreTake(moodLock(),0)!=pdTRUE)return false;
  char path[16];pathFor(side,path);File f=SPIFFS.open(path,"r");
  bool ok=f&&f.size()==kFileBytes&&f.seek(4);
  // The half is 25 bytes per row; the framebuffer row is 50 bytes.
  for(unsigned y=0;ok&&y<300;++y)ok=f.read(pixels+y*50+side*25,25)==25;
  xSemaphoreGive(moodLock());return ok;
}
uint32_t moodGeneration(){return generation;}
#endif
