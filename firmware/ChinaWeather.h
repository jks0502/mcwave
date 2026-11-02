#pragma once
#include <cstdlib>
#include "WeatherData.h"

inline int chinaWeatherCode(int code) {
  switch(code) {
    case 0:return 0;case 1:return 2;case 2:return 3;
    case 3:return 80;case 4:case 5:return 95;case 6:return 66;
    case 7:case 21:return 61;case 8:case 22:return 63;
    case 9:case 10:case 11:case 12:case 23:case 24:case 25:return 65;
    case 13:return 85;case 14:case 26:return 71;case 15:case 27:return 73;
    case 16:case 17:case 28:return 75;case 18:case 20:case 29:case 30:case 31:case 53:return 45;
    case 19:return 67;default:return -1;
  }
}
inline bool parseChinaWeather(const char *json,const tm &today,WeatherData &result) {
  cJSON *root=cJSON_Parse(json);if(!root)return false;
  const auto field=[&](const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);return cJSON_IsString(v)?v->valuestring:"";};
  WeatherData next;
  char *end=nullptr;next.temperature=std::strtod(field("temp"),&end);
  const bool temperatureOk=end && end!=field("temp") && *end==0 && std::isfinite(next.temperature) && next.temperature>=-100 && next.temperature<=70;
  int humidity=-1,hour=-1,minute=-1,month=-1,day=-1,code=-1;
  const char *condition=field("weathercode");
  bool ok=std::strcmp(field("city"),"101190104")==0 && temperatureOk &&
      std::sscanf(field("SD"),"%d%%",&humidity)==1 && humidity>=0 && humidity<=100 &&
      std::sscanf(field("time"),"%d:%d",&hour,&minute)==2 && hour>=0 && hour<24 && minute>=0 && minute<60 &&
      std::sscanf(field("date"),"%d月%d",&month,&day)==2 && month==today.tm_mon+1 && day==today.tm_mday &&
      (condition[0]=='d'||condition[0]=='n') && std::sscanf(condition+1,"%d",&code)==1 && chinaWeatherCode(code)>=0;
  if(ok) {
    next.humidity=humidity;next.code=chinaWeatherCode(code);next.valid=true;
    std::snprintf(next.dataTime,sizeof(next.dataTime),"%04d-%02d-%02dT%02d:%02d",today.tm_year+1900,month,day,hour,minute);
    int64_t stamp=0;ok=weatherMinute(next.dataTime,stamp);
  }
  cJSON_Delete(root);if(ok)result=next;return ok;
}
inline bool parseChinaForecast(const char *json,const tm &today,WeatherData &result) {
  cJSON *root=cJSON_Parse(json);if(!root)return false;
  const cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"1d");
  WeatherData next=result;next.forecastCount=0;next.forecastStepMinutes=180;bool ok=cJSON_IsArray(items);
  int64_t previous=0;
  for(const cJSON *item=ok?items->child:nullptr;item && ok && next.forecastCount<8;item=item->next) {
    int day=0,hour=-1,code=-1;
    ok=cJSON_IsString(item) && std::sscanf(item->valuestring,"%d日%d时,%*c%d",&day,&hour,&code)==3 && hour>=0 && hour<24 && chinaWeatherCode(code)>=0;
    if(!ok)break;
    tm stamp=today;stamp.tm_hour=hour;stamp.tm_min=0;stamp.tm_sec=0;
    // Forecasts cover the coming 24 hours, including month/year rollover.
    if(day!=today.tm_mday){++stamp.tm_mday;std::mktime(&stamp);}
    if(day!=stamp.tm_mday){ok=false;break;}
    char *time=next.forecastTime[next.forecastCount];std::strftime(time,17,"%Y-%m-%dT%H:%M",&stamp);
    int64_t minute=0;ok=weatherMinute(time,minute) && (!next.forecastCount || minute==previous+180);
    if(ok){next.forecastCode[next.forecastCount++]=chinaWeatherCode(code);previous=minute;}
  }
  cJSON_Delete(root);
  if(ok && next.forecastCount){result=next;return true;}
  result.forecastCount=0;return false;
}
