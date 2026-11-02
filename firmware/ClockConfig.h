#pragma once
#include <stdint.h>

// edison shares these pages and defines its own version before including.
#ifndef CLOCK_FIRMWARE_VERSION
#if defined(MCWAVE_NET_DIAGNOSTIC)
#define CLOCK_FIRMWARE_VERSION "V6.4.0-netdiag"
#elif defined(MCWAVE_RWDT_DIAGNOSTIC)
#define CLOCK_FIRMWARE_VERSION "V6.3.2-test-rwdt"
#elif defined(MCWAVE_MULTI_NETWORK)
#define CLOCK_FIRMWARE_VERSION "V7.1.0"
#else
#define CLOCK_FIRMWARE_VERSION "V6.4.1"
#endif
#endif
constexpr char kFirmwareVersion[] = CLOCK_FIRMWARE_VERSION;
// 1: HINK-E042A03-A1, full refresh then sleep. 0: original Waveshare V2.
#ifndef CLOCK_HINK_FULL_REFRESH_ONLY
#define CLOCK_HINK_FULL_REFRESH_ONLY 1
#endif
constexpr bool kHinkFullRefreshOnly = CLOCK_HINK_FULL_REFRESH_ONLY != 0;
constexpr uint32_t kScreenAutoRefreshMinMs = kHinkFullRefreshOnly ? 60000UL : 10000UL;
constexpr uint32_t kSportsLiveIntervalMs = 60UL * 1000;
constexpr uint32_t kSportsIdleIntervalMs = 30UL * 60 * 1000;

// China Weather's Jiangning district station, not phone GPS.
constexpr char kWeatherUrl[] = "https://d1.weather.com.cn/sk_2d/101190104.html";
constexpr char kChinaForecastUrl[] = "https://www.weather.com.cn/weather/101190104.shtml";
constexpr char kNmcWeatherUrl[] = "https://www.nmc.cn/rest/weather?stationid=Dfezs";
constexpr char kNmcRainUrl[] = "https://www.nmc.cn/publish/forecast/AJS/jiangning.html";
// Public China Weather/Huafeng Jiangning district page; location ID is not an API key.
constexpr char kRainProbabilityUrl[] = "https://platform.weathercn.com/api/live-data/city/2333030";
constexpr uint32_t kWeatherIntervalMs = 10UL * 60 * 1000;
constexpr uint32_t kWeatherRetryMs = 60UL * 1000;
constexpr uint32_t kWifiRetryMs = 30UL * 1000;
constexpr uint32_t kFullRefreshMs = 15UL * 60 * 1000;
constexpr int kWidth = 400;
constexpr int kHeight = 300;

inline bool elapsed(uint32_t now, uint32_t start, uint32_t interval) {
  return uint32_t(now - start) >= interval;
}
