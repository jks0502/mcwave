#if defined(MCWAVE_NJIT) || defined(MCWAVE_MULTI_NETWORK)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_random.h>
#include "SiteAddresses.h"

siteAddress::Cache &siteAddresses(){static siteAddress::Cache cache;return cache;}
bool refreshSiteAddresses(){
  auto &cache=siteAddresses();if(cache.fresh(millis()))return true;
  uint8_t query[96],reply[512];const size_t length=siteAddress::question(query,sizeof(query),uint16_t(esp_random()),"jks0502.xyz");
  const uint32_t started=millis();
  // Ask the DHCP-provided resolver for the complete answer, rather than lwIP's
  // single-address hostByName result. Never change system DNS or use stale IPs.
  for(unsigned server=0;server<2;++server){
    const IPAddress dns=WiFi.dnsIP(server);
    if(dns==IPAddress(uint32_t(0))||(server&&dns==WiFi.dnsIP(0)))continue;
    WiFiUDP udp;if(!udp.begin(0)||!udp.beginPacket(dns,53))continue;
    udp.write(query,length);if(!udp.endPacket())continue;
    const uint32_t sent=millis();
    while(uint32_t(millis()-sent)<1200){
      const int size=udp.parsePacket();
      if(size>0){
        const bool source=udp.remoteIP()==dns&&udp.remotePort()==53;
        const int n=udp.read(reply,sizeof(reply));siteAddress::Records records;
        if(source&&size<=int(sizeof(reply))&&n==size&&siteAddress::answers(reply,n,query,length,records)){
          cache.update(records,millis());
          Serial.printf("[site-route] DNS count=%u ttl=%lus ms=%lu\n",records.count,records.ttl,millis()-started);return true;
        }
      }
      delay(1);
    }
  }
  Serial.println("[site-route] DNS answer unavailable; using SDK resolver");return false;
}
#endif
