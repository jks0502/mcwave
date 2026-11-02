#pragma once
#include <cstring>
#include <cstdio>
#include <cstdint>

// Count first, then fill the exactly sized result. UTF-8 stays unchanged,
// as with cJSON, without building a JSON tree or temporary strings.
class SportsJsonWriter {
  char *out_;
  size_t size_=0;
 public:
  explicit SportsJsonWriter(char *out=nullptr):out_(out) {}
  size_t size() const{return size_;}
  void byte(char value){if(out_)out_[size_]=value;++size_;}
  void text(const char *value){const size_t n=std::strlen(value);if(out_)std::memcpy(out_+size_,value,n);size_+=n;}
  void number(int64_t value){char digits[32];std::snprintf(digits,sizeof(digits),"%lld",static_cast<long long>(value));text(digits);}
  void string(const char *value) {
    byte('"');
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(value);*p;++p) {
      switch(*p) {
        case '"':text("\\\"");break;
        case '\\':text("\\\\");break;
        case '\b':text("\\b");break;
        case '\f':text("\\f");break;
        case '\n':text("\\n");break;
        case '\r':text("\\r");break;
        case '\t':text("\\t");break;
        default:
          if(*p<32){text("\\u00");byte("0123456789abcdef"[*p>>4]);byte("0123456789abcdef"[*p&15]);}
          else byte(char(*p));
      }
    }
    byte('"');
  }
};

// Leave room for UI/task housekeeping as well as the terminating NUL.
inline bool sportsJsonFitsHeap(size_t bytes,size_t freeBytes,size_t largestBlock) {
  return freeBytes>=4097 && largestBlock>=2049 &&
         bytes<=freeBytes-4097 && bytes<=largestBlock-2049;
}
