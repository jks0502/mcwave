#pragma once
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>
#include "cJSON.h"

constexpr int kMaxSportsEvents = 128;
struct SportsEvent {
  char id[32] = {}, title[96] = {}, value[40] = {}, status[48] = {}, meta[100] = {}, start[20] = {};
  uint8_t day = 0;
  bool live = false;
};
struct SportsData {
  bool valid = false;
  bool cached = false;
  uint8_t errors = 0;
  uint16_t count = 0;
  int64_t updated = 0;
  char date[11] = {};
  SportsEvent events[kMaxSportsEvents];
  bool hasLive() const { for (int i=0; i<count; ++i) if (events[i].live) return true; return false; }
};
inline bool sportsString(const cJSON *object, const char *key, char *out, size_t size) {
  const cJSON *field = cJSON_GetObjectItemCaseSensitive(object,key);
  if (!cJSON_IsString(field) || !field->valuestring || std::strlen(field->valuestring)>=size) return false;
  std::strcpy(out,field->valuestring);
  return true;
}
inline bool sportsReadEvent(const cJSON *item,SportsEvent &e,int previousDay) {
  const cJSON *day=cJSON_GetObjectItemCaseSensitive(item,"day");
  const cJSON *live=cJSON_GetObjectItemCaseSensitive(item,"live");
  const bool ok=cJSON_IsNumber(day) && day->valuedouble==day->valueint && day->valueint>=previousDay && day->valueint<=1 &&
      cJSON_IsBool(live) && sportsString(item,"id",e.id,sizeof(e.id)) && e.id[0] &&
      sportsString(item,"title",e.title,sizeof(e.title)) && e.title[0] &&
      sportsString(item,"value",e.value,sizeof(e.value)) &&
      sportsString(item,"status",e.status,sizeof(e.status)) && sportsString(item,"meta",e.meta,sizeof(e.meta));
  if(!ok)return false;
  e.start[0]=0; // Old NVS snapshots did not store kickoff time.
  if(cJSON_GetObjectItemCaseSensitive(item,"start")) {
    if(!sportsString(item,"start",e.start,sizeof(e.start)) || std::strlen(e.start)!=19)return false;
    for(int i=0;i<19;++i) {
      const char separator=i==4||i==7?'-':i==10?' ':i==13||i==16?':':0;
      if(separator?e.start[i]!=separator:e.start[i]<'0'||e.start[i]>'9')return false;
    }
  }
  e.day=uint8_t(day->valueint);e.live=cJSON_IsTrue(live);
  return true;
}
inline int sportsProvider(const SportsEvent &e) {
  return std::strncmp(e.id,"fb-",3)==0?1:std::strncmp(e.id,"nba-",4)==0?2:
         std::strncmp(e.id,"f1-",3)==0?4:std::strncmp(e.id,"cs-",3)==0?8:0;
}
struct SportsTeamName { const char *text; size_t length; };
inline SportsTeamName sportsTeamName(const char *text,size_t length,int provider) {
  const auto is=[&](const char *name){return std::strlen(name)==length && std::memcmp(text,name,length)==0;};
  const char *canonical=nullptr;
  if(provider==1) {
    if(is("皇马"))canonical="皇家马德里";
    else if(is("热刺"))canonical="托特纳姆热刺";
    else if(is("Viking"))canonical="维京";
    else if(is("纽卡斯尔联喷气机"))canonical="纽卡斯尔喷气机";
    else if(is("大田韩亚市民"))canonical="大田市民";
    else if(is("拉查布里府"))canonical="拉查武里";
    else if(is("阿尔加拉法"))canonical="加拉法";
    else if(is("East Bengal"))canonical="东孟加拉";
    else if(is("阿尔萨德"))canonical="萨德";
    else if(is("胡塞因"))canonical="伊尔比德侯赛因";
    else if(is("迪拜国民"))canonical="迪拜青年国民";
    else if(is("FC首尔"))canonical="首尔FC";
    else if(is("越电信体工"))canonical="越南军用电子电信";
  } else if(provider==8) {
    if(is("Natus Vincere"))canonical="NAVI";
    else if(is("BB")||is("BetBoom Team"))canonical="BetBoom";
    else if(is("Team Spirit"))canonical="Spirit";
    else if(is("Team Vitality"))canonical="Vitality";
    else if(is("FaZe Clan"))canonical="FaZe";
    else if(is("Ninjas in Pyjamas"))canonical="NIP";
  }
  return canonical?SportsTeamName{canonical,std::strlen(canonical)}:SportsTeamName{text,length};
}
inline bool sportsSameFixture(const SportsEvent &a,const SportsEvent &b) {
  if(std::strcmp(a.id,b.id)==0)return true;
  const int provider=sportsProvider(a);
  if(!provider || provider!=sportsProvider(b) || a.day!=b.day)return false;
  if(*a.start && *b.start) {
    if(std::strcmp(a.start,b.start)!=0)return false;
  } else if(provider!=1 && provider!=2)return false; // Legacy CS2 rematches need kickoff data.
  if(provider==4)return std::strcmp(a.title,b.title)==0;
  if(provider==8) {
    const char *aExtra=std::strstr(a.meta," · 补充"),*bExtra=std::strstr(b.meta," · 补充");
    const size_t aLength=aExtra?size_t(aExtra-a.meta):std::strlen(a.meta),bLength=bExtra?size_t(bExtra-b.meta):std::strlen(b.meta);
    if(aLength!=bLength || std::memcmp(a.meta,b.meta,aLength)!=0)return false;
  }
  const char *aVs=std::strstr(a.title," vs "),*bVs=std::strstr(b.title," vs ");
  if(!aVs || !bVs || aVs==a.title || bVs==b.title || !aVs[4] || !bVs[4])return false;
  const auto aLeft=sportsTeamName(a.title,size_t(aVs-a.title),provider),aRight=sportsTeamName(aVs+4,std::strlen(aVs+4),provider);
  const auto bLeft=sportsTeamName(b.title,size_t(bVs-b.title),provider),bRight=sportsTeamName(bVs+4,std::strlen(bVs+4),provider);
  const auto equal=[](SportsTeamName x,SportsTeamName y){return x.length==y.length && std::memcmp(x.text,y.text,x.length)==0;};
  return (equal(aLeft,bLeft)&&equal(aRight,bRight)) || (equal(aLeft,bRight)&&equal(aRight,bLeft));
}
inline int sportsFixtureQuality(const SportsEvent &event) {
  const bool score=std::strcmp(event.status,"未开始")!=0 &&
      ((std::strstr(event.value,":") && std::strpbrk(event.value,"0123456789")) || std::strstr(event.value,"圈"));
  const int stage=std::strcmp(event.status,"已结束")==0?2:event.live?1:0;
  return stage*2+int(score);
}
inline bool sportsContains(const cJSON *events,const SportsEvent &event) {
  for(const cJSON *item=events->child;item;item=item->next) {
    SportsEvent candidate;
    if(sportsReadEvent(item,candidate,0) && sportsSameFixture(candidate,event))return true;
  }
  return false;
}
// Validate everything before modifying the existing array. No second 40 KB
// snapshot is allocated: fragmented ESP32 heaps may have only 35 KB blocks.
inline bool parseSports(const char *json, SportsData &result, const char *expectedDate=nullptr) {
  cJSON *root = cJSON_Parse(json);
  if (!root) return false;
  const cJSON *events=cJSON_GetObjectItemCaseSensitive(root,"events");
  const cJSON *updated=cJSON_GetObjectItemCaseSensitive(root,"updated");
  const cJSON *errors=cJSON_GetObjectItemCaseSensitive(root,"errors");
  const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version");
  char date[11]={};
  const int incoming=cJSON_GetArraySize(events);
  bool ok=cJSON_IsArray(events) && incoming<=kMaxSportsEvents &&
      cJSON_IsNumber(version) && version->valuedouble==3 &&
      sportsString(root,"date",date,sizeof(date)) && std::strlen(date)==10 &&
      (!expectedDate || std::strcmp(expectedDate,date)==0) &&
      cJSON_IsNumber(updated) && updated->valuedouble>=1704067200 &&
      cJSON_IsNumber(errors) && errors->valuedouble==errors->valueint && errors->valueint>=0 && errors->valueint<15;
  int previousDay=0;
  if (ok) {
    for (const cJSON *item=events->child; item && ok; item=item->next) {
      SportsEvent e;
      ok=sportsReadEvent(item,e,previousDay);
      if (!ok) break;
      for(const cJSON *before=events->child;before!=item;before=before->next)
        if(std::strcmp(e.id,cJSON_GetObjectItemCaseSensitive(before,"id")->valuestring)==0)ok=false;
      previousDay=e.day;
    }
  }
  const bool retain=ok && result.valid && std::strcmp(result.date,date)==0 && errors->valueint;
  int retained=0;
  if(retain)for(int i=0;i<result.count;++i)
    if((errors->valueint&sportsProvider(result.events[i])) && !sportsContains(events,result.events[i]))++retained;
  if(!ok || incoming+retained>kMaxSportsEvents){cJSON_Delete(root);return false;}
  // Compact retained entries first; each destination precedes its source.
  int kept=0;
  if(retain) {
    for(int i=0;i<result.count;++i) {
      if(!(errors->valueint&sportsProvider(result.events[i])) || sportsContains(events,result.events[i]))continue;
      result.events[kept]=result.events[i];
      SportsEvent &cached=result.events[kept++];
      std::snprintf(cached.status,sizeof(cached.status),"%s",cached.live?"缓存比分":"缓存赛事");
    }
  }
  result.count=uint16_t(kept);
  for(const cJSON *item=events->child;item;item=item->next)sportsReadEvent(item,result.events[result.count++],0);
  // Rotate new entries in front of cached entries using three in-place reversals.
  const auto reverse=[&](int begin,int end){while(begin<--end){const SportsEvent moved=result.events[begin];result.events[begin++]=result.events[end];result.events[end]=moved;}};
  reverse(0,kept);reverse(kept,result.count);reverse(0,result.count);
  // Clean legacy snapshots and partial-source merges before pagination.
  int unique=0;
  for(int i=0;i<result.count;++i) {
    int duplicate=0;
    while(duplicate<unique && !sportsSameFixture(result.events[duplicate],result.events[i]))++duplicate;
    if(duplicate==unique)result.events[unique++]=result.events[i];
    else if(sportsFixtureQuality(result.events[i])>=sportsFixtureQuality(result.events[duplicate]))result.events[duplicate]=result.events[i];
  }
  result.count=uint16_t(unique);
  const auto before=[](const SportsEvent &a,const SportsEvent &b){
    if(a.day!=b.day)return a.day<b.day;
    return !std::strstr(a.meta," · 补充") && std::strstr(b.meta," · 补充");
  };
  for(int i=1;i<result.count;++i)if(before(result.events[i],result.events[i-1])) {
    const SportsEvent moved=result.events[i];int j=i;
    while(j>0 && before(moved,result.events[j-1])){result.events[j]=result.events[j-1];--j;}
    result.events[j]=moved;
  }
  result.updated=int64_t(updated->valuedouble);result.errors=uint8_t(errors->valueint);
  std::strcpy(result.date,date);result.valid=true;result.cached=false;cJSON_Delete(root);return true;
}
struct SportsGroup { uint16_t start=0; uint8_t count=0, column=0, day=0; };
struct SportsPage { uint8_t count=0; SportsGroup groups[4]; };
class SportsPages {
 public:
  uint8_t count=0;
  SportsPage pages[34];
  void build(const SportsData &data) {
    count=0;
    int today=0; while (today<data.count && data.events[today].day==0) ++today;
    int index=0;
    if (today) {
      const int first=today<8 ? today : 8, left=(first+1)/2;
      SportsPage &p=pages[count++]; p.count=0;
      for (int col=0; col<2 && index<first; ++col) {
        const int n=col==0 ? left : first-left;
        p.groups[p.count++]={uint16_t(index),uint8_t(n),uint8_t(col),0}; index+=n;
      }
    }
    while (index<data.count && count<34) {
      SportsPage &p=pages[count++]; p.count=0;
      const int pageDay=data.events[index].day;
      for (int col=0; col<2; ++col) {
        int remaining=191;
        // Each page belongs to one date: today's two pages, then tomorrow's page.
        while (index<data.count && data.events[index].day==pageDay) {
          const int capacity=(remaining-23)/42;
          if (capacity<=0) break;
          const int day=data.events[index].day, start=index;
          while (index<data.count && data.events[index].day==day && index-start<capacity) ++index;
          const int n=index-start;
          p.groups[p.count++]={uint16_t(start),uint8_t(n),uint8_t(col),uint8_t(day)};
          remaining-=23+n*42;
        }
      }
    }
    if (!count) { count=1; pages[0].count=0; }
  }
};
