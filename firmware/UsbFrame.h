#pragma once
#include <stddef.h>
#include <stdint.h>

// mcwave 7.0.0 USB link: frames between the firmware and the Windows gateway.
// [A5 5A][type][conn][len lo][len hi][payload][crc16 lo][crc16 hi]; CRC-16/CCITT-FALSE over type..payload.
// Text log lines share the UART; anything outside a valid frame is a log byte.
namespace usbframe {
constexpr uint8_t kMagic0=0xA5, kMagic1=0x5A;
constexpr size_t kMaxPayload=1024, kOverhead=8;
constexpr uint32_t kFastBaud=921600, kSlowBaud=115200, kConnCredit=4096;
enum Type:uint8_t {
  HELLO=0x01, PING=0x02, OPEN_RESULT=0x11, DATA_IN=0x12, CLOSE_IN=0x13,        // host -> device
  HELLO_ACK=0x81, PONG=0x82, OPEN=0x91, DATA_OUT=0x92, CLOSE_OUT=0x93, CREDIT=0x94  // device -> host
};

inline uint16_t crc16(const uint8_t *data,size_t size,uint16_t crc=0xFFFF) {
  for(size_t i=0;i<size;++i) {
    crc^=uint16_t(data[i])<<8;
    for(int bit=0;bit<8;++bit)crc=(crc&0x8000)?uint16_t((crc<<1)^0x1021):uint16_t(crc<<1);
  }
  return crc;
}

// Returns the frame size, or 0 if the payload is too large for `capacity`.
inline size_t encode(uint8_t type,uint8_t conn,const uint8_t *payload,size_t size,uint8_t *out,size_t capacity) {
  if(size>kMaxPayload||capacity<size+kOverhead)return 0;
  out[0]=kMagic0;out[1]=kMagic1;out[2]=type;out[3]=conn;out[4]=uint8_t(size);out[5]=uint8_t(size>>8);
  for(size_t i=0;i<size;++i)out[6+i]=payload[i];
  const uint16_t crc=crc16(out+2,4+size);
  out[6+size]=uint8_t(crc);out[7+size]=uint8_t(crc>>8);
  return size+kOverhead;
}

class Parser {
 public:
  uint8_t type=0, conn=0;
  uint16_t size=0;
  uint8_t payload[kMaxPayload];
  uint32_t badFrames=0;
  // Feed one received byte; true when a complete, CRC-valid frame is in type/conn/size/payload.
  bool feed(uint8_t b) {
    switch(state_) {
      case 0: if(b==kMagic0)state_=1; return false;
      case 1: state_=b==kMagic1?2:(b==kMagic0?1:0); return false;
      case 2: type_=b;state_=3;return false;
      case 3: conn_=b;state_=4;return false;
      case 4: len_=b;state_=5;return false;
      case 5: len_|=uint16_t(b)<<8;
        if(len_>kMaxPayload){++badFrames;state_=0;return false;}
        got_=0;state_=len_?6:7;return false;
      case 6: buf_[got_++]=b;if(got_==len_)state_=7;return false;
      case 7: crc_=b;state_=8;return false;
      default: {
        crc_|=uint16_t(b)<<8;state_=0;
        uint8_t head[4]={type_,conn_,uint8_t(len_),uint8_t(len_>>8)};
        uint16_t crc=crc16(head,4);crc=crc16(buf_,len_,crc);
        if(crc!=crc_){++badFrames;return false;}
        type=type_;conn=conn_;size=len_;for(uint16_t i=0;i<len_;++i)payload[i]=buf_[i];
        return true;
      }
    }
  }
 private:
  uint8_t state_=0, type_=0, conn_=0;
  uint16_t len_=0, got_=0, crc_=0;
  uint8_t buf_[kMaxPayload];
};

inline void putU32(uint8_t *out,uint32_t v){out[0]=uint8_t(v);out[1]=uint8_t(v>>8);out[2]=uint8_t(v>>16);out[3]=uint8_t(v>>24);}
inline uint32_t getU32(const uint8_t *in){return uint32_t(in[0])|uint32_t(in[1])<<8|uint32_t(in[2])<<16|uint32_t(in[3])<<24;}
}  // namespace usbframe
