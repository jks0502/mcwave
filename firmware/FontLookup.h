#pragma once
#include <cstddef>
#include <cstdint>

// Font generators order records by (size, code); bitmap offsets stay intact.
template<class GlyphType,size_t N>
inline const GlyphType *findFontGlyph(const GlyphType (&table)[N],uint32_t code,int size) {
  size_t first=0,last=N;
  while(first<last) {
    const size_t middle=first+(last-first)/2;
    const auto &g=table[middle];
    if(g.size<size || (g.size==size && g.code<code))first=middle+1;
    else last=middle;
  }
  return first<N && table[first].size==size && table[first].code==code?&table[first]:nullptr;
}
