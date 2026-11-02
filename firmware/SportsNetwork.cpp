#include <Arduino.h>
#include <Preferences.h>
#include "SportsNetwork.h"
#include "DomesticNetwork.h"
#include "NetworkAccess.h"
#include "ClockConfig.h"
#include "RemoteControl.h"

// edison's 20KB NVS also holds its album and OTA state, so it does not keep
// the (up to 14KB) feed there and starts without a saved feed.
#ifndef SPORTS_FEED_NVS
#define SPORTS_FEED_NVS 1
#endif
static QueueHandle_t requests=nullptr;
static SemaphoreHandle_t lock=nullptr;
static SemaphoreHandle_t consumed=nullptr;
static std::string pendingBody;
static bool ready=false;
static char pendingDate[11]={};
struct SportsRequest { char date[11]; };

static void sportsTask(void *) {
  Preferences cache;
  if(SPORTS_FEED_NVS && cache.begin("sports-cn-v31",true)) {
    const size_t length=cache.getBytesLength("feed");
    if(length && length<=14000) {
      std::string saved(length,'\0');
      if(cache.getBytes("feed",saved.data(),length)==length) {
        xSemaphoreTake(lock,portMAX_DELAY);
        pendingBody=std::move(saved);ready=true;
        xSemaphoreGive(lock);
      }
    }
    cache.end();
  }
  SportsRequest request;
  for(;;) {
#ifdef ESP_PLATFORM
    exchangeRemoteControl();
    if(xQueueReceive(requests,&request,pdMS_TO_TICKS(2000))!=pdTRUE)continue;
#else
    if(xQueueReceive(requests,&request,portMAX_DELAY)!=pdTRUE)continue;
#endif
    std::string body=fetchDomesticSports(request.date);
    xSemaphoreTake(lock,portMAX_DELAY);
    pendingBody=std::move(body);std::strcpy(pendingDate,request.date);ready=true;
    xSemaphoreGive(lock);
    // Let the UI consume this result before another TLS round claims the heap.
    xSemaphoreTake(consumed,portMAX_DELAY);
  }
}
void startSportsNetwork() {
  requests=xQueueCreate(1,sizeof(SportsRequest));lock=xSemaphoreCreateMutex();consumed=xSemaphoreCreateBinary();
  if(!requests||!lock||!consumed||xTaskCreate(sportsTask,"sports",8192,nullptr,1,nullptr)!=pdPASS)Serial.println("[sports] task could not start");
}
void requestSports(const char *date) {
  if(!requests)return;SportsRequest r={};std::snprintf(r.date,sizeof(r.date),"%s",date);xQueueOverwrite(requests,&r);
}
bool pollSports(SportsData &data,bool &failed,bool *freshResult) {
  if(!lock||xSemaphoreTake(lock,0)!=pdTRUE)return false;
  if(!ready){xSemaphoreGive(lock);return false;}
  // Parse on the UI task: only the UI task owns the full event array. The
  // background task retains a compact JSON message instead of another 40 KB array.
  NetworkGuard network(0);
  if(!network){xSemaphoreGive(lock);return false;}
  const bool ok=!pendingBody.empty() && parseSports(pendingBody.c_str(),data,*pendingDate?pendingDate:nullptr);
  const bool fresh=*pendingDate;
  if(freshResult)*freshResult=fresh;
  if(ok)data.cached=!fresh;
  failed=!ok;ready=false;
  Serial.printf("[sports] parse=%s events=%u errors=%u cached=%d heap=%u largest=%u\n",ok?"OK":"FAILED",data.count,data.errors,data.cached,ESP.getFreeHeap(),ESP.getMaxAllocHeap());
  // Persist each complete successful query so a restart restores the latest feed.
  if(SPORTS_FEED_NVS && ok && fresh && !data.errors) {
    Preferences cache;
    if(pendingBody.length()<=14000 && cache.begin("sports-cn-v31",false) && cache.putBytes("feed",pendingBody.c_str(),pendingBody.length())==pendingBody.length()) {
      Serial.println("[sports] latest complete feed saved");
    } else Serial.println("[sports] persistent cache full; current complete feed remains in RAM");
    cache.end();
  }
  std::string().swap(pendingBody);xSemaphoreGive(lock);
  if(fresh)xSemaphoreGive(consumed);
  return true;
}
