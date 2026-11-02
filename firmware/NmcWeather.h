#pragma once
#include <string>
#include "ChinaWeather.h"
#include "RainForecast.h"
#include "JsonObjects.h"

struct NmcWeatherDiagnostic {
  const char *reason="json";
  int image=-2;
  bool unknown=false;
};
inline bool parseNmcWeather(const char *json,const tm &today,WeatherData &result,NmcWeatherDiagnostic *diagnostic=nullptr) {
  NmcWeatherDiagnostic detail;if(diagnostic)*diagnostic=detail;
  cJSON *root=cJSON_Parse(json);if(!root)return false;
  const cJSON *real=cJSON_GetObjectItemCaseSensitive(root,"real");
  if(!real)real=root; // Streaming path captures only the small realtime object.
  const cJSON *station=cJSON_GetObjectItemCaseSensitive(real,"station");
  const cJSON *w=cJSON_GetObjectItemCaseSensitive(real,"weather");
  const auto field=[](const cJSON *object,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(object,key);return cJSON_IsString(v)?v->valuestring:"";};
  const cJSON *t=cJSON_GetObjectItemCaseSensitive(w,"temperature"),*h=cJSON_GetObjectItemCaseSensitive(w,"humidity");
  const char *stamp=field(real,"publish_time"),*img=field(w,"img");
  const cJSON *image=cJSON_GetObjectItemCaseSensitive(w,"img");WeatherData next;
  const auto reject=[&](const char *reason){detail.reason=reason;return false;};
  const bool ok=[&]() {
    if(std::strcmp(field(station,"code"),"Dfezs") ||
       (std::strcmp(field(station,"city"),"江宁")&&std::strcmp(field(station,"url"),"/publish/forecast/AJS/jiangning.html")))return reject("station");
    if(!cJSON_IsNumber(t)||!std::isfinite(t->valuedouble)||t->valuedouble<-100||t->valuedouble>70)return reject("temperature");
    if(!cJSON_IsNumber(h)||h->valuedouble<0||h->valuedouble>100||h->valuedouble!=h->valueint)return reject("humidity");
    const bool missing=!image||cJSON_IsNull(image)||(cJSON_IsString(image)&&!*img);
    const bool unavailable=(cJSON_IsString(image)&&!std::strcmp(img,"9999"))||(cJSON_IsNumber(image)&&image->valuedouble==9999);
    detail.unknown=missing||unavailable;
    if(detail.unknown){detail.image=unavailable?9999:-1;next.code=-1;}
    else {
      char *end=nullptr;const long code=std::strtol(img,&end,10);
      if(code>=0&&code<=9999)detail.image=int(code);
      if(!cJSON_IsString(image)||!*img||!end||*end||code<0||code>99||chinaWeatherCode(int(code))<0)return reject("weather_code");
      next.code=chinaWeatherCode(int(code));
    }
    if(std::strlen(stamp)!=16)return reject("time_format");
    std::memcpy(next.dataTime,stamp,17);next.dataTime[10]='T';int64_t minute=0;
    if(!weatherMinute(next.dataTime,minute))return reject("time_format");
    tm now=today;const int64_t wall=std::mktime(&now)/60;
    if(minute>wall+5||minute<wall-180)return reject("time_range");
    next.temperature=t->valuedouble;next.humidity=h->valueint;next.valid=true;
    detail.reason=detail.unknown?"unknown_weather":"ok";return true;
  }();
  cJSON_Delete(root);if(diagnostic)*diagnostic=detail;if(ok)result=next;return ok;
}

// Read just each small hourly table cell; never store the 75 KB HTML page.
class NmcRainReader {
  RainForecast next_;tm today_,base_={},day_={};
  std::string tag_,text_,item_;
  bool inTag_=false,skipTag_=false,calendar_=false,dateText_=false,haveDate_=false,table_=false,identity_=false,ended_=false,ok_=true;
  int depth_=0,previousHour_=-1,dailyCount_=0,dailyCode_[14]={};size_t bytes_=0;
  bool hour() {
    std::string values[9];int count=0;bool tag=false;std::string value;
    for(char c:item_) {
      if(c=='<') {if(!value.empty() && count<9){values[count++]=value;value.clear();}tag=true;}
      else if(c=='>')tag=false;
      else if(!tag && !std::isspace((unsigned char)c))value+=c;
    }
    if(!count || !haveDate_)return false;
    int day=-1,hour=-1,minute=-1;
    const bool explicitDay=values[0].find("日")!=std::string::npos;
    if(explicitDay) {
      if(std::sscanf(values[0].c_str(),"%d日%d:%d",&day,&hour,&minute)!=3)return false;
      if(day!=day_.tm_mday){++day_.tm_mday;std::mktime(&day_);if(day!=day_.tm_mday)return false;}
    } else {
      if(std::sscanf(values[0].c_str(),"%d:%d",&hour,&minute)!=2)return false;
      if(previousHour_>=0 && hour<previousHour_){++day_.tm_mday;std::mktime(&day_);}
    }
    if(hour<0 || hour>=24 || minute!=0)return false;previousHour_=hour;
    tm stamp=day_;stamp.tm_hour=hour;const int64_t m=std::mktime(&stamp)/60;
    const char *icon=std::strstr(item_.c_str(),"/40x40/3/");int image=-1;
    if(!icon || std::sscanf(icon+9,"%d.png",&image)!=1)return false;
    // Adjacent day panels repeat two slots. Keep only one chronological slot.
    if(next_.count && m<=next_.minute[next_.count-1])return true;
    // NMC may omit early hours of a new day. Keep the missing times unknown
    // instead of rejecting the complete page or inventing dry weather.
    if(next_.count && (m-next_.minute[next_.count-1])%180!=0)return false;
    while(next_.count && next_.minute[next_.count-1]+180<m && next_.count<kRainSlots) {
      const int index=next_.count++;
      next_.minute[index]=next_.minute[index-1]+180;next_.code[index]=-1;
    }
    if(next_.count>=kRainSlots)return true;
    int code=chinaWeatherCode(image);
    // Some NMC cells have a missing icon (9999). Rainfall amount alone also
    // includes snow, so confirm its type with NMC's day/night forecast.
    if(code<0 && count>=2) {
      tm midnight=stamp;midnight.tm_hour=0;
      tm base=base_;const int date=int((std::mktime(&midnight)-std::mktime(&base))/86400);
      const int period=hour<8?(date-1)*2+1:date*2+(hour>=20?1:0);
      double amount=0;char unit[4]={};
      if(period>=0 && period<dailyCount_ && std::sscanf(values[1].c_str(),"%lf%3s",&amount,unit)==2 &&
         std::strcmp(unit,"mm")==0 && amount>0) {
        const int daily=dailyCode_[period];
        if(isRainCode(daily) || (daily>=71 && daily<=77) || daily==85 || daily==86)code=daily;
      }
    }
    const int index=next_.count++;next_.minute[index]=m;next_.code[index]=code;
    return true;
  }
  void tag() {
    if(tag_.find("id=day7")!=std::string::npos || tag_.find("id=\"day7\"")!=std::string::npos)calendar_=true;
    if(calendar_ && !haveDate_ && (tag_.find("class=date>")!=std::string::npos || tag_.find("class=\"date\"")!=std::string::npos)){dateText_=true;text_.clear();}
    if(tag_.find("id=hourValues")!=std::string::npos || tag_.find("id=\"hourValues\"")!=std::string::npos)table_=true;
    if(calendar_ && !table_ && dailyCount_<14) {
      const size_t icon=tag_.find("/40x40/4/");int image=-1;
      if(icon!=std::string::npos && std::sscanf(tag_.c_str()+icon+9,"%d.png",&image)==1)dailyCode_[dailyCount_++]=chinaWeatherCode(image);
    }
    if(table_ && !depth_) {
      size_t pos=tag_.find("id=day");if(pos==std::string::npos)pos=tag_.find("id=\"day");
      if(pos!=std::string::npos){const size_t digit=tag_.find_first_of("0123456",pos);if(digit!=std::string::npos){day_=base_;day_.tm_mday+=tag_[digit]-'0';std::mktime(&day_);previousHour_=-1;}}
      if(tag_.find("class=\"hour3")!=std::string::npos || tag_.find("class=hour3")!=std::string::npos){depth_=1;item_.clear();return;}
    }
    if(depth_) {
      item_+=tag_;
      if(tag_.compare(0,4,"<div")==0)++depth_;
      else if(tag_=="</div>" && --depth_==0)ok_=hour();
      if(item_.size()>1536)ok_=false;
    }
    if((tag_.find("name=stationId")!=std::string::npos || tag_.find("name=\"stationId\"")!=std::string::npos) &&
       (tag_.find("value=Dfezs")!=std::string::npos || tag_.find("value=\"Dfezs\"")!=std::string::npos))identity_=true;
    if(tag_=="</html>")ended_=true;
  }
 public:
  explicit NmcRainReader(const tm &today):today_(today) {tm copy=today;next_.fetchedMinute=std::mktime(&copy)/60;}
  bool put(char c) {
    if(!ok_ || ++bytes_>512UL*1024)return ok_=false;
    if(c=='<') {
      if(dateText_) {
        int month=0,day=0;dateText_=false;
        if(std::sscanf(text_.c_str(),"%d/%d",&month,&day)!=2 || month<1 || month>12 || day<1 || day>31)return ok_=false;
        base_=today_;base_.tm_hour=base_.tm_min=base_.tm_sec=0;base_.tm_mon=month-1;base_.tm_mday=day;
        if(today_.tm_mon==0 && month==12)--base_.tm_year;
        if(today_.tm_mon==11 && month==1)++base_.tm_year;
        const int64_t start=std::mktime(&base_)/60;
        if(base_.tm_mon!=month-1 || base_.tm_mday!=day || start>next_.fetchedMinute || start<next_.fetchedMinute-2880)return ok_=false;
        haveDate_=true;day_=base_;
      }
      inTag_=true;skipTag_=false;tag_="<";
    } else if(inTag_) {
      if(tag_.size()>=768){if(depth_)return ok_=false;skipTag_=true;}
      if(!skipTag_)tag_+=c;
      if(c=='>'){inTag_=false;if(!skipTag_)tag();}
    } else {
      if(dateText_){if(text_.size()>=32)return ok_=false;text_+=c;}
      if(depth_){if(item_.size()>=1536)return ok_=false;item_+=c;}
    }
    return ok_;
  }
  bool finish(RainForecast &result) const {
    if(!ok_ || !ended_ || !identity_ || !haveDate_ || depth_ || next_.count<8 || !validRainForecast(next_))return false;
    result=next_;return true;
  }
};

// Public China Weather/Huafeng dashboard response. Only its hourly array is parsed.
class RainProbabilityReader {
  RainForecast &next_;JsonObjects objects_;
  std::string needle_="\"hourly\":";
  std::string identityTail_;
  size_t matched_=0,bytes_=0;int depth_=0;
  bool identity_=false,found_=false,quoted_=false,escaped_=false,done_=false,ok_=true;unsigned rows_=0;
  static bool collect(const cJSON *row,void *context) {return static_cast<RainProbabilityReader *>(context)->hour(row);}
  bool hour(const cJSON *row) {
    const cJSON *dt=cJSON_GetObjectItemCaseSensitive(row,"DateTime");
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(row,"RainProbability");
    if(!cJSON_IsString(dt) || std::strlen(dt->valuestring)!=25 || std::strcmp(dt->valuestring+19,"+08:00")!=0 ||
       std::strncmp(dt->valuestring+16,":00",3)!=0)return false;
    char time[17];std::memcpy(time,dt->valuestring,16);time[16]=0;int64_t minute=0;
    if(!weatherMinute(time,minute))return false;++rows_;
    if(!cJSON_IsNumber(p))return true; // Missing is unknown; never fabricate 0%.
    if(!std::isfinite(p->valuedouble) || p->valuedouble<0 || p->valuedouble>100 || p->valuedouble!=p->valueint)return false;
    if(minute<=next_.fetchedMinute && next_.fetchedMinute<minute+60){next_.currentProbability=p->valueint;next_.currentProbabilityMinute=minute;}
    for(int i=0;i<next_.count;++i)if(next_.minute[i]==minute)next_.probability[i]=p->valueint;
    return true;
  }
 public:
  explicit RainProbabilityReader(RainForecast &next):next_(next),objects_(collect,this) {}
  bool put(char c) {
    if(!ok_ || ++bytes_>1024UL*1024)return ok_=false;
    if(done_)return true;
    if(!identity_) {
      identityTail_+=c;if(identityTail_.size()>64)identityTail_.erase(0,1);
      identity_=identityTail_.find("\"location_key\":\"2333030\"")!=std::string::npos;
      return true;
    }
    if(!found_) {
      if(c==needle_[matched_])++matched_;else matched_=c==needle_[0]?1:0;
      if(matched_==needle_.size())found_=true;return true;
    }
    if(!depth_ && std::isspace((unsigned char)c))return true;
    if(!depth_ && c!='[')return ok_=false;
    if(!objects_.put(c))return ok_=false;
    if(quoted_) {if(escaped_)escaped_=false;else if(c=='\\')escaped_=true;else if(c=='"')quoted_=false;}
    else {if(c=='"')quoted_=true;else if(c=='[')++depth_;else if(c==']' && --depth_==0)done_=true;}
    return true;
  }
  bool finish() const {return ok_ && identity_ && done_ && rows_ && objects_.complete();}
};
