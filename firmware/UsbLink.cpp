#include "UsbLink.h"
#include <Arduino.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "ClockConfig.h"
#include "UsbFrame.h"

namespace {
using namespace usbframe;
constexpr int kMaxConn=2;
constexpr uint32_t kHostTimeoutMs=15000, kOpenTimeoutMs=15000, kPendingExpiryMs=20000;

struct Target {char host[64];uint16_t port;uint32_t queuedMs;};
struct Conn {
  bool used=false, opened=false, hostClosed=false;
  uint8_t id=0;                             // unique per connection (not the slot), so a late frame never hits a new one
  int sock=-1;
  uint32_t startedMs=0, lastOpenMs=0;
  uint16_t port=0;
  char host[64]={};
  uint32_t rxOffset=0, consumedOffset=0, ackedConsumed=0;  // host->device stream: accepted / delivered to the socket
  uint16_t head=0, count=0;                 // host->device bytes waiting for the socket
  uint8_t *ring=nullptr;                    // kConnCredit bytes, allocated once at start-up
};
Conn conns[kMaxConn];
Target pending[2];
int pendingCount=0;
SemaphoreHandle_t stateLock=nullptr, txLock=nullptr;
std::atomic<uint32_t> lastHostMs{0};
std::atomic<bool> hostSeen{false}, clockSet{false};
std::atomic<uint32_t> uartErrors{0};
uint32_t currentBaud=kSlowBaud;
int listener=-1;
uint8_t nextConnId=1;
Parser *parserPtr=nullptr;
#define parser (*parserPtr)

void sendFrame(uint8_t type,uint8_t conn,const uint8_t *payload,size_t size) {
  uint8_t buffer[kMaxPayload+kOverhead];
  const size_t total=encode(type,conn,payload,size,buffer,sizeof buffer);
  if(!total)return;
  xSemaphoreTake(txLock,portMAX_DELAY);
  Serial.write(buffer,total);
  xSemaphoreGive(txLock);
}

void setClock(uint32_t epoch) {
  if(epoch<1704067200UL)return;  // not a plausible date: ignore
  timeval now={};gettimeofday(&now,nullptr);
  const long diff=long(now.tv_sec)-long(epoch);
  if(diff>2||diff<-2){timeval set={time_t(epoch),0};settimeofday(&set,nullptr);}
  clockSet.store(true);
}

// The gateway resends anything not acknowledged: the UART drops bytes while flash is being written.
// Callers hold stateLock. Payload: accepted offset, delivered offset, close seen.
void sendAck(int index) {
  const Conn &c=conns[index];
  uint8_t ack[9];putU32(ack,c.rxOffset);putU32(ack+4,c.consumedOffset);ack[8]=c.hostClosed?1:0;
  sendFrame(CREDIT,c.id,ack,sizeof ack);
}

// Callers hold stateLock. -1 when the gateway names a connection that is gone.
int findConn(uint8_t id) {
  for(int i=0;i<kMaxConn;++i)if(conns[i].used&&conns[i].id==id)return i;
  return -1;
}

// Callers hold stateLock.
void closeConn(int index,bool notifyHost) {
  Conn &c=conns[index];
  if(!c.used)return;
  if(c.sock>=0)close(c.sock);
  c.sock=-1;c.used=false;c.opened=false;c.hostClosed=false;c.head=0;c.count=0;c.rxOffset=0;c.consumedOffset=0;c.ackedConsumed=0;
  if(notifyHost)sendFrame(CLOSE_OUT,c.id,nullptr,0);
}

void setBaud(uint32_t value) {
  if(value==currentBaud)return;
  Serial.flush();
  Serial.updateBaudRate(value);
  currentBaud=value;
}

void handleFrame() {
  const uint32_t now=millis();
  lastHostMs.store(now);
  switch(parser.type) {
    case HELLO: {
      if(parser.size<8)return;
      hostSeen.store(true);
      setClock(getU32(parser.payload));
      const uint32_t wanted=getU32(parser.payload+4);
      const uint32_t chosen=wanted==kFastBaud?kFastBaud:kSlowBaud;
      uint8_t ack[4+24];putU32(ack,chosen);
      const int n=snprintf(reinterpret_cast<char *>(ack+4),24,"mcwave %s",kFirmwareVersion);
      sendFrame(HELLO_ACK,0,ack,4+(n>0?n:0));
      setBaud(chosen);
      Serial.printf("[usb] gateway attached baud=%lu\n",static_cast<unsigned long>(chosen));
      break;
    }
    case PING: {
      if(parser.size<4)return;
      hostSeen.store(true);
      setClock(getU32(parser.payload));
      uint8_t pong[4];putU32(pong,now);
      sendFrame(PONG,0,pong,4);
      break;
    }
    case OPEN_RESULT: {
      xSemaphoreTake(stateLock,portMAX_DELAY);
      const int index=findConn(parser.conn);
      if(index>=0&&!conns[index].opened){
        if(parser.size>=1&&parser.payload[0]==1)conns[index].opened=true;
        else {Serial.printf("[usb] open refused by gateway reason=%u\n",parser.size>1?parser.payload[1]:0);closeConn(index,false);}
      }
      xSemaphoreGive(stateLock);
      break;
    }
    case DATA_IN: {
      if(parser.size<4)return;
      xSemaphoreTake(stateLock,portMAX_DELAY);
      const int index=findConn(parser.conn);
      if(index>=0&&conns[index].opened) {
        Conn &c=conns[index];
        const uint16_t n=parser.size-4;
        // In order and within the window: accept. Otherwise stay silent about the data and say what we have.
        if(getU32(parser.payload)==c.rxOffset&&c.count+n<=kConnCredit) {
          for(uint16_t i=0;i<n;++i)c.ring[(c.head+c.count+i)%kConnCredit]=parser.payload[4+i];
          c.count+=n;c.rxOffset+=n;
        }
        sendAck(index);
      }
      xSemaphoreGive(stateLock);
      break;
    }
    case CLOSE_IN: {
      xSemaphoreTake(stateLock,portMAX_DELAY);
      // Payload: total bytes the gateway sent. Close only after all of them arrived; buffered bytes still drain.
      const int index=findConn(parser.conn);
      if(index>=0) {
        Conn &c=conns[index];
        if(parser.size>=4&&getU32(parser.payload)==c.rxOffset)c.hostClosed=true;
        sendAck(index);
      }
      xSemaphoreGive(stateLock);
      break;
    }
    default: break;
  }
}

void rxTask(void *) {
  for(;;) {
    int available=Serial.available();
    if(available<=0){vTaskDelay(1);continue;}
    while(available-->0) {
      const int b=Serial.read();
      if(b<0)break;
      if(parser.feed(uint8_t(b)))handleFrame();
    }
  }
}

void sendOpen(int slot,const char *host,uint16_t port) {
  uint8_t open[2+64];open[0]=uint8_t(port);open[1]=uint8_t(port>>8);
  const size_t n=strnlen(host,63);memcpy(open+2,host,n);
  sendFrame(OPEN,uint8_t(slot),open,2+n);
}

void attachAccepted(int sock) {
  xSemaphoreTake(stateLock,portMAX_DELAY);
  const uint32_t now=millis();
  while(pendingCount&&now-pending[0].queuedMs>kPendingExpiryMs){pending[0]=pending[1];--pendingCount;}
  int slot=-1;
  for(int i=0;i<kMaxConn;++i)if(!conns[i].used){slot=i;break;}
  if(slot<0||!pendingCount||!hostSeen.load()) {
    xSemaphoreGive(stateLock);
    close(sock);return;
  }
  const Target target=pending[0];pending[0]=pending[1];--pendingCount;
  const int flags=fcntl(sock,F_GETFL,0);fcntl(sock,F_SETFL,flags|O_NONBLOCK);
  Conn &c=conns[slot];
  c.used=true;c.opened=false;c.hostClosed=false;c.id=nextConnId++;if(!nextConnId)nextConnId=1;c.sock=sock;c.startedMs=now?now:1;c.lastOpenMs=now;c.head=0;c.count=0;
  c.rxOffset=0;c.consumedOffset=0;c.ackedConsumed=0;c.port=target.port;strcpy(c.host,target.host);
  const uint8_t id=c.id;
  xSemaphoreGive(stateLock);
  sendOpen(id,target.host,target.port);
}

void serviceConn(int index) {
  uint8_t chunk[kMaxPayload-24];
  xSemaphoreTake(stateLock,portMAX_DELAY);
  Conn &c=conns[index];
  if(!c.used){xSemaphoreGive(stateLock);return;}
  const uint32_t now=millis();
  if(!c.opened){
    if(c.hostClosed){closeConn(index,false);xSemaphoreGive(stateLock);return;}
    if(now-c.startedMs>kOpenTimeoutMs){Serial.println("[usb] gateway did not answer OPEN");closeConn(index,true);}
    else if(now-c.lastOpenMs>1500){c.lastOpenMs=now;char host[64];strcpy(host,c.host);const uint16_t port=c.port;const uint8_t id=c.id;xSemaphoreGive(stateLock);sendOpen(id,host,port);return;}
    xSemaphoreGive(stateLock);return;
  }
  const int sock=c.sock;
  const uint8_t id=c.id;
  xSemaphoreGive(stateLock);

  const int received=recv(sock,chunk,sizeof chunk,MSG_DONTWAIT);
  if(received>0)sendFrame(DATA_OUT,id,chunk,size_t(received));
  else if(received==0||(errno!=EWOULDBLOCK&&errno!=EAGAIN)) {
    xSemaphoreTake(stateLock,portMAX_DELAY);closeConn(index,true);xSemaphoreGive(stateLock);return;
  }

  xSemaphoreTake(stateLock,portMAX_DELAY);
  if(!c.used){xSemaphoreGive(stateLock);return;}
  size_t take=c.count<sizeof chunk?c.count:sizeof chunk;
  for(size_t i=0;i<take;++i)chunk[i]=c.ring[(c.head+i)%kConnCredit];
  xSemaphoreGive(stateLock);
  if(take) {
    const int sent=send(sock,chunk,take,MSG_DONTWAIT);
    if(sent>0) {
      xSemaphoreTake(stateLock,portMAX_DELAY);
      if(c.used){
        c.head=uint16_t((c.head+sent)%kConnCredit);c.count-=uint16_t(sent);c.consumedOffset+=uint32_t(sent);
        if(c.consumedOffset-c.ackedConsumed>=1024||c.count==0){c.ackedConsumed=c.consumedOffset;sendAck(index);}
      }
      xSemaphoreGive(stateLock);
    } else if(sent<0&&errno!=EWOULDBLOCK&&errno!=EAGAIN) {
      xSemaphoreTake(stateLock,portMAX_DELAY);closeConn(index,true);xSemaphoreGive(stateLock);
    }
  }
  // The gateway closed its side: finish once everything it sent has reached the socket.
  xSemaphoreTake(stateLock,portMAX_DELAY);
  if(c.used&&c.hostClosed&&c.count==0)closeConn(index,false);
  xSemaphoreGive(stateLock);
}

void relayTask(void *) {
  uint32_t lastStats=0,reportedErrors=0,reportedBad=0;
  for(;;) {
    if(listener>=0) {
      sockaddr_in peer={};socklen_t length=sizeof peer;
      const int accepted=accept(listener,reinterpret_cast<sockaddr *>(&peer),&length);
      if(accepted>=0)attachAccepted(accepted);
    }
    for(int i=0;i<kMaxConn;++i)serviceConn(i);
    if(hostSeen.load()&&millis()-lastHostMs.load()>kHostTimeoutMs) {
      hostSeen.store(false);
      xSemaphoreTake(stateLock,portMAX_DELAY);
      for(int i=0;i<kMaxConn;++i)closeConn(i,false);
      pendingCount=0;
      xSemaphoreGive(stateLock);
      setBaud(kSlowBaud);
      Serial.println("[usb] gateway lost; back to Wi-Fi");
    }
    const uint32_t tick=millis();
    if(hostSeen.load()&&tick-lastStats>30000) {
      lastStats=tick;
      const uint32_t errors=uartErrors.load(),bad=parserPtr->badFrames;
      if(errors!=reportedErrors||bad!=reportedBad){reportedErrors=errors;reportedBad=bad;Serial.printf("[usb] stats uartErrors=%lu badFrames=%lu\n",static_cast<unsigned long>(errors),static_cast<unsigned long>(bad));}
    }
    vTaskDelay(1);
  }
}
}  // namespace

bool usbLinkActive(){return hostSeen.load()&&millis()-lastHostMs.load()<kHostTimeoutMs;}
bool usbLinkClockReady(){return clockSet.load();}

bool usbLinkPrepareConnect(const char *host,uint16_t port) {
  if(!stateLock||!usbLinkActive()||!host||strlen(host)>=sizeof pending[0].host)return false;
  xSemaphoreTake(stateLock,portMAX_DELAY);
  bool queued=false;
  if(pendingCount<2) {
    Target &t=pending[pendingCount++];
    strcpy(t.host,host);t.port=port;t.queuedMs=millis();queued=true;
  }
  xSemaphoreGive(stateLock);
  return queued;
}

void startUsbLink() {
  if(stateLock)return;
  // Allocate while the heap is still unfragmented; TLS needs big contiguous blocks later.
  parserPtr=new Parser();
  for(Conn &c:conns)c.ring=static_cast<uint8_t *>(malloc(kConnCredit));
  for(Conn &c:conns)if(!c.ring){Serial.println("[usb] out of memory; USB link disabled");return;}
  stateLock=xSemaphoreCreateMutex();txLock=xSemaphoreCreateMutex();
  setenv("TZ","CST-8",1);tzset();
  // A larger receive buffer rides out the gateway's flow-control window (4 KB per connection).
  Serial.end();Serial.setRxBufferSize(8192);Serial.begin(kSlowBaud);
  Serial.onReceiveError([](hardwareSerial_error_t){uartErrors.fetch_add(1);});
  listener=socket(AF_INET,SOCK_STREAM,0);
  if(listener>=0) {
    sockaddr_in address={};address.sin_family=AF_INET;address.sin_port=htons(kUsbRelayPort);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    const int reuse=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    if(bind(listener,reinterpret_cast<sockaddr *>(&address),sizeof address)!=0||listen(listener,2)!=0){
      Serial.printf("[usb] loopback relay unavailable errno=%d\n",errno);close(listener);listener=-1;
    } else fcntl(listener,F_SETFL,fcntl(listener,F_GETFL,0)|O_NONBLOCK);
  }
  xTaskCreate(rxTask,"usbrx",5120,nullptr,3,nullptr);
  xTaskCreate(relayTask,"usbrelay",6144,nullptr,2,nullptr);
  Serial.printf("[usb] link ready relay=%s\n",listener>=0?"loopback":"none");
}
