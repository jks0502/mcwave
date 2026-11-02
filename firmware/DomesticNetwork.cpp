#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include "DomesticNetwork.h"
#include "DomesticSports.h"
#include "ChinaWeather.h"
#include "JsonObjects.h"
#include "NetworkAccess.h"
#include "CampusCertificates.h"
#include "ClockConfig.h"
#include "CooperativeSecureClient.h"
#include "TlsTrust.h"
#include "NmcWeather.h"
#include "FixedHttpClient.h"

template<class Reader> class JsonOutput:public Stream {
  Reader &reader_;
  size_t consumed_=0;
 public:
  explicit JsonOutput(Reader &reader):reader_(reader) {}
  size_t write(uint8_t c) override{if(!reader_.put(char(c)))return 0;++consumed_;return 1;}
  size_t write(const uint8_t *bytes,size_t count) override {size_t i=0;for(;i<count;++i)if(!reader_.put(char(bytes[i])))break;consumed_+=i;return i;}
  size_t consumed() const{return consumed_;}
  int available() override{return 0;}int read() override{return -1;}int peek() override{return -1;}void flush() override{}
};
// Caller holds NetworkGuard, including any later parsing of jsonWorkspace().
template<class Reader> static bool queryLocked(const String &url,Reader &reader,const char *label) {
  // Reuse the indexed trust bundle: avoid parsing every PEM root into RAM.
  CooperativeSecureClient client;configureTlsTrust(client,url.c_str());client.setHandshakeTimeout(8);
  FixedHttpClient http;http.setConnectTimeout(8000);http.setTimeout(15000);http.setReuse(false);
  http.setUserAgent("WeatherClock/3.1");
  if(!http.begin(client,url))return false;
  http.addHeader("Referer","https://www.weather.com.cn/");
  const int status=http.GET();int bytes=-1;
  Serial.printf("[%s] response byte_heap=%u largest=%u epoch=%lld\n",label,heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),(long long)time(nullptr));
  if(status==200){JsonOutput<Reader> output(reader);bytes=http.writeToStream(&output);
    if(bytes<0)Serial.printf("[%s] body failed=%d consumed=%u byte_heap=%u largest=%u stack=%u\n",label,bytes,unsigned(output.consumed()),heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),unsigned(uxTaskGetStackHighWaterMark(nullptr)));}
  if(status<0){char detail[160];const int error=client.lastError(detail,sizeof(detail));Serial.printf("[%s] TLS=%d %s\n",label,error,detail);}
  http.end();
  Serial.printf("[%s] HTTP=%d bytes=%d heap=%u largest=%u\n",label,status,bytes,ESP.getFreeHeap(),ESP.getMaxAllocHeap());
  return status==200 && bytes>=0;
}
template<class Reader> static bool query(const String &url,Reader &reader,const char *label) {
  NetworkGuard network;if(!network)return false;
  return queryLocked(url,reader,label);
}
bool fetchDomesticWeather(WeatherData &weather) {
  // Acquire before JsonAssignment clears the workspace; retain through parse.
  NetworkGuard network;if(!network)return false;
  const time_t now=time(nullptr);tm local={};localtime_r(&now,&local);
  JsonAssignment observation("\"real\"");
  if(!queryLocked(kNmcWeatherUrl,observation,"weather")){Serial.println("[weather] parse ok=0 reason=transport");return false;}
  if(!observation.complete()){Serial.println("[weather] parse ok=0 reason=extraction_incomplete");return false;}
  NmcWeatherDiagnostic detail;
  const bool ok=parseNmcWeather(observation.json(),local,weather,&detail);
  Serial.printf("[weather] parse ok=%d complete=1 reason=%s img=%d unknown=%d\n",ok,detail.reason,detail.image,detail.unknown);
  if(!ok)return false;
  Serial.printf("[weather] NMC updated %.1fC RH=%d code=%d\n",weather.temperature,weather.humidity,weather.code);
  return true;
}
bool fetchDomesticRain(RainForecast &rain) {
  const time_t now=time(nullptr);tm local={};localtime_r(&now,&local);
  NmcRainReader forecast(local);RainForecast next;
  if(!query(kNmcRainUrl,forecast,"rain") || !forecast.finish(next))return false;
  RainProbabilityReader probabilities(next);
  const bool predicted=query(kRainProbabilityUrl,probabilities,"rain-probability") && probabilities.finish();
  if(predicted)next.probabilityMinute=next.fetchedMinute;
  else {
    // Discard any incomplete probability response and retain only matching cached hours.
    next.currentProbability=rain.currentProbability;next.currentProbabilityMinute=rain.currentProbabilityMinute;
    next.probabilityMinute=rain.probabilityMinute;
    for(int i=0;i<next.count;++i) {
      next.probability[i]=-1;
      for(int j=0;j<rain.count;++j)if(next.minute[i]==rain.minute[j]){next.probability[i]=rain.probability[j];break;}
    }
  }
  if(!validRainForecast(next))return false;
  rain=next;
  Serial.printf("[rain] NMC slots=%u; China Weather probability=%s\n",rain.count,predicted?"updated":"retained/unknown");
  return true;
}
struct SourceContext {DomesticSports &feed;int source;};
static bool collect(const cJSON *item,void *context) {auto &c=*static_cast<SourceContext *>(context);return c.source==8?c.feed.cs2(item):c.feed.tencent(item,c.source);}
std::string fetchDomesticSports(const char *date) {
  const time_t now=time(nullptr);tm local={};localtime_r(&now,&local);DomesticSports feed(date,local);
  int errors=0;
  // The football category includes cups, so Chelsea/Real Madrid are not limited to league fixtures.
  for(const auto &source:{std::pair<int,const char*>{1,"football"},{2,"100000"},{4,"100360"}}) {
    SourceContext context{feed,source.first};JsonObjects reader(collect,&context);
    const String url=String("https://matchweb.sports.qq.com/matchUnion/list?columnId=")+source.second+"&startTime="+date+"&endTime="+feed.tomorrow()+"&today="+date+"&index=3&isInit=true";
    const bool ok=query(url,reader,source.first==1?"football":source.first==2?"nba":"f1") && reader.finish();
    if(!ok)errors|=source.first;
  }
  SourceContext context{feed,8};JsonObjects reader(collect,&context);
  const bool csOk=query(String("https://esports-data.5eplaycdn.com/v1/api/csgo/platform/matches/preview?start_date=")+date,reader,"cs2") && reader.finish(true);
  if(!csOk)errors|=8;
  if(errors==15 || feed.overflow())return {};
  Serial.printf("[sports] domestic snapshot events=%u errors=%d\n",unsigned(feed.count()),errors);
  // Keep TLS queries out while allocating/writing the handoff JSON.
  NetworkGuard network;if(!network)return {};
  std::string body=feed.json(time(nullptr),errors);
  Serial.printf("[sports] JSON bytes=%u byte_heap=%u largest=%u stack=%u\n",unsigned(body.size()),heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  return body;
}
