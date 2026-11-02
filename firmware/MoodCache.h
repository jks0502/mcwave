#pragma once
#include <cstdint>
// mcwave 6.0.0 two-person mood page. Each person's browser draws their half
// (200x300, 1 bit per pixel, same bit order as the album) and the website
// stores it; the device keeps the latest copy of each half in SPIFFS.
constexpr uint32_t kMoodHalfBytes=200/8*300;
#if defined(ESP_PLATFORM)
void moodVersionsOffered(const char *json); // network task: from the sync reply
bool moodDownloadPending();                  // an offered version differs from the stored one
void moodDownload();                        // network task, network lock held
bool loadMoodHalf(uint8_t side,uint8_t *pixels); // UI task: writes columns of side 0/1
uint32_t moodGeneration();                  // changes whenever a stored half changes
#else
inline bool loadMoodHalf(uint8_t,uint8_t *){return false;}
#ifdef CLOCK_MOOD_TESTING
uint32_t moodGeneration();
#else
inline uint32_t moodGeneration(){return 0;}
#endif
#endif
