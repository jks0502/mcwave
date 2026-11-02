#pragma once
#include "RemoteSelection.h"
#if defined(ESP_PLATFORM) || defined(CLOCK_SCREEN_MIRROR_TESTING)
void remoteScreenPreparing();
void remoteScreenRendered(const uint8_t *frame,const char *mode);
void remoteScreenAllow(bool allow);
#else
inline void remoteScreenPreparing(){}
inline void remoteScreenRendered(const uint8_t *,const char *){}
inline void remoteScreenAllow(bool){}
#endif
#if defined(ESP_PLATFORM) || defined(CLOCK_ALBUM_TESTING)
bool remoteStartAlbum(RemoteCommand &command);
void remoteAlbumRendered(const RemoteCommand &command);
#else
inline bool remoteStartAlbum(RemoteCommand &){return false;}
inline void remoteAlbumRendered(const RemoteCommand &){}
#endif
#if defined(ESP_PLATFORM)
// OTA (V5.5): classify the boot early in setup(); the loop rolls back a trial
// image that never reached the website.
void remoteOtaBoot();
void remoteOtaWatch();
#else
inline void remoteOtaBoot(){}
inline void remoteOtaWatch(){}
#endif
#if defined(ESP_PLATFORM) || defined(CLOCK_REMOTE_TESTING)
void startRemoteControl(bool &mood);
void startRemoteTask();       // 6.0.0: the network task that runs exchangeRemoteControl.
void exchangeRemoteControl();
bool takeRemoteCommand(RemoteCommand &command);
void remoteRendered(bool mood,uint32_t revision);
void remoteDropConnection(); // 6.1.2: free the kept website connection before a weather query
#else
inline void remoteDropConnection(){}
inline void startRemoteControl(bool &){}
inline void startRemoteTask(){}
inline void exchangeRemoteControl(){}
inline bool takeRemoteCommand(RemoteCommand &){return false;}
inline void remoteRendered(bool,uint32_t){}
#endif
