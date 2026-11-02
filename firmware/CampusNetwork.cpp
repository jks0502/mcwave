#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <atomic>
#include <ctime>
#include <sys/time.h>
#include <esp_sntp.h>
#include "CampusConfig.h"
#include "CampusNetwork.h"
#if defined(MCWAVE_MULTI_NETWORK)
#include "UsbLink.h"
#else
// Edison shares campus authentication, but has no mcwave USB gateway.
static bool usbLinkActive(){return false;}
static bool usbLinkClockReady(){return false;}
#endif
#include "CampusCertificates.h"
#include "CooperativeSecureClient.h"
#include "TlsTrust.h"
#include "NetworkAccess.h"
#include "FixedHttpClient.h"

static CampusSession session;
static std::atomic<bool> synced{false};
static bool savedTime=false;
static bool timeSaveAttempted=false;
static uint32_t timeSaveAttemptMs=0;
static std::atomic<bool> authenticated{false};
void resetCampusNetwork(){authenticated.store(false);session.resetLink();}
uint32_t campusRetryRemaining(uint32_t now){return session.retryRemaining(now);}
static bool portalLogin(const char *url){return std::strstr(url,"a=login")||std::strstr(url,"/portal/login?");}
class PortalBody:public Stream {
 public:
  std::string body;
  size_t write(uint8_t c)override {if(body.size()>=4096)return 0;body+=char(c);return 1;}
  size_t write(const uint8_t *bytes,size_t count)override {
    if(count>4096-body.size())return 0;body.append(reinterpret_cast<const char *>(bytes),count);return count;
  }
  int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}
};
static bool portalQueryOnce(const char *url,std::string &body,bool &retryable) {
  retryable=false;
  CooperativeSecureClient client;NetworkClient plain;
  const bool tls=std::strncmp(url,"https://",8)==0;
  if(tls){configureTlsTrust(client,url);client.setHandshakeTimeout(6);}
  // Only the explicitly selected NJIT portal may use its HTTP transport.
  else if(!campusUsesNjit() || (std::strncmp(url,"http://172.31.255.156/drcom/",28)!=0&&
                          std::strncmp(url,"http://172.31.255.156:801/eportal/portal/",41)!=0))return false;
  FixedHttpClient http;http.setConnectTimeout(6000);http.setTimeout(6000);http.setReuse(false);
  // Never follow a redirect carrying credentials or print the login URL.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if(!http.begin(tls?static_cast<NetworkClient&>(client):plain,url))return false;
  const char *keys[]={"Date"};http.collectHeaders(keys,1);
  const int code=http.GET();PortalBody output;
  const int bytes=code==200&&http.getSize()<=4096?http.writeToStream(&output):-1;
  retryable=code<0||code>=500||(code==200&&bytes<0);
  if(tls&&code==200&&!synced.load()) {
    const time_t stamp=portalHttpTime(http.header("Date").c_str());
    if(stamp){timeval exact={stamp,0};settimeofday(&exact,nullptr);synced.store(true);Serial.println("[campus] clock synchronized from verified HTTPS Date");}
  }
  if(tls&&code<0){char detail[160];const int error=client.lastError(detail,sizeof(detail));Serial.printf("[campus] TLS=%d %s\n",error,detail);}
  http.end();Serial.printf("[campus] %s HTTP=%d bytes=%d\n",portalLogin(url)?"login":"status",code,bytes);
  if(code!=200||bytes<0)return false;body=output.body;return true;
}
static bool portalQuery(const char *url,std::string &body) {
  bool retryable=false;
  if(portalQueryOnce(url,body,retryable))return true;
  // Retry only read-only status requests, never replay a credentialed login.
  if(!retryable||portalLogin(url))return false;
  delay(200);body.clear();
  Serial.println("[campus] retrying status check with a fresh verified TLS connection");
  return portalQueryOnce(url,body,retryable);
}
void startCampusNetwork() {
  if(!campusUsesPortal())return;
  const esp_err_t trust=esp_crt_bundle_set(kCampusCaBundle,sizeof(kCampusCaBundle));
  Serial.printf("[tls] indexed CA bundle %s bytes=%u\n",trust==ESP_OK?"installed":"REJECTED",unsigned(sizeof(kCampusCaBundle)));
  esp_sntp_set_time_sync_notification_cb([](struct timeval *){synced.store(true);});
  // Seed certificate validity checks only. UI/queries wait for SNTP or an
  // actual Date header from the certificate-verified campus HTTPS server.
  setenv("TZ","CST-8",1);tzset();
  char month[4];int day=0,year=0,hour=0,minute=0,second=0;
  std::sscanf(__DATE__,"%3s %d %d",month,&day,&year);std::sscanf(__TIME__,"%d:%d:%d",&hour,&minute,&second);
  static const char names[]="JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *months=std::strstr(names,month);
  tm built={};built.tm_year=year-1900;built.tm_mon=months?int(months-names)/3:0;
  built.tm_mday=day;built.tm_hour=hour;built.tm_min=minute;built.tm_sec=second;
  time_t seed=std::mktime(&built);Preferences cache;
  if(cache.begin("campus-time-v1",true)){const int64_t saved=cache.getLong64("ntp",0);if(saved>seed)seed=time_t(saved);cache.end();}
  if(time(nullptr)<seed){timeval initial={seed,0};settimeofday(&initial,nullptr);}
}
bool pollCampusNetwork(bool linked,uint32_t now) {
  if(usbLinkActive()){authenticated.store(true);return false;}  // the USB gateway is the network: no portal
  if(!campusUsesPortal())return false;
  // A failed open/write must not retry every 20ms. A successful save stays once per boot.
  if(synced.load()&&!savedTime&&(!timeSaveAttempted||uint32_t(now-timeSaveAttemptMs)>=60000)){
    timeSaveAttempted=true;timeSaveAttemptMs=now;
    Preferences cache;if(cache.begin("campus-time-v1",false)){savedTime=cache.putLong64("ntp",int64_t(time(nullptr)))==sizeof(int64_t);cache.end();}
    if(!savedTime)Serial.println("[campus] clock cache save failed; retry in 60s");
  }
  if(!linked){authenticated.store(false);return session.poll(false,now,campusUsername(),campusPassword(),portalQuery,campusUsesNjit());}
  NetworkGuard network(0);if(!network)return false;
  const uint32_t reports=session.loginReports;
  const bool changed=session.poll(linked,now,campusUsername(),campusPassword(),portalQuery,campusUsesNjit());
  if(session.loginReports!=reports)Serial.printf("[campus] login result=%d msg=%s\n",session.loginResult,session.loginMessage);
  authenticated.store(session.online());
  if(changed)Serial.printf("[campus] state=%s\n",campusStatus(session.state));
  return changed;
}
bool campusAuthenticated(){return usbLinkActive()||!campusUsesPortal()||authenticated.load();}
bool campusClockReady(){return usbLinkClockReady()||!campusUsesPortal()||synced.load();}
const char *campusNetworkStatus(){return !usbLinkActive()&&campusUsesPortal()&&!session.online()?campusStatus(session.state):nullptr;}
const char *campusNetworkHint(){return !usbLinkActive()&&campusUsesPortal()&&!session.online()?campusHint(session.state):nullptr;}
