#if defined(MCWAVE_MULTI_NETWORK)
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <esp_wifi.h>
#include "CampusConfig.h"
#include "CampusNetwork.h"
#include "UsbLink.h"
#include "WifiProfiles.h"
#include "WifiPriority.h"
#include "NetworkAccess.h"
#include "SiteAddresses.h"
#include "MoyimiSecurity.h"

static std::atomic<int> active{0};
int activeWifiProfile(){return active.load();}
bool pollWifiProfiles(){
  static WifiPriority priority;
  if(usbLinkActive())return false;  // the USB gateway is the network: leave Wi-Fi alone
  const bool linked=WiFi.status()==WL_CONNECTED;
  if(!priority.selectionDue(linked,linked&&campusAuthenticated(),millis()))return false;
  // A switch cannot overlap a remote sync, album download or OTA transfer.
  NetworkGuard network(0);if(!network)return false;
  Serial.println("[wifi] selecting NJIT > moyimi");
  WiFi.setAutoReconnect(false); // Stop old-profile reconnects before switching.
  WiFi.disconnect();
  resetCampusNetwork();
  siteAddresses()=siteAddress::Cache{};
  const int count=WiFi.scanNetworks(false,true);
  uint8_t visible=0;
  for(int i=0;i<count;++i){
    const String ssid=WiFi.SSID(i);
    if(ssid==njitProfile::kCampusSsid||ssid==kMoyimiSsid){
      visible|=ssid==njitProfile::kCampusSsid?1:2;
      Serial.printf("[wifi] target SSID=%s channel=%ld RSSI=%ld auth=%d\n",ssid.c_str(),WiFi.channel(i),WiFi.RSSI(i),int(WiFi.encryptionType(i)));
    }
  }
  WiFi.scanDelete();
  const uint32_t cooling=campusRetryRemaining(millis());
  const int next=priority.select(visible,millis(),cooling?2:3);
  Serial.printf("[wifi] scan=%d visible=%u njit_cooldown_ms=%lu\n",count,visible,cooling);
  if(next<0){Serial.printf("[wifi] no untried profile scan=%d visible=%u; retry in 30s\n",count,visible);return true;}
  active.store(next);
  const char *ssid=next==0?njitProfile::kCampusSsid:kMoyimiSsid;
  Serial.printf("[wifi] selected priority=%d SSID=%s directed=%d\n",next+1,ssid,!(visible&(1U<<next)));
  // NJIT's begin, auto-reconnect, scan/sort and empty password match6.4.1.
  if(next==0){WiFi.setAutoReconnect(true);WiFi.begin(ssid,"");return true;}
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  // The legacy begin wrapper returns an old status even on successful config.
  // Use the same public STA implementation, but check its boolean result.
  if(!WiFi.STA.begin()||!WiFi.STA.connect(ssid,kMoyimiPassword,0,nullptr,false)){Serial.println("[wifi] moyimi config rejected; retry budget=30s");return true;}
  wifi_config_t config={};esp_err_t error=esp_wifi_get_config(WIFI_IF_STA,&config);
  if(error==ESP_OK){applyMoyimiSecurity(config,WIFI_AUTH_WPA2_PSK,WPA3_SAE_PWE_BOTH);error=esp_wifi_set_config(WIFI_IF_STA,&config);}
  if(error==ESP_OK)error=esp_wifi_get_config(WIFI_IF_STA,&config);
  if(error==ESP_OK&&(config.sta.threshold.authmode!=WIFI_AUTH_WPA2_PSK||config.sta.sae_pwe_h2e!=WPA3_SAE_PWE_BOTH||!config.sta.pmf_cfg.capable||config.sta.pmf_cfg.required))error=ESP_ERR_INVALID_STATE;
  if(error==ESP_OK)error=esp_wifi_connect();
  Serial.printf("[wifi] moyimi connect start err=%d min=%d sae=%d pmf=%u/%u\n",int(error),int(config.sta.threshold.authmode),int(config.sta.sae_pwe_h2e),unsigned(config.sta.pmf_cfg.capable),unsigned(config.sta.pmf_cfg.required));
  return true;
}
#endif
