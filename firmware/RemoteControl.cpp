#include "RemoteControl.h"
#include "UsbLink.h"
#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <atomic>
#include "RecoveryPolicy.h"
#include "RemoteIdentity.h"
#include "CampusNetwork.h"
#include "CampusCertificates.h"
#include "ClockConfig.h"
#include "NetworkAccess.h"
#include "CooperativeSecureClient.h"
#include "TlsTrust.h"
#include "FixedHttpClient.h"
#include "AlbumCache.h"
#include "MoodCache.h"
#include "MoodTiming.h"
#include "WifiAwake.h"
#include "ScreenMirror.h"
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "../EdisonAlbum/OtaPolicy.h"
#include "../EdisonAlbum/FirmwareImage.h"

// OTA (V5.5). Static DRAM is full on this board, so all OTA state lives in one
// heap block and the 4KB chunk buffer exists only while downloading. Chunks
// reuse one TLS connection per batch; control sync keeps its own cadence.
namespace {
constexpr char kBoardName[]="mcwave";
__attribute__((used)) const char kBoardMarker[]="EPAPER-BOARD:mcwave";
constexpr uint16_t kBoardChipId=0;
std::atomic<uint32_t> lastSyncSuccessMs{0};
constexpr uint32_t kChunkMax=4096;
struct Ota {
  edison::OtaOffer offer,job;edison::OtaRecord rejected;
  bool offered=false,haveRejected=false,trial=false;
  std::atomic<bool> restart{false},active{false};
  const char *state="idle",*error="";char sha[65]={};
  uint32_t offset=0,chunk=kChunkMax;uint8_t refusals=0;uint8_t *buffer=nullptr;
  esp_ota_handle_t handle=0;const esp_partition_t *part=nullptr;mbedtls_sha256_context hash;
  firmwareImage::Check image{kBoardMarker,kBoardChipId};
};
Ota &ota(){static Ota *state=new Ota();return *state;}
bool otaLoad(const char *key,edison::OtaRecord &out){
  Preferences p;if(!p.begin("mcwave-ota",true))return false;
  const bool ok=p.isKey(key)&&p.getBytesLength(key)==sizeof(out)&&p.getBytes(key,&out,sizeof(out))==sizeof(out)&&out.magic==edison::OtaRecord{}.magic&&out.sha[64]==0&&out.version[32]==0;
  p.end();return ok;
}
void otaStore(const char *key,const edison::OtaRecord *record){
  Preferences p;if(!p.begin("mcwave-ota",false))return;
  if(record)p.putBytes(key,record,sizeof(*record));else if(p.isKey(key))p.remove(key);p.end();
}
edison::OtaRecord otaRecord(const edison::OtaOffer &o){edison::OtaRecord r;std::strcpy(r.sha,o.sha);std::strcpy(r.version,o.version);r.attempt=o.attempt;return r;}
void otaFinish(const char *state,const char *error,bool reject){
  Ota &o=ota();
  if(o.active){esp_ota_abort(o.handle);mbedtls_sha256_free(&o.hash);o.active=false;}
  free(o.buffer);o.buffer=nullptr;o.state=state;o.error=error;
  if(reject){o.rejected=otaRecord(o.job);o.haveRejected=true;otaStore("rej",&o.rejected);}
  Serial.printf("[ota] %s %s %s\n",state,o.job.version,error);
}
unsigned otaReportProgress(){
  const Ota &o=ota();
  if(o.active)return edison::otaProgress(o.offset,o.job.size);
  return !std::strcmp(o.state,"rebooting")||!std::strcmp(o.state,"installed")||!std::strcmp(o.state,"confirming")?100:0;
}
// One bounded slice (30s) of the update; returns after network losses and
// tries again on the next exchange.
void otaStep(){
  Ota &o=ota();
  if(o.offered&&!edison::offerBoardAllowed(o.offer,kBoardName)){
    if(o.active||!o.haveRejected||!edison::sameAttempt(o.rejected,o.offer)){
      if(!o.active){o.job=o.offer;std::strcpy(o.sha,o.job.sha);}
      otaFinish("failed","board",true);
    }
    return;
  }
  if(!o.active){
    if(!o.offered||!edison::otaShouldStart(o.offer,kFirmwareVersion,o.haveRejected?&o.rejected:nullptr,o.trial))return;
    o.job=o.offer;std::strcpy(o.sha,o.job.sha);
    const esp_partition_t *part=esp_ota_get_next_update_partition(nullptr);
    if(!part||o.job.size>part->size){otaFinish("failed","partition",true);return;}
    o.buffer=static_cast<uint8_t *>(malloc(kChunkMax));
    if(!o.buffer){o.state="downloading";o.error="memory";return;}
    if(esp_ota_begin(part,OTA_WITH_SEQUENTIAL_WRITES,&o.handle)!=ESP_OK){otaFinish("failed","begin",true);return;}
    o.part=part;o.offset=0;o.chunk=kChunkMax;o.refusals=0;o.active=true;o.image.reset();
    mbedtls_sha256_init(&o.hash);mbedtls_sha256_starts(&o.hash,0);o.state="downloading";o.error="";
    Serial.printf("[ota] start %s size=%lu partition=%s\n",o.job.version,o.job.size,part->label);
  }
  if(!o.offered||std::strcmp(o.offer.sha,o.job.sha)||o.offer.attempt!=o.job.attempt){otaFinish("idle","cancelled",false);return;}
  String base=kRemoteUrl;base=base.substring(0,base.lastIndexOf('/'))+"/firmware/"+o.job.sha;
  CooperativeSecureClient client;configureTlsTrust(client,base.c_str());client.setHandshakeTimeout(8);
  FixedHttpClient http;http.setConnectTimeout(8000);http.setTimeout(12000);http.setReuse(true);
  const uint32_t started=millis();
  while(o.offset<o.job.size&&!elapsed(millis(),started,edison::kOtaBatchMs)){
    const uint32_t n=edison::otaChunkLength(o.job.size,o.offset,o.chunk);
    char query[48];std::snprintf(query,sizeof(query),"?offset=%lu&length=%lu",o.offset,n);
    int status=-1;size_t got=0;
    if(http.begin(client,base+query)){
      http.addHeader("Authorization",String("Bearer ")+kRemoteToken);
      status=http.GET();
      if(status==200&&http.getSize()!=int(n))status=-102;
      if(status==200){
        // Receive the whole chunk before writing it: a broken transfer leaves no partial bytes.
        WiFiClient *stream=http.getStreamPtr();const uint32_t begun=millis();
        while(got<n&&stream&&(stream->connected()||stream->available())&&!elapsed(millis(),begun,12000)){
          const int r=stream->read(o.buffer+got,n-got);if(r>0)got+=size_t(r);else delay(2);
        }
        if(got!=n)status=-103;
      }
      if(status!=200)http.setReuse(false);
      http.end();
    }
    if(status!=200){
      o.chunk=edison::otaNextChunk(o.chunk,false,kChunkMax);
      if(!edison::otaServerRefusal(status))o.error="network";else if(++o.refusals>=edison::kOtaMaxRefusals)otaFinish("failed","server",true);
      Serial.printf("[ota] chunk at %lu failed HTTP=%d; next size=%lu\n",o.offset,status,o.chunk);
      return;
    }
    if(!o.image.accept(o.buffer,n)){otaFinish("failed","board",true);return;}
    if(esp_ota_write(o.handle,o.buffer,n)!=ESP_OK){otaFinish("failed","write",true);return;}
    mbedtls_sha256_update(&o.hash,o.buffer,n);o.offset+=n;o.refusals=0;o.error="";o.chunk=edison::otaNextChunk(o.chunk,true,kChunkMax);
  }
  Serial.printf("[ota] %s %u%% (%lu/%lu) heap=%u\n",o.job.version,edison::otaProgress(o.offset,o.job.size),o.offset,o.job.size,ESP.getFreeHeap());
  if(o.offset<o.job.size)return;
  uint8_t sum[32];char hex[65];mbedtls_sha256_finish(&o.hash,sum);mbedtls_sha256_free(&o.hash);
  for(unsigned i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",sum[i]);
  const esp_ota_handle_t handle=o.handle;o.active=false;free(o.buffer);o.buffer=nullptr;
  if(std::strcmp(hex,o.job.sha)){esp_ota_abort(handle);otaFinish("failed","sha256",true);return;}
  if(!o.image.complete()){esp_ota_abort(handle);otaFinish("failed","board",true);return;}
  if(esp_ota_end(handle)!=ESP_OK){otaFinish("failed","image",true);return;}
  const edison::OtaRecord attempt=otaRecord(o.job);otaStore("try",&attempt);
  if(esp_ota_set_boot_partition(o.part)!=ESP_OK){otaStore("try",nullptr);otaFinish("failed","boot",true);return;}
  o.state="rebooting";o.error="";o.restart=true;
  Serial.printf("[ota] %s verified; restarting into %s\n",o.job.version,o.part->label);
}
void otaConfirm(){
  Ota &o=ota();if(!o.trial)return;
  esp_ota_mark_app_valid_cancel_rollback();otaStore("try",nullptr);
  o.trial=false;o.state="installed";o.error="";
  Serial.printf("[ota] %s confirmed after reaching the website\n",kFirmwareVersion);
}
}
// Keep a trial image unconfirmed until the website answers (see otaConfirm).
extern "C" bool verifyRollbackLater(){return true;}
void remoteOtaBoot(){
  Serial.println(kBoardMarker); // A live reference also retains the used array through linker GC.
  lastSyncSuccessMs.store(millis()); // Boot starts a fresh 60-minute grace period.
  Ota &o=ota();esp_ota_img_states_t state;const esp_partition_t *running=esp_ota_get_running_partition();
  const bool pendingVerify=esp_ota_get_state_partition(running,&state)==ESP_OK&&state==ESP_OTA_IMG_PENDING_VERIFY;
  edison::OtaRecord attempt;const bool tried=otaLoad("try",attempt);o.haveRejected=otaLoad("rej",o.rejected);
  switch(edison::classifyBoot(pendingVerify,tried?&attempt:nullptr,kFirmwareVersion)){
    case edison::BootOutcome::Trial:o.trial=true;o.state="confirming";if(tried)std::strcpy(o.sha,attempt.sha);break;
    case edison::BootOutcome::RolledBack:o.rejected=attempt;o.haveRejected=true;otaStore("rej",&o.rejected);otaStore("try",nullptr);
      o.state="rejected";o.error="rollback";std::strcpy(o.sha,attempt.sha);break;
    case edison::BootOutcome::Normal:if(tried)otaStore("try",nullptr);break;
  }
  Serial.printf("[ota] running=%s state=%s\n",running?running->label:"?",o.state);
}
void remoteOtaWatch(){
  // Run in loopTask even if the remote task no longer makes progress.
  // Read the cross-task timestamp before now, avoiding a newer success being
  // mistaken for a millis wrap and an immediate expiry.
  const uint32_t lastSuccess=lastSyncSuccessMs.load();
  if(mcwaveRecovery::syncExpired(millis(),lastSuccess,ota().active.load()||ota().restart.load())&&
     lastSyncSuccessMs.load()==lastSuccess) {
    Serial.println("[watchdog] no successful website sync for 60 minutes; restarting");
    Serial.flush();esp_restart();
  }
  if(ota().trial&&edison::otaTrialExpired(millis())){
    Serial.println("[ota] trial image never reached the website; rolling back");Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

struct RemoteReport {uint32_t revision=0;bool mood=false;};
static portMUX_TYPE remoteMux=portMUX_INITIALIZER_UNLOCKED;
static RemoteReport report;
static RemoteCommand pending;
static bool pendingReady=false;
static uint32_t attemptMs=0;
static bool attempted=false;
static bool cacheChecked=false;
static uint32_t cacheCheckMs=0;
static RemoteCommand albumReport;
static uint32_t albumShownMs=0;
bool remoteStartAlbum(RemoteCommand &command) {
  Preferences cache;bool ok=false;
  if(cache.begin("album-v5",true)) {
    if(cache.getBytesLength("view")==sizeof(command)&&cache.getBytes("view",&command,sizeof(command))==sizeof(command)&&command.album&&validPhotoId(command.photoId)&&
      (command.interval==60||command.interval==300||command.interval==600))ok=true;
    cache.end();
  }
  // The startup frame must render (or download again) before this revision is
  // acknowledged after a cold boot. A missing/damaged cache remains retryable.
  if(ok)report.revision=command.revision?command.revision-1:0;
  return ok;
}
static void persistAlbum(const RemoteCommand &command) {
  Preferences cache;if(cache.begin("album-v5",false))cache.putBytes("view",&command,sizeof(command));cache.end();
}
void remoteAlbumRendered(const RemoteCommand &command) {
  portENTER_CRITICAL(&remoteMux);
  const bool changed=std::strcmp(albumReport.photoId,command.photoId)||albumReport.revision!=command.revision||albumReport.paused!=command.paused||albumReport.random!=command.random||albumReport.interval!=command.interval;
  albumReport=command;albumReport.automatic=false;report.mood=false;
  report.revision=command.revision;albumShownMs=millis();
  const RemoteReport saved=report;
  portEXIT_CRITICAL(&remoteMux);
  if(changed){persistAlbum(albumReport);Preferences cache;if(cache.begin("remote-v4",false))cache.putBytes("view",&saved,sizeof(saved));cache.end();}
  Serial.printf("[album] rendered revision=%lu id=%.8s paused=%d interval=%u cache=%u heap=%u largest=%u\n",command.revision,command.photoId,command.paused,command.interval,albumCacheCount(),ESP.getFreeHeap(),ESP.getMaxAllocHeap());
}

void startRemoteControl(bool &mood) {
  Preferences cache;
  if(cache.begin("remote-v4",true)) {
    RemoteReport saved;
    // A 5.6 record (sports/page fields) has another size and is ignored: weather.
    if(cache.getBytesLength("view")==sizeof(saved)&&cache.getBytes("view",&saved,sizeof(saved))==sizeof(saved))report=saved;
    cache.end();
  }
  mood=report.mood;
}
static void remoteTask(void *) {
  for(;;){exchangeRemoteControl();vTaskDelay(pdMS_TO_TICKS(2000));}
}
void startRemoteTask() {
  if(xTaskCreate(remoteTask,"remote",8192,nullptr,1,nullptr)!=pdPASS)Serial.println("[remote] task could not start");
}
bool takeRemoteCommand(RemoteCommand &command) {
  portENTER_CRITICAL(&remoteMux);
  const bool ready=pendingReady&&(pending.revision>report.revision||pending.automatic);
  if(ready)command=pending;
  pendingReady=false;portEXIT_CRITICAL(&remoteMux);return ready;
}
void remoteRendered(bool mood,uint32_t revision) {
  portENTER_CRITICAL(&remoteMux);
  const bool changed=albumReport.album||report.mood!=mood||revision>report.revision;
  report.mood=mood;
  const bool wasAlbum=albumReport.album;albumReport.album=false;
  if(revision>report.revision)report.revision=revision;
  const RemoteReport saved=report;
  portEXIT_CRITICAL(&remoteMux);
  if(wasAlbum)persistAlbum(albumReport);
  if(changed){Preferences cache;if(cache.begin("remote-v4",false))cache.putBytes("view",&saved,sizeof(saved));cache.end();}
  // Report only after the physical refresh and sleep have completed.
  if(revision)Serial.printf("[remote] rendered revision=%lu mode=%s\n",revision,mood?"mood":"weather");
}
class RemoteOutput:public Stream {
 public:
  char body[512]={};size_t size=0;
  size_t write(uint8_t c) override{if(size>=sizeof(body)-1)return 0;body[size++]=char(c);body[size]=0;return 1;}
  size_t write(const uint8_t *data,size_t count) override{size_t i=0;while(i<count&&write(data[i]))++i;return i;}
  int available() override{return 0;}int read() override{return -1;}int peek() override{return -1;}void flush() override{}
};
// 6.0.1: keep the website connection between syncs, as edison 7.0.1 does: new
// TLS connections from the campus network often fail, a kept one rarely does.
struct SyncTransport {CooperativeSecureClient client;FixedHttpClient http;};
static SyncTransport &syncTransport(){static SyncTransport *t=new SyncTransport();return *t;}
static ScreenMirrorPolicy &screenMirror(){static ScreenMirrorPolicy *s=new ScreenMirrorPolicy();return *s;}
static bool screenAllowed=false;
void remoteScreenAllow(bool allow){portENTER_CRITICAL(&remoteMux);screenAllowed=allow;portEXIT_CRITICAL(&remoteMux);}
void remoteScreenPreparing(){screenMirror().invalidate();remoteScreenAllow(false);}
void remoteScreenRendered(const uint8_t *frame,const char *mode){char hash[17];screenFrameHash(frame,hash);screenMirror().rendered(frame,hash,mode);}
// Called last under NetworkGuard. Best effort, no retry, no second TLS client.
static void uploadScreen(){
  auto &s=screenMirror();auto &t=syncTransport();
  portENTER_CRITICAL(&remoteMux);const bool allowed=screenAllowed&&!pendingReady;portEXIT_CRITICAL(&remoteMux);
  if(!allowed||!s.due(millis())||!t.client.connected())return;
  String url=kRemoteUrl;const int api=url.indexOf("/api/");if(api<0)return;url=url.substring(0,api)+"/api/v2/device/screen";
  s.attempt(millis());
  // Match a warm sync; syncOnce reapplies its warm/cold timeouts before reuse.
  t.client.setTimeout(10000);t.client.setHandshakeTimeout(12);
  auto &http=t.http;http.setConnectTimeout(10000);http.setTimeout(10000);http.setReuse(true);
  if(!http.begin(t.client,url))return;
  http.addHeader("Authorization",String("Bearer ")+kRemoteToken);http.addHeader("Content-Type","application/octet-stream");
  http.addHeader("X-Frame-Hash",s.hash);http.addHeader("X-Frame-Mode",s.mode);
  const int code=http.sendRequest("PUT",const_cast<uint8_t *>(s.frame),15000);
  if(code!=204)http.setReuse(false);http.end();
  Serial.printf("[screen-mirror] HTTP=%d hash=%s\n",code,s.hash);
}
// 6.1.2: the kept connection holds ~40KB. Free it before any other TLS request
// (with it open a weather query had only ~25KB left). Caller holds NetworkGuard.
static void dropSync(){auto &t=syncTransport();if(t.client.connected())t.client.stop();}
void remoteDropConnection(){NetworkGuard network(pdMS_TO_TICKS(30000));if(network)dropSync();}
static bool syncOnce(const RemoteReport &snapshot,const RemoteCommand &album,RemoteCommand &command,bool &stale) {
  const Ota &o=ota();
  char body[480];std::snprintf(body,sizeof(body),"{\"mode\":\"%s\",\"page\":0,\"pageCount\":1,\"appliedRevision\":%lu,\"firmware\":\"%s\",\"photoId\":\"%s\",\"cacheCount\":%u,\"ota\":{\"state\":\"%s\",\"progress\":%u,\"sha256\":\"%s\",\"error\":\"%s\"}}",
    album.album?"album":snapshot.mood?"mood":"weather",snapshot.revision,kFirmwareVersion,album.album?album.photoId:"",albumCacheCount(),
    o.state,otaReportProgress(),o.sha,o.error);
  auto &client=syncTransport().client;auto &http=syncTransport().http;
  configureTlsTrust(client,kRemoteUrl);client.setHandshakeTimeout(12);
  // 6.1.2: log whether the kept connection was reused and how long the exchange took.
  const bool warm=client.connected();const uint32_t started=millis();
  // 6.1.6: a silently dead kept connection is given 10 s, then a fresh one is tried.
  http.setConnectTimeout(10000);http.setTimeout(warm?10000:15000);http.setReuse(true);
  if(!http.begin(client,kRemoteUrl))return false;
  http.addHeader("Authorization",String("Bearer ")+kRemoteToken);http.addHeader("Content-Type","application/json");
  const int status=http.POST(reinterpret_cast<uint8_t *>(body),std::strlen(body));
  RemoteOutput output;const int bytes=status==200?http.writeToStream(&output):-1;
  // Only a fully read 200 keeps the connection; anything else reconnects next time.
  if(status!=200||bytes<0)http.setReuse(false);
  http.end();
  const bool ok=status==200&&bytes>=0&&parseRemoteCommand(output.body,command);
  if(!ok&&!warm&&status<0)client.siteRequestFailed();
  if(ok){lastSyncSuccessMs.store(millis());ota().offered=edison::parseOtaOffer(output.body,ota().offer);moodVersionsOffered(output.body);}
  const unsigned long ms=millis()-started;
  if(ok)Serial.printf("[remote] sync OK revision=%lu applied=%lu reuse=%d ms=%lu\n",command.revision,snapshot.revision,warm,ms);
  else {
    char detail[160];int tls=client.lastError(detail,sizeof(detail));
    if(tls>=0){tls=0;std::strcpy(detail,"no TLS connect error");}
    Serial.printf("[remote] sync failed HTTP=%d TLS=%d %s reuse=%d ms=%lu; current screen retained\n",status,tls,detail,warm,ms);
  }
  // A kept connection can fail at once (closed by the server, HTTP=-5) or go silent
  // (6.1.5 daytime: 17 of 24 failures were HTTP=-11 on a reused connection).
  stale=!ok&&warm&&status<0;
  return ok;
}
// 6.1.4/6.1.6: a failure on a kept connection is retried once on a fresh one.
// Safe to repeat: the legacy sync is a state report plus a read.
static bool syncControl(const RemoteReport &snapshot,const RemoteCommand &album,RemoteCommand &command) {
  bool stale=false;const uint32_t started=millis();unsigned attempts=1;
  bool ok=syncOnce(snapshot,album,command,stale);
  if(!ok&&stale){
    Serial.println("[remote] kept connection failed; retrying once on a fresh one");
    ++attempts;ok=syncOnce(snapshot,album,command,stale);
  }
  Serial.printf("[remote] round ok=%d http_attempts=%u ms=%lu\n",ok,attempts,millis()-started);
  return ok;
}
static bool nextCloudPhoto(const RemoteCommand &album,char *id) {
  String url=kRemoteUrl;url=url.substring(0,url.lastIndexOf('/'))+"/album/next?current="+album.photoId+"&random="+(album.random?"1":"0");
  CooperativeSecureClient client;configureTlsTrust(client,url.c_str());client.setHandshakeTimeout(12);
  FixedHttpClient http;http.setConnectTimeout(10000);http.setTimeout(15000);http.setReuse(false);
  bool ok=false;
  if(http.begin(client,url)){http.addHeader("Authorization",String("Bearer ")+kRemoteToken);
    RemoteOutput output;const int status=http.GET();const int bytes=status==200?http.writeToStream(&output):-1;http.end();
    cJSON *root=bytes>=0?cJSON_Parse(output.body):nullptr;const auto *value=root?cJSON_GetObjectItemCaseSensitive(root,"id"):nullptr;
    if(cJSON_IsString(value)&&validPhotoId(value->valuestring)){std::strcpy(id,value->valuestring);ok=true;}cJSON_Delete(root);
  }
  return ok;
}
void exchangeRemoteControl() {
  if(!*kRemoteToken)return;
  portENTER_CRITICAL(&remoteMux);const RemoteReport snapshot=report;const RemoteCommand album=albumReport;const bool waiting=pendingReady;const uint32_t shown=albumShownMs;portEXIT_CRITICAL(&remoteMux);
  if(attempted&&!elapsed(millis(),attemptMs,moodTiming::syncInterval(snapshot.mood&&!album.album)))return;
  NetworkGuard network(0);if(!network)return;
  attempted=true;attemptMs=millis();
  const bool online=(usbLinkActive()||WiFi.status()==WL_CONNECTED)&&campusAuthenticated()&&campusClockReady();
  WifiAwake awake;
  RemoteCommand command;
  const bool synced=online&&syncControl(snapshot,album,command);
  if(synced){otaConfirm();if(moodDownloadPending()){
    const uint32_t before=moodGeneration();dropSync();moodDownload();
    // Both halves are complete before releasing the lock to the UI. Do not
    // hold a newly ready mood frame behind album reconciliation or OTA work.
    if(snapshot.mood&&!album.album&&command.mood&&!command.album&&moodGeneration()!=before&&!ota().offered&&!ota().active&&!ota().restart){
      if(command.revision>snapshot.revision){portENTER_CRITICAL(&remoteMux);pending=command;pendingReady=true;portEXIT_CRITICAL(&remoteMux);}
      return;
    }
  }}
  // 6.1.3: each check closes the kept connection; outside album mode once per 10 min is enough.
  if(online&&(!cacheChecked||elapsed(millis(),cacheCheckMs,album.album?60000:600000))) {
    cacheChecked=true;cacheCheckMs=millis();dropSync();reconcileAlbumCache();
  }
  // Firmware download only when no new display command arrived this round.
  if(synced&&command.revision<=snapshot.revision){if(ota().offered||ota().active)dropSync();otaStep();}
  if(ota().restart){RemoteCommand ignored;syncControl(snapshot,album,ignored);delay(300);Serial.println("[ota] restarting");Serial.flush();esp_restart();}
  const bool albumDue=album.album&&!album.paused&&elapsed(millis(),shown,uint32_t(album.interval)*1000);
  if(synced&&!waiting&&command.revision<=snapshot.revision&&!moodDownloadPending()&&!albumDue&&!ota().offered&&!ota().active&&!ota().restart)uploadScreen();
  if(synced&&command.revision>snapshot.revision) {
    if(command.album){dropSync();if(!cacheAlbumPhoto(command.photoId,album.photoId))return;}
  } else {
    if(waiting||!album.album||album.paused||!elapsed(millis(),shown,uint32_t(album.interval)*1000))return;
    dropSync();command=album;command.automatic=true;char next[65]={};
    bool found=synced&&nextCloudPhoto(album,next);
    if(!found)found=nextCachedPhoto(album.photoId,album.random,next);
    if(!found||!std::strcmp(next,album.photoId))return;
    if(!cacheAlbumPhoto(next,album.photoId))return;
    std::strcpy(command.photoId,next);
  }
  portENTER_CRITICAL(&remoteMux);pending=command;pendingReady=true;portEXIT_CRITICAL(&remoteMux);
}
#endif
