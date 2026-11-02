#include "AlbumCache.h"
#include "AlbumCachePolicy.h"
#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <SPIFFS.h>
#include <freertos/semphr.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include "RemoteIdentity.h"
#include "CampusCertificates.h"
#include "CooperativeSecureClient.h"
#include "TlsTrust.h"
#include "FixedHttpClient.h"

static SemaphoreHandle_t cacheLock=nullptr;
static bool mounted=false;
static uint8_t victim=0;
static volatile uint8_t cachedCount=0;
static void pathFor(uint8_t slot,char *path){std::snprintf(path,16,"/album%02u.bin",slot);}
static bool slotId(uint8_t slot,char *id) {
  char path[16];pathFor(slot,path);File f=SPIFFS.open(path,"r");
  return f&&f.size()==15065&&f.read(reinterpret_cast<uint8_t *>(id),65)==65&&id[64]==0&&validPhotoId(id);
}
bool startAlbumCache(const char *protectedId) {
  cacheLock=xSemaphoreCreateMutex();if(!cacheLock)return false;
  mounted=SPIFFS.begin(false);
  if(!mounted) {
    // Only initialize a verified erased partition; never format existing data.
    const auto *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,nullptr);
    bool erased=part;uint8_t block[256];
    for(size_t offset=0;erased&&offset<part->size;offset+=sizeof(block)) {
      if(esp_partition_read(part,offset,block,sizeof(block))!=ESP_OK){erased=false;break;}
      for(uint8_t byte:block)if(byte!=0xff){erased=false;break;}
      delay(1);
    }
    if(erased&&SPIFFS.format())mounted=SPIFFS.begin(false);
  }
  if(mounted){
    SPIFFS.remove("/album.tmp");
    cachedCount=trimAlbumCache(protectedId,slotId,[](uint8_t slot){char path[16];pathFor(slot,path);SPIFFS.remove(path);});
  }
  Serial.printf("[album] cache mounted=%d used=%u total=%u\n",mounted,unsigned(SPIFFS.usedBytes()),unsigned(SPIFFS.totalBytes()));
  return mounted;
}
static bool hashFile(File &file,const char *id,uint8_t *pixels=nullptr) {
  if(!file||file.size()!=15065||!file.seek(65))return false;
  mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts(&hash,0);
  uint8_t block[256],sum[32];size_t offset=0;bool ok=true;
  while(offset<15000) {
    const size_t count=std::min(size_t(15000-offset),sizeof(block));
    if(file.read(block,count)!=count){ok=false;break;}
    mbedtls_sha256_update(&hash,block,count);if(pixels)std::memcpy(pixels+offset,block,count);offset+=count;
  }
  mbedtls_sha256_finish(&hash,sum);mbedtls_sha256_free(&hash);
  char hex[65];for(unsigned i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",sum[i]);
  return ok&&!std::strcmp(hex,id);
}
uint8_t albumCacheCount() {
  // A download may own the filesystem lock; do not temporarily report zero.
  return cachedCount;
}
bool loadAlbumPhoto(const char *id,uint8_t *pixels) {
  if(!mounted||!validPhotoId(id)||xSemaphoreTake(cacheLock,0)!=pdTRUE)return false;
  bool ok=false;char saved[65],path[16];
  for(uint8_t i=0;i<20;++i)if(slotId(i,saved)&&!std::strcmp(id,saved)) {
    pathFor(i,path);File f=SPIFFS.open(path,"r");
    // Verify first, then fill the shared framebuffer; a damaged file cannot overwrite it.
    if(hashFile(f,id)&&f.seek(65))ok=f.read(pixels,15000)==15000;
    break;
  }
  xSemaphoreGive(cacheLock);return ok;
}
bool nextCachedPhoto(const char *current,bool random,char *next) {
  if(!mounted||xSemaphoreTake(cacheLock,0)!=pdTRUE)return false;
  uint8_t slots[20],count=0;int position=-1;char id[65];
  for(uint8_t i=0;i<20;++i)if(slotId(i,id)){
    char path[16];pathFor(i,path);File file=SPIFFS.open(path,"r");if(!hashFile(file,id))continue;
    if(!std::strcmp(id,current))position=count;slots[count++]=i;
  }
  bool ok=false;
  if(count) {
    unsigned selected=(position+1)%count;
    if(random&&count>1){selected=esp_random()%(position>=0?count-1:count);if(int(selected)>=position&&position>=0)++selected;}
    ok=slotId(slots[selected],next)&&std::strcmp(current,next);
  }
  xSemaphoreGive(cacheLock);return ok;
}
bool cacheAlbumPhoto(const char *id,const char *protectedId) {
  if(!mounted||!validPhotoId(id)||xSemaphoreTake(cacheLock,0)!=pdTRUE)return false;
  int target=-1;bool replacing=false;char saved[65],path[16];
  for(uint8_t i=0;i<20;++i) {
    if(!slotId(i,saved)){if(target<0)target=i;continue;}
    if(!std::strcmp(saved,id)){pathFor(i,path);File f=SPIFFS.open(path,"r");if(hashFile(f,id)){xSemaphoreGive(cacheLock);return true;}target=i;replacing=true;break;}
  }
  if(cachedCount>=kAlbumCacheLimit&&!replacing)target=-1;
  if(target<0)for(unsigned n=0;n<20;++n) {
    const uint8_t slot=(victim+n)%20;
    if(!slotId(slot,saved)||!std::strcmp(saved,protectedId))continue;
    target=slot;victim=(slot+1)%20;break;
  }
  bool ok=false;
  if(target>=0&&SPIFFS.totalBytes()-SPIFFS.usedBytes()>18000) {
    File file=SPIFFS.open("/album.tmp","w");
    if(file&&file.write(reinterpret_cast<const uint8_t *>(id),65)==65) {
      String url=kRemoteUrl;const int slash=url.lastIndexOf('/');url=url.substring(0,slash)+"/album/images/"+id;
      CooperativeSecureClient client;configureTlsTrust(client,url.c_str());client.setHandshakeTimeout(12);
      FixedHttpClient http;http.setConnectTimeout(10000);http.setTimeout(15000);http.setReuse(false);
      if(http.begin(client,url)){http.addHeader("Authorization",String("Bearer ")+kRemoteToken);const int status=http.GET();
        const int received=status==200&&http.getSize()==15000?http.writeToStream(&file):-1;
        file.close();http.end();File check=SPIFFS.open("/album.tmp","r");ok=received==15000&&hashFile(check,id);check.close();
        Serial.printf("[album] download HTTP=%d bytes=%d hash=%s heap=%u largest=%u\n",status,received,ok?"OK":"FAILED",ESP.getFreeHeap(),ESP.getMaxAllocHeap());
      }
    }
    file.close();
    if(ok){pathFor(target,path);SPIFFS.remove(path);ok=SPIFFS.rename("/album.tmp",path);}
    if(!ok)SPIFFS.remove("/album.tmp");
    cachedCount=0;for(uint8_t i=0;i<20;++i)if(slotId(i,saved))++cachedCount;
  }
  xSemaphoreGive(cacheLock);return ok;
}
void reconcileAlbumCache() {
  if(!mounted||xSemaphoreTake(cacheLock,0)!=pdTRUE)return;
  static uint8_t cursor=0;
  inspectAlbumCacheBatch(cursor,slotId,[](uint8_t i,const char *id) {
    char path[16];
    String url=kRemoteUrl;url=url.substring(0,url.lastIndexOf('/'))+"/album/images/"+id;
    CooperativeSecureClient client;configureTlsTrust(client,url.c_str());client.setHandshakeTimeout(12);
    FixedHttpClient http;http.setConnectTimeout(10000);http.setTimeout(15000);http.setReuse(false);
    if(http.begin(client,url)) {
      http.addHeader("Authorization",String("Bearer ")+kRemoteToken);
      const int status=http.sendRequest("HEAD");http.end();
      if(status<0)client.siteRequestFailed();
      if(albumPhotoAbsent(status)) {
        pathFor(i,path);
        if(SPIFFS.remove(path)){if(cachedCount)--cachedCount;Serial.printf("[album] removed unavailable cache id=%.8s count=%u\n",id,cachedCount);}
      } else if(status!=200) Serial.printf("[album] cache check HTTP=%d; retained\n",status);
    }
  });
  Serial.printf("[album] cache checked count=%u used=%u\n",cachedCount,unsigned(SPIFFS.usedBytes()));
  xSemaphoreGive(cacheLock);
}
#endif
