#pragma once
// mcwave 6.1.5: keep Wi-Fi reception awake for one batch of transfers, then restore
// idle power saving (same as edison's WifiRequestAwake). With modem sleep on, 6.1.4
// often stalled mid-response (sync -11 after 15 s, mood body -11) while edison on
// the same network, awake during transfers, had none.
#ifdef ESP_PLATFORM
#include <WiFi.h>
struct WifiAwake {
  wifi_ps_type_t previous=WiFi.getSleep();
  bool changed=previous!=WIFI_PS_NONE&&WiFi.setSleep(WIFI_PS_NONE);
  ~WifiAwake(){if(changed)WiFi.setSleep(previous);}
};
#else
struct WifiAwake {};
#endif
