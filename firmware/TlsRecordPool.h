#pragma once
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>

// ESP32's SDK uses 16 KiB TLS records plus protocol overhead. Keep two
// byte-accessible blocks available regardless of general-heap fragmentation.
class TlsRecordPool {
 public:
  static constexpr size_t capacity=17408;
  static bool recordSize(size_t count,size_t size) {
    return size&&count<=std::numeric_limits<size_t>::max()/size&&count*size>=16384&&count*size<=capacity;
  }
  void *claim(size_t count,size_t size) {
    if(!recordSize(count,size))return nullptr;
    for(unsigned i=0;i<2;++i)if(!used_[i]){used_[i]=true;std::memset(blocks_[i],0,count*size);return blocks_[i];}
    return nullptr;
  }
  bool release(void *ptr) {
    for(unsigned i=0;i<2;++i)if(ptr==blocks_[i]){std::memset(blocks_[i],0,capacity);used_[i]=false;return true;}
    return false;
  }
 private:
  alignas(16) uint8_t blocks_[2][capacity]={};
  bool used_[2]={};
};
#if defined(ARDUINO_ARCH_ESP32)
void reportTlsMemory();
#endif
