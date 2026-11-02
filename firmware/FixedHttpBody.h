#pragma once
#include <stdint.h>
#include <stddef.h>
#include <climits>

// All callers hold NetworkGuard through the read. One shared buffer is reused
// for weather, sports, campus authentication and remote control responses.
inline uint8_t *fixedHttpBuffer() { static uint8_t bytes[1024];return bytes; }

// HTTPClient-compatible errors: connection lost=-5, encoding=-9,
// output/length mismatch=-10, inactivity timeout=-11. No heap allocation.
template<class Client,class Sink,class Clock> class FixedHttpBody {
  Client &client_;Sink &sink_;Clock &clock_;
  uint32_t timeout_,progress_,deadline_,started_;
  int total_=0;
  int receive(uint8_t *out,size_t limit) {
    for(;;) {
      // mcwave 6.0.2: a body that keeps trickling never hits the inactivity
      // timeout; on a phone hotspot one held the network lock for 10+ minutes.
      if(deadline_ && uint32_t(clock_.now()-started_)>=deadline_)return -11;
      const int available=client_.available();
      if(available>0) {
        const size_t count=size_t(available)<limit?size_t(available):limit;
        const int n=client_.read(out,count);
        if(n<0)return -5;
        if(n>0){progress_=clock_.now();return n;}
      }
      if(!client_.connected() && client_.available()<=0)return 0;
      if(uint32_t(clock_.now()-progress_)>=timeout_)return -11;
      clock_.pause();
    }
  }
  int character() {
    uint8_t c;const int n=receive(&c,1);return n==1?int(c):n<0?n:-5;
  }
  int line(char *out,size_t capacity) {
    size_t n=0;
    for(;;) {
      const int c=character();if(c<0)return c;
      if(c=='\r') {const int end=character();if(end<0)return end;if(end!='\n')return -9;out[n]=0;return int(n);}
      if(c=='\n'||n>=capacity-1)return -9;
      out[n++]=char(c);
    }
  }
  int copy(int length) {
    int remaining=length;
    while(remaining!=0) {
      const size_t count=remaining>0&&remaining<1024?size_t(remaining):1024;
      const int n=receive(fixedHttpBuffer(),count);
      if(n<0)return n;
      if(n==0)return remaining<0?0:-10;
      if(n>INT_MAX-total_)return -9;
      size_t written=sink_.write(fixedHttpBuffer(),size_t(n));
      if(written>size_t(n))return -10;
      if(written!=size_t(n)) {
        if(sink_.getWriteError())sink_.clearWriteError();
        clock_.pause();
        const size_t left=size_t(n)-written;
        if(sink_.write(fixedHttpBuffer()+written,left)!=left)return -10;
      }
      if(sink_.getWriteError())return -10;
      total_+=n;if(remaining>0)remaining-=n;
    }
    return 0;
  }
 public:
  // deadline: whole-body limit in clock units; 0 = none.
  FixedHttpBody(Client &client,Sink &sink,Clock &clock,uint32_t timeout,uint32_t deadline=0):
    client_(client),sink_(sink),clock_(clock),timeout_(timeout),progress_(clock.now()),deadline_(deadline),started_(clock.now()) {}
  int read(int length,bool chunked) {
    if(!chunked){const int result=copy(length);return result<0?result:total_;}
    char header[128];
    for(;;) {
      const int count=line(header,sizeof(header));if(count<0)return count;
      int chunk=0,digits=0;size_t i=0;
      for(;header[i];++i) {
        const char c=header[i];
        const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(digit<0)break;
        if(chunk>(INT_MAX-digit)/16)return -9;
        chunk=chunk*16+digit;++digits;
      }
      if(!digits || (header[i] && header[i]!=';' && header[i]!=' ' && header[i]!='\t'))return -9;
      if(chunk==0) {
        // Consume optional trailers through the final empty line.
        size_t trailerBytes=0;
        do {
          const int trailer=line(header,sizeof(header));if(trailer<0)return trailer;
          trailerBytes+=size_t(trailer)+2;if(trailerBytes>8192)return -9;
          if(!trailer)break;
        } while(true);
        return length>0&&length!=total_?-10:total_;
      }
      if(chunk>INT_MAX-total_)return -9;
      const int result=copy(chunk);if(result<0)return result;
      const int cr=character();if(cr<0)return cr;
      const int lf=character();if(lf<0)return lf;
      if(cr!='\r'||lf!='\n')return -9;
    }
  }
};
