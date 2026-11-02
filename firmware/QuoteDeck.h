#pragma once
#include <stdint.h>
#include <cstring>
#ifdef CLOCK_NAILONG_ASSETS
#include "../EdisonAlbum/nailong/Quotes.h"
#else
#include "Quotes.h"
#endif

// Uniform draw among unused sayings, with a compact 13-byte bitset.
class QuoteDeck {
  static_assert(kQuoteCount>=2 && kQuoteCount<=255,"Quote indices must fit in one byte");
 public:
  const char *next(uint32_t (*randomValue)()) {
    if (!remaining_) {std::memset(used_,0,sizeof(used_));remaining_=kQuoteCount;}
    const bool boundary=remaining_==kQuoteCount && previous_<kQuoteCount;
    const uint32_t bound=remaining_-(boundary?1:0);
    const uint32_t threshold=uint32_t(-bound)%bound;
    uint32_t sample;do {sample=randomValue();}while(sample<threshold);
    uint32_t rank=sample%bound;
    for(uint8_t i=0;i<kQuoteCount;++i) {
      if((used_[i/8]&(1U<<(i%8))) || (boundary && i==previous_))continue;
      if(rank){--rank;continue;}
      used_[i/8]|=uint8_t(1U<<(i%8));previous_=i;--remaining_;break;
    }
    return kQuotes[previous_];
  }
 private:
  uint8_t used_[(kQuoteCount+7)/8]={};
  uint8_t remaining_ = kQuoteCount;
  uint8_t previous_ = kQuoteCount;
};
