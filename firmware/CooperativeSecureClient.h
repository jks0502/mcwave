#pragma once
#include <NetworkClientSecure.h>
#include <cstring>
#if defined(MCWAVE_NJIT) || defined(MCWAVE_MULTI_NETWORK)
#include "SiteAddresses.h"
#endif
#if defined(MCWAVE_MULTI_NETWORK)
#include "UsbLink.h"
#endif
#include <freertos/task.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_crt_bundle.h>
#endif

// HTTPClient may continuously read a large calendar without blocking. Yield
// one real RTOS tick per read so CPU 0's idle task can service its watchdog.
class CooperativeSecureClient :public NetworkClientSecure {
 public:
  using NetworkClientSecure::read;
#if defined(MCWAVE_NET_DIAGNOSTIC)
  // Diagnostic only: same SDK connect sequence, timestamped at DNS/TCP/TLS boundaries.
  using NetworkClientSecure::connect;
  int connect(const char *host,uint16_t port,int32_t timeout) override {
    _timeout=timeout;return connect(host,port);
  }
  int connect(const char *host,uint16_t port) override {
    IPAddress ip;uint32_t started=millis();
    const bool resolved=Network.hostByName(host,ip);
    Serial.printf("[netdiag] DNS host=%s ip=%s ok=%d ms=%lu heap=%u\n",host,ip.toString().c_str(),resolved,millis()-started,ESP.getFreeHeap());
    if(!resolved)return 0;
    started=millis();
    int ret=start_ssl_client(sslclient.get(),ip,port,host,_timeout,_CA_cert,_use_ca_bundle,_cert,_private_key,nullptr,nullptr,_use_insecure,_alpn_protos);
    Serial.printf("[netdiag] TCP-config host=%s ip=%s ret=%d ms=%lu\n",host,ip.toString().c_str(),ret,millis()-started);
    if(ret>=0){
      started=millis();ret=ssl_starttls_handshake(sslclient.get());
      Serial.printf("[netdiag] TLS host=%s ip=%s ret=%d ms=%lu heap=%u\n",host,ip.toString().c_str(),ret,millis()-started,ESP.getFreeHeap());
    }
    sslclient->last_error=ret;
    if(ret<0){stop();return 0;}
    _connected=true;return 1;
  }
#elif defined(MCWAVE_NJIT) || defined(MCWAVE_MULTI_NETWORK)
 private:
  uint32_t siteAddress_=0;
 public:
  using NetworkClientSecure::connect;
  int connect(const char *host,uint16_t port,int32_t timeout) override {
    _timeout=timeout;return connect(host,port);
  }
  int connect(const char *host,uint16_t port) override {
    siteAddress_=0;
#if defined(MCWAVE_MULTI_NETWORK)
    // USB gateway attached: TLS still ends here; the bytes cross the loopback relay and the serial link.
    if(usbLinkActive()){
      if(!usbLinkPrepareConnect(host,port))return 0;
      const uint32_t started=millis();
      const int ok=NetworkClientSecure::connect(IPAddress(127,0,0,1),kUsbRelayPort,host,_CA_cert,_cert,_private_key);
      Serial.printf("[usb] connect host=%s port=%u ok=%d ms=%lu\n",host,unsigned(port),ok,millis()-started);
      return ok;
    }
#endif
    if(std::strcmp(host,"jks0502.xyz")||!refreshSiteAddresses())return NetworkClientSecure::connect(host,port);
    auto &addresses=siteAddresses();
    // Snapshot this request's bounded choices before failure feedback rotates
    // the next request's starting point. Do not skip or repeat an address here.
    const unsigned tries=addresses.count()<2?addresses.count():2;
    const uint32_t choices[]={addresses.candidate(0),addresses.candidate(1)};
    // Retry a failed connection before sending any HTTP bytes. Retain the
    // original hostname for SNI and certificate verification on every address.
    for(unsigned attempt=0;attempt<tries;++attempt){
      const uint32_t value=choices[attempt];const IPAddress ip(value>>24,(value>>16)&255,(value>>8)&255,value&255);
      const uint32_t started=millis();
      const int ok=NetworkClientSecure::connect(ip,port,host,_CA_cert,_cert,_private_key);
      Serial.printf("[site-route] connect ip=%s attempt=%u ok=%d ms=%lu\n",ip.toString().c_str(),attempt+1,ok,millis()-started);
      if(ok){siteAddress_=value;addresses.succeeded(value);return 1;}
      addresses.failed(value);
    }
    return 0;
  }
#endif
  void siteRequestFailed(){
#if (defined(MCWAVE_NJIT) || defined(MCWAVE_MULTI_NETWORK)) && !defined(MCWAVE_NET_DIAGNOSTIC)
    if(siteAddress_)siteAddresses().failed(siteAddress_);
#endif
  }
  void setCACertBundle(const uint8_t *bundle,size_t size) {
#if defined(ARDUINO_ARCH_ESP32)
    const esp_err_t result=esp_crt_bundle_set(bundle,size);
    if(result!=ESP_OK) {
      Serial.printf("[tls] custom CA bundle rejected: %d; refusing fallback\n",int(result));
      NetworkClientSecure::setCACertBundle(nullptr,0);return;
    }
#endif
    NetworkClientSecure::setCACertBundle(bundle,size);
  }
  int read(uint8_t *buffer,size_t size) override {
    const int received=NetworkClientSecure::read(buffer,size);
    vTaskDelay(1);
    return received;
  }
};
