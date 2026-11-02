#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdint.h>
#include "cJSON.h"

struct WeatherData {
  bool valid = false;
  double temperature = 0;
  int humidity = 0;
  int code = -1;
  char dataTime[17] = {};
  uint8_t forecastCount = 0;
  uint16_t forecastStepMinutes = 60;
  char forecastTime[8][17] = {};
  int forecastCode[8] = {};
};

// Both API timestamps and the screen clock use Asia/Shanghai local time.
inline bool weatherMinute(const char *stamp, int64_t &minute) {
  int y = 0, m = 0, d = 0, h = -1, n = -1;
  if (!stamp || std::strlen(stamp) != 16 || stamp[4] != '-' || stamp[7] != '-' ||
      stamp[10] != 'T' || stamp[13] != ':' ||
      std::sscanf(stamp,"%4d-%2d-%2dT%2d:%2d",&y,&m,&d,&h,&n) != 5 ||
      y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || n < 0 || n > 59) return false;
  tm local = {};
  local.tm_year = y-1900; local.tm_mon = m-1; local.tm_mday = d;
  local.tm_hour = h; local.tm_min = n;
  const time_t seconds = std::mktime(&local);
  if (seconds == time_t(-1) || local.tm_year != y-1900 || local.tm_mon != m-1 ||
      local.tm_mday != d || local.tm_hour != h || local.tm_min != n) return false;
  minute = int64_t(seconds)/60;
  return true;
}

// Validate a whole response before replacing the last successful reading.
inline bool parseWeather(const char *json, WeatherData &result) {
  cJSON *root = cJSON_Parse(json);
  if (!root) return false;
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
  const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
  const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
  const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(current, "time");
  int64_t currentMinute = 0;
  bool ok = cJSON_IsNumber(temp) && std::isfinite(temp->valuedouble) &&
      temp->valuedouble >= -100 && temp->valuedouble <= 70 &&
      cJSON_IsNumber(humidity) && humidity->valuedouble >= 0 &&
      humidity->valuedouble <= 100 && humidity->valuedouble == humidity->valueint &&
      cJSON_IsNumber(code) && code->valuedouble >= 0 && code->valuedouble <= 99 &&
      code->valuedouble == code->valueint && cJSON_IsString(stamp) &&
      weatherMinute(stamp->valuestring,currentMinute);
  if (ok) {
    result.temperature = temp->valuedouble;
    result.humidity = humidity->valueint;
    result.code = code->valueint;
    std::memcpy(result.dataTime, stamp->valuestring, 17);
    result.valid = true;
    // Missing/malformed forecast must not discard usable current weather or
    // retain an old forecast from a previous response.
    result.forecastCount = 0;
    result.forecastStepMinutes = 60;
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root,"hourly");
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly,"time");
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(hourly,"weather_code");
    const int count = cJSON_GetArraySize(times);
    bool forecastOk = cJSON_IsArray(times) && cJSON_IsArray(codes) &&
                      count > 0 && count <= 8 && cJSON_GetArraySize(codes) == count;
    int64_t previousMinute = 0;
    for (int i = 0; forecastOk && i < count; ++i) {
      const cJSON *t = cJSON_GetArrayItem(times,i);
      const cJSON *c = cJSON_GetArrayItem(codes,i);
      int64_t forecastMinute = 0;
      forecastOk = cJSON_IsString(t) && weatherMinute(t->valuestring,forecastMinute) &&
                   std::atoi(t->valuestring+14) == 0 &&
                   (i == 0 || forecastMinute == previousMinute+60) &&
                   cJSON_IsNumber(c) && c->valuedouble == c->valueint &&
                   c->valueint >= 0 && c->valueint <= 99;
      if (forecastOk) {
        std::memcpy(result.forecastTime[i],t->valuestring,17);
        result.forecastCode[i] = c->valueint;
        previousMinute = forecastMinute;
      }
    }
    if (forecastOk) result.forecastCount = uint8_t(count);
  }
  cJSON_Delete(root);
  return ok;
}

inline const char *weatherLabel(int code) {
  switch (code) {
    case 0: return "晴";
    case 1: return "晴间多云";
    case 2: return "多云";
    case 3: return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 56: case 57: case 66: case 67: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: case 81: case 82: return "阵雨";
    case 85: case 86: return "阵雪";
    case 95: return "雷雨";
    case 96: case 99: return "雷雨冰雹";
    default: return "未知天气";
  }
}

inline bool isRainCode(int code) {
  return (code >= 51 && code <= 57) || (code >= 61 && code <= 67) ||
         (code >= 80 && code <= 82) || code == 95 || code == 96 || code == 99;
}

inline void weatherTrend(const WeatherData &weather, const tm *local,
                         bool usable, char *text, size_t size) {
  std::snprintf(text,size,"降雨趋势待更新");
  if (!usable || !weather.valid || !local || !weather.forecastCount ||
      std::strcmp(weatherLabel(weather.code),"未知天气") == 0) return;
  char stamp[17];
  std::strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M",local);
  int64_t now = 0;
  if (!weatherMinute(stamp,now)) return;
  const bool raining = isRainCode(weather.code);
  int covered = 0;
  for (int i = 0; i < weather.forecastCount; ++i) {
    int64_t at = 0;
    if (!weatherMinute(weather.forecastTime[i],at)) return;
    if (at <= now) continue;
    if (at > now+360) break;
    const int code = weather.forecastCode[i];
    if (std::strcmp(weatherLabel(code),"未知天气") == 0) return;
    ++covered;
    if (!raining && isRainCode(code)) {
      std::snprintf(text,size,"预计%s%02d时前后%s",
                    std::strncmp(stamp,weather.forecastTime[i],10) ? "明日" : "",
                    std::atoi(weather.forecastTime[i]+11),
                    code >= 95 ? "有雷雨" : "有雨");
      return;
    }
    // Require two consecutive dry forecast periods before reporting rain ending.
    if (raining && !isRainCode(code) && i+1 < weather.forecastCount &&
        std::strcmp(weatherLabel(weather.forecastCode[i+1]),"未知天气") != 0 &&
        !isRainCode(weather.forecastCode[i+1])) {
      const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
      std::snprintf(text,size,"预计%s%02d时前后%s",
                    std::strncmp(stamp,weather.forecastTime[i],10) ? "明日" : "",
                    std::atoi(weather.forecastTime[i]+11),
                    snow ? "转雪" : "雨停");
      return;
    }
  }
  if (covered >= 360 / weather.forecastStepMinutes)
    std::snprintf(text,size,raining ? "未来6小时雨势持续" : "未来6小时暂无降雨");
}
