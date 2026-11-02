#pragma once
#include <cstring>
#include <cstdint>
constexpr uint8_t kAlbumCacheSlots=20; // Scan legacy slots too; never orphan old files.
constexpr uint8_t kAlbumCacheLimit=20;
// Only an authenticated, explicit absence may invalidate a cached photo.
inline bool albumPhotoAbsent(int status) { return status==404; }
template<class ReadId,class Remove>
uint8_t trimAlbumCache(const char *protectedId,ReadId readId,Remove remove) {
  int protectedSlot=-1;char id[65];
  for(uint8_t i=0;i<kAlbumCacheSlots;++i)
    if(readId(i,id)&&!std::strcmp(id,protectedId)){protectedSlot=i;break;}
  uint8_t kept=protectedSlot>=0?1:0;
  for(uint8_t i=0;i<kAlbumCacheSlots;++i) {
    if(i==protectedSlot)continue;
    if(readId(i,id)&&kept<kAlbumCacheLimit){++kept;continue;}
    remove(i); // Includes incomplete legacy files.
  }
  return kept;
}

// Bound TLS requests per pass while eventually visiting every cached photo.
template<class ReadId,class Visit>
void inspectAlbumCacheBatch(uint8_t &cursor,ReadId readId,Visit visit) {
  uint8_t checked=0;char id[65];
  for(uint8_t scanned=0;scanned<kAlbumCacheSlots&&checked<2;++scanned) {
    const uint8_t slot=cursor;cursor=(cursor+1)%kAlbumCacheSlots;
    if(readId(slot,id)){++checked;visit(slot,id);}
  }
}
