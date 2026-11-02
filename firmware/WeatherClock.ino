// mcwave 6.0.0：天气、双人心情、云相册（赛事页只留在 edison）。
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/task.h>
#include <time.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "RecoveryPolicy.h"
#include "UsbLink.h"
#include "PowerRecovery.h"
#include "PowerRecoveryPolicy.h"
#ifdef ESP_PLATFORM
#include <Preferences.h>
#endif
#include "Secrets.h"
#include "CampusConfig.h"
#include "CampusNetwork.h"
#include "WifiProfiles.h"
#include "ClockConfig.h"
#include "WeatherData.h"
#include "WeatherCache.h"
#include "RainCache.h"
#include "Display.h"
#include "QuoteDeck.h"
#include "DomesticNetwork.h"
#include "PageButton.h"
#include "RemoteControl.h"
#include "AlbumCache.h"
#include "MoodCache.h"
#include "MoodTiming.h"
#include "WifiAwake.h"
#include "NetworkAccess.h"
#include "src/waveshare/DEV_Config.h"
#include "src/waveshare/EPD_4in2_V2.h"

// The current ESP32 core allocates the FreeRTOS timer stack from the heap
// before Arduino setup. Keep this small startup allocation deterministic.
#ifdef ESP_PLATFORM
extern "C" void __wrap_vApplicationGetTimerTaskMemory(
    StaticTask_t **tcbBuffer, StackType_t **stackBuffer, uint32_t *stackSize) {
  static StaticTask_t timerTcb;
  static StackType_t timerStack[configTIMER_TASK_STACK_DEPTH];
  *tcbBuffer = &timerTcb;
  *stackBuffer = timerStack;
  *stackSize = configTIMER_TASK_STACK_DEPTH;
}
#endif

static Display display;
static WeatherData weather;
static RainForecast rain;
static bool rainFailed=true;
static QuoteDeck quotes;
static bool lastConnected = false, queryFailed = false, weatherAttempted = false;
static bool lastLinked=false;
static bool hasDisplay = false, dirty = true;
static bool weatherFromCache=false;
static uint32_t wifiAttemptMs = 0, weatherAttemptMs = 0, weatherSuccessMs = 0;
static uint32_t fullRefreshMs = 0, displayMs = 0;
static int64_t shownMinute = -1;
static uint8_t currentPose = 0;
static PageButton button;
static bool moodMode=false, buttonRefresh=false;
static uint32_t shownMoodGeneration=0;
static uint32_t remoteRevision=0,remoteApplied=0;
static bool albumMode=false,albumDirty=false;
static RemoteCommand albumView;
static char shownAlbumId[65]={};

static const char *resetReasonName(esp_reset_reason_t reason) {
  return mcwavePower::reasonName(uint32_t(reason));
}

static void startLoopWatchdog() {
  esp_task_wdt_config_t config={};
  config.timeout_ms=mcwaveRecovery::kLoopTimeoutMs;
  config.trigger_panic=true;
  // Preserve the SDK's existing idle-task subscriptions when reconfiguring.
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  config.idle_core_mask|=1U;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  config.idle_core_mask|=2U;
#endif
  ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&config));
  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr)); // setup() runs in loopTask.
  Serial.printf("[watchdog] loopTask timeout=%lu ms\n",config.timeout_ms);
}

static bool clockReady(time_t now) { return now >= 1704067200 && campusClockReady(); }

static bool fetchWeather() { return fetchDomesticWeather(weather); }

static uint8_t randomPose() {
  const uint32_t bound = kNailongFrames;
  const uint32_t threshold = uint32_t(-bound) % bound;
  uint32_t sample;
  do { sample = esp_random(); } while (sample < threshold);
  return uint8_t(sample % bound);
}

// Each half comes from that person's browser; a half never chosen (or not yet
// downloaded) shows only the name.
static void renderMood() {
  static const char *const names[2]={"jks","momosaysay"};
  display.clear();
  for(uint8_t side=0;side<2;++side) {
    if(loadMoodHalf(side,display.pixels))continue;
    for(int y=0;y<kHeight;++y)std::memset(display.pixels+y*(kWidth/8)+side*25,0xff,25);
    display.text(side*200+100-Display::textWidth(names[side],14)/2,140,names[side],14);
  }
  display.rect(199,16,2,268);
}

static uint32_t assetRetryMs=0;
static void refresh(time_t now, bool connected) {
  // 5.6.2: not while another task holds a TLS session (sync, OTA).
  // Drawing inflates packed glyphs/frames with an ~11KB temporary state that
  // may then not fit, which left Nailong or text out of the frame.
  NetworkGuard idle(0);
  if(!idle || (assetRetryMs && !elapsed(millis(),assetRetryMs,5000)))return;
  remoteScreenPreparing(); // This shared buffer is no longer the last physical image.
  if(albumMode) {
    if(!loadAlbumPhoto(albumView.photoId,display.pixels))return;
    // A settings-only command can acknowledge the already displayed same image.
    if(hasDisplay&&!std::strcmp(shownAlbumId,albumView.photoId)) {
      remoteAlbumRendered(albumView);remoteApplied=albumView.revision;remoteRevision=0;albumDirty=false;return;
    }
  }
  tm local = {};
  const bool ready = clockReady(now);
  const int64_t minute = ready ? now / 60 : -1;
  // Independent random draw per minute; other refreshes keep that minute's pose.
  if (!hasDisplay || minute != shownMinute) currentPose = randomPose();
  if (ready) localtime_r(&now, &local);
  const bool stale = weather.valid && elapsed(millis(), weatherSuccessMs, 30UL * 60 * 1000);
  const uint32_t failures=packedAssetFailures();
  const uint32_t moodShown=moodGeneration();
  if(!albumMode && moodMode) renderMood();
  else if(!albumMode) display.render(ready ? &local : nullptr, weather, connected, queryFailed || weatherFromCache, stale,
                        quotes.next(esp_random), currentPose,campusNetworkStatus(),campusNetworkHint(),&rain,rainFailed);
  // A frame with a block that could not be inflated keeps the panel's image.
  if(packedAssetFailures()!=failures){assetRetryMs=millis();Serial.println("[screen] frame had missing assets; retrying shortly");return;}
  assetRetryMs=0;
  const uint32_t started = millis();
  if (albumMode || kHinkFullRefreshOnly || !hasDisplay || buttonRefresh || elapsed(started, fullRefreshMs, kFullRefreshMs)) {
    EPD_4IN2_V2_Init();
    EPD_4IN2_V2_Display(display.pixels);
    if(kHinkFullRefreshOnly)EPD_4IN2_V2_Sleep();
    fullRefreshMs = started;
    Serial.println(kHinkFullRefreshOnly?"[screen] HINK full refresh; asleep":"[screen] full refresh");
  } else {
    // The vendor API expects a packed region. A full-width region matches our
    // framebuffer stride, so no cropping/copy buffer is needed. Keep the
    // controller awake between updates to preserve its previous-frame RAM.
    EPD_4IN2_V2_PartialDisplay(display.pixels, 0, 0, kWidth, kHeight);
    Serial.println("[screen] partial refresh");
  }
  if(EPD_4IN2_V2_RefreshSucceeded())
    remoteScreenRendered(display.pixels,albumMode?"album":moodMode?"mood":"weather");
  hasDisplay = true;
  displayMs = started;
  shownMinute = minute;
  dirty = false;
  buttonRefresh=false;
  shownMoodGeneration=moodShown;
  if(albumMode){std::strcpy(shownAlbumId,albumView.photoId);remoteAlbumRendered(albumView);albumDirty=false;}
  else {shownAlbumId[0]=0;remoteRendered(moodMode,remoteRevision);}
  if(remoteRevision)remoteApplied=remoteRevision;
  remoteRevision=0;
}

void setup() {
  DEV_Module_Init();
  startPowerRecovery();
  const esp_reset_reason_t reason=esp_reset_reason();
  Serial.printf("[boot] reset reason=%d %s\n",int(reason),resetReasonName(reason));
  startLoopWatchdog();
  startCampusNetwork();
  pinMode(0,INPUT_PULLUP);
  weatherFromCache=loadWeatherCache(weather);
  Serial.printf("[weather] boot from_cache=%d valid=%d code=%d\n",weatherFromCache,weather.valid,weather.code);
  const bool rainCached=loadRainCache(rain);
  Serial.printf("[rain] startup cache=%s\n",rainCached?"loaded":"empty");
  Serial.printf("[weather] startup cache=%s\n",weatherFromCache?"loaded":"empty");
  remoteOtaBoot();
  startRemoteControl(moodMode);
#ifdef ESP_PLATFORM
  // 6.0.0 dropped the sports page; free the up-to-14KB feed it kept in NVS.
  {Preferences old;if(old.begin("sports-cn-v31",false)){old.clear();old.end();}}
  albumMode=remoteStartAlbum(albumView);
  startAlbumCache(albumMode?albumView.photoId:"");
  if(albumMode&&!loadAlbumPhoto(albumView.photoId,display.pixels))albumMode=false;
#endif
#ifndef MCWAVE_RWDT_DIAGNOSTIC
  startRemoteTask();
#endif
  Serial.printf("WeatherClock %s: Jiangning / %u poses / %u sayings\n", kFirmwareVersion, unsigned(kNailongFrames), unsigned(kQuoteCount));
  Serial.printf("[screen] panel=%s auto interval=%lu ms\n",kHinkFullRefreshOnly?"HINK full-only":"Waveshare V2",kScreenAutoRefreshMinMs);
#ifdef ESP32
  WiFi.onEvent([](arduino_event_id_t,arduino_event_info_t info) {
    Serial.printf("[wifi] link disconnected reason=%u%s ms=%lu\n",info.wifi_sta_disconnected.reason,
                  info.wifi_sta_disconnected.reason==201?" (target AP not found)":"",millis());
  },ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
  WiFi.mode(WIFI_STA);
#if defined(MCWAVE_MULTI_NETWORK)
  WiFi.setAutoReconnect(true); // NJIT baseline; scheduler disables before fallback.
#else
  WiFi.setAutoReconnect(true);
#endif
#ifdef ESP32
  // 6.0.2: join the strongest campus AP, not the first found (6.0.1 once sat at -89 dBm
  // while a -65 dBm AP was in range).
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif
#if defined(MCWAVE_MULTI_NETWORK)
  pollWifiProfiles();
#else
#ifdef ESP32
  if(kCampusEnabled) {
    Serial.printf("[wifi] scanning 2.4GHz for exact SSID=%s\n",kCampusSsid);
    const int count=WiFi.scanNetworks(false,true);int matching=0;
    for(int i=0;i<count;++i) {
      const String ssid=WiFi.SSID(i);
      if(ssid.equalsIgnoreCase(kCampusSsid)) {
        Serial.printf("[wifi] campus AP SSID=%s channel=%ld RSSI=%ld auth=%d%s\n",ssid.c_str(),WiFi.channel(i),WiFi.RSSI(i),
                      int(WiFi.encryptionType(i)),ssid==kCampusSsid?" (exact match)":" (case mismatch)");
        if(ssid==kCampusSsid)++matching;
      }
    }
    Serial.printf("[wifi] campus scan=%d exact APs=%d%s\n",count,matching,count>=0&&!matching?"; no matching 2.4GHz AP seen":"");
    WiFi.scanDelete();
  }
#endif
  WiFi.begin(kCampusEnabled?kCampusSsid:kWifiSsid, kCampusEnabled?"":kWifiPassword);
  wifiAttemptMs = millis();
#endif
#if defined(MCWAVE_MULTI_NETWORK)
  startUsbLink();
#endif
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "ntp.ntsc.ac.cn");
  // Show useful feedback immediately; network and SNTP run asynchronously.
  refresh(time(nullptr), false);
}

void loop() {
  ESP_ERROR_CHECK(esp_task_wdt_reset());
  feedPowerRecovery();
#ifdef MCWAVE_RWDT_DIAGNOSTIC
  if(!powerResetWasRtcWatchdog())freezeForRtcWatchdogTest();
  static bool noted=false;
  if(!noted){Serial.println("[diag] recovered from RWDT; no second deliberate freeze");noted=true;}
  delay(20);return;
#endif
  remoteScreenAllow(false);
  uint32_t nowMs = millis();
  remoteOtaWatch();
  const ButtonAction action=button.update(digitalRead(0)==LOW,nowMs);
  if(action==ButtonAction::Switch) {
    if(albumMode){albumMode=false;moodMode=false;}else moodMode=!moodMode;
    dirty=true;buttonRefresh=true;
  }
#if defined(MCWAVE_MULTI_NETWORK)
  if(pollWifiProfiles())dirty=true;
#endif
  const bool usb = usbLinkActive();
  static bool lastUsb=false;
  if(usb!=lastUsb){
    lastUsb=usb;dirty=true;weatherAttempted=false;
    if(usb){WiFi.setAutoReconnect(false);WiFi.disconnect();Serial.println("[usb] using the Windows gateway; Wi-Fi paused");}
  }
  const bool linked = usb || WiFi.status() == WL_CONNECTED;
  if(linked!=lastLinked) {
    lastLinked=linked;dirty=true;
#ifdef ESP32
    if(linked&&!usb)Serial.printf("[wifi] associated IP=%s RSSI=%d\n",WiFi.localIP().toString().c_str(),WiFi.RSSI());
#endif
  }
  if(pollCampusNetwork(linked,nowMs))dirty=true;
  bool connected = linked && campusAuthenticated();
  if (connected != lastConnected) {
    lastConnected = connected;
    dirty = true;
    weatherAttempted = false; // Query promptly after internet/authentication recovery.
    Serial.println(connected ? "[network] authenticated/ready" : "[network] offline/unauthenticated");
    if(connected) {
      configTime(8*3600,0,"ntp.aliyun.com","ntp.tencent.com","ntp.ntsc.ac.cn");
    }
  }
#if !defined(MCWAVE_MULTI_NETWORK)
  if (!linked && elapsed(nowMs, wifiAttemptMs, kWifiRetryMs)) {
    WiFi.disconnect();
    WiFi.begin(kCampusEnabled?kCampusSsid:kWifiSsid, kCampusEnabled?"":kWifiPassword);
    wifiAttemptMs = nowMs;
    Serial.println("[wifi] reconnecting");
  }
#endif
  time_t now = time(nullptr);
  RemoteCommand command;
  if(takeRemoteCommand(command)&&(command.revision>remoteApplied||command.automatic)&&
     !(command.automatic&&albumMode&&!std::strcmp(command.photoId,shownAlbumId))) {
    albumMode=command.album;
    if(albumMode){albumView=command;albumDirty=true;moodMode=false;}
    else moodMode=command.mood;
    remoteRevision=command.automatic?0:command.revision;dirty=true;
  }
  // A changed half waits for the same 60 s minimum as other automatic frames.
  const bool moodDirty=!albumMode&&moodMode&&moodGeneration()!=shownMoodGeneration;
  // Render a button action before any synchronous weather query starts.
  if(buttonRefresh || (remoteRevision && elapsed(millis(),displayMs,10000)) ||
     (albumDirty && elapsed(millis(),displayMs,kScreenAutoRefreshMinMs)) ||
     moodTiming::refreshDue(moodDirty,hasDisplay,millis(),displayMs,kScreenAutoRefreshMinMs))refresh(now,connected);
  // A half may finish while the network task is still downloading its partner.
  // Keep retrying the UI/deadline, rather than blocking it in a weather fetch.
  if(moodDirty&&moodGeneration()!=shownMoodGeneration){delay(20);return;}
  if (connected && clockReady(now) &&
      (!weatherAttempted || elapsed(nowMs, weatherAttemptMs,
                       queryFailed ? kWeatherRetryMs : kWeatherIntervalMs))) {
    weatherAttempted = true;
    remoteDropConnection();
    {
      WifiAwake awake;
      queryFailed = !fetchWeather();
      ESP_ERROR_CHECK(esp_task_wdt_reset());
      feedPowerRecovery();
      rainFailed = !fetchDomesticRain(rain);
    }
    if(!rainFailed && !saveRainCache(rain))Serial.println("[rain] persistent cache write failed; retaining RAM forecast");
    if(queryFailed)Serial.printf("[weather] query failed; wifi=%s; retaining last data\n",WiFi.status()==WL_CONNECTED?"connected":"disconnected");
    weatherAttemptMs = millis();
    if (!queryFailed) {
      weatherSuccessMs = weatherAttemptMs;weatherFromCache=false;
      if(!saveWeatherCache(weather))Serial.println("[weather] persistent cache write failed; current weather retained in RAM");
    }
    Serial.printf("[weather] result query_failed=%d from_cache=%d valid=%d code=%d\n",queryFailed,weatherFromCache,weather.valid,weather.code);
    dirty = true;
    // TLS and HTTP are bounded but may cross a minute or lose Wi-Fi.
    now = time(nullptr);
    connected = (usbLinkActive() || WiFi.status() == WL_CONNECTED) && campusAuthenticated();
  }
  const int64_t minute = clockReady(now) ? now / 60 : -1;
  // Coalesce automatic updates; HINK must not fully flash on every status event.
  // The mood page has no clock or weather, so it redraws only for moodDirty above.
  if (!albumMode && !moodMode && (dirty || minute != shownMinute) && elapsed(millis(), displayMs, kScreenAutoRefreshMinMs))
    refresh(now, connected);
  remoteScreenAllow(!buttonRefresh&&!remoteRevision&&!albumDirty&&!moodDirty&&
    !(!albumMode&&!moodMode&&(dirty||minute!=shownMinute)&&elapsed(millis(),displayMs,kScreenAutoRefreshMinMs)));
  delay(20);
}
