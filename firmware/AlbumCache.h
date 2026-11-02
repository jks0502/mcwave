#pragma once
#include "RemoteSelection.h"
#if defined(ESP_PLATFORM) || defined(CLOCK_ALBUM_TESTING)
bool startAlbumCache(const char *protectedId="");
void reconcileAlbumCache();
bool cacheAlbumPhoto(const char *id,const char *protectedId);
bool loadAlbumPhoto(const char *id,uint8_t *pixels);
uint8_t albumCacheCount();
bool nextCachedPhoto(const char *current,bool random,char *next);
#else
inline bool startAlbumCache(const char *protectedId=""){return false;}
inline void reconcileAlbumCache(){}
inline bool cacheAlbumPhoto(const char *,const char *){return false;}
inline bool loadAlbumPhoto(const char *,uint8_t *){return false;}
inline uint8_t albumCacheCount(){return 0;}
inline bool nextCachedPhoto(const char *,bool,char *){return false;}
#endif
