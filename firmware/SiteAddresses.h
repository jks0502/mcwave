#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace siteAddress {
struct Records {
  uint32_t ip[4]={};unsigned count=0;uint32_t ttl=300;
};
inline uint16_t word(const uint8_t *p){return uint16_t(p[0])<<8|p[1];}
inline uint32_t number(const uint8_t *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline size_t question(uint8_t *out,size_t capacity,uint16_t id,const char *host){
  if(capacity<17)return 0;
  std::memset(out,0,12);out[0]=id>>8;out[1]=id;out[2]=1;out[5]=1;
  size_t at=12;
  while(*host){
    const char *dot=std::strchr(host,'.');const size_t n=dot?size_t(dot-host):std::strlen(host);
    if(!n||n>63||at+1+n+5>capacity)return 0;
    out[at++]=uint8_t(n);std::memcpy(out+at,host,n);at+=n;
    host+=n;if(*host)++host;
  }
  out[at++]=0;out[at++]=0;out[at++]=1;out[at++]=0;out[at++]=1;return at;
}
inline bool skipName(const uint8_t *data,size_t size,size_t &at){
  while(at<size){
    const uint8_t n=data[at++];if(!n)return true;
    if((n&0xc0)==0xc0){if(at>=size)return false;const size_t pointer=size_t(n&63)<<8|data[at++];return pointer>=12&&pointer<size;}
    if(n>63||at+n>size)return false;at+=n;
  }
  return false;
}
// Accept only a matching, complete, successful A/IN response. No fixed IPs.
inline bool answers(const uint8_t *data,size_t size,const uint8_t *query,size_t querySize,Records &out){
  if(querySize<17||size<querySize||word(data)!=word(query)||(word(data+2)&0xfa0f)!=0x8000||
     word(data+4)!=1||std::memcmp(data+12,query+12,querySize-12))return false;
  Records next;size_t at=querySize;const unsigned count=word(data+6);
  if(count>64)return false;
  for(unsigned i=0;i<count;++i){
    if(!skipName(data,size,at)||at+10>size)return false;
    const unsigned type=word(data+at),cls=word(data+at+2),n=word(data+at+8);const uint32_t ttl=number(data+at+4);at+=10;
    if(at+n>size)return false;
    if(cls==1&&ttl<next.ttl)next.ttl=ttl; // Also respect a shorter CNAME-chain TTL.
    if(type==1&&cls==1&&n==4){
      const uint32_t ip=number(data+at);bool known=false;
      for(unsigned j=0;j<next.count;++j)known=known||next.ip[j]==ip;
      if(ip&&!known&&next.count<4)next.ip[next.count++]=ip;
    }
    at+=n;
  }
  if(!next.count)return false;out=next;return true;
}
class Cache {
  Records records_;uint32_t saved_=0,preferred_=0;
 public:
  bool fresh(uint32_t now)const{return records_.count&&uint32_t(now-saved_)<records_.ttl*1000;}
  void update(const Records &records,uint32_t now){
    records_=records;saved_=now;bool present=false;
    for(unsigned i=0;i<records_.count;++i)present=present||records_.ip[i]==preferred_;
    if(!present)preferred_=0;
  }
  unsigned count()const{return records_.count;}
  uint32_t candidate(unsigned attempt)const{
    if(attempt>=records_.count)return 0;unsigned start=0;
    for(unsigned i=0;i<records_.count;++i)if(records_.ip[i]==preferred_)start=i;
    return records_.ip[(start+attempt)%records_.count];
  }
  void succeeded(uint32_t ip){preferred_=ip;}
  void failed(uint32_t ip){
    for(unsigned i=0;i<records_.count;++i)if(records_.ip[i]==ip){preferred_=records_.ip[(i+1)%records_.count];break;}
  }
};
}
// Callers already hold NetworkGuard. The cache is shared across site clients.
siteAddress::Cache &siteAddresses();
bool refreshSiteAddresses();
