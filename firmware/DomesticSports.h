#pragma once
#include <algorithm>
#include <string>
#include <deque>
#include <ctime>
#include "SportsData.h"
#include "SportsJson.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

inline const char *cnField(const cJSON *object,const char *key) {
  const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
  return cJSON_IsString(value)?value->valuestring:"";
}
inline bool cnMember(const char *name,std::initializer_list<const char *> names) {
  for(const char *candidate:names)if(std::strcmp(name,candidate)==0)return true;
  return false;
}
inline bool cnFavorite(const char *name){return cnMember(name,{"切尔西","皇家马德里","皇马"});}
inline bool cnBigClub(const char *name){return cnMember(name,{"切尔西","曼城","阿森纳","利物浦","曼联","热刺","托特纳姆热刺"});}
inline bool cnNation(const char *name){return cnMember(name,{"巴西","阿根廷","法国","西班牙","英格兰","德国","葡萄牙","荷兰","意大利","克罗地亚","比利时"});}
inline bool cnHotNba(const char *name){return cnMember(name,{"凯尔特人","湖人","勇士","掘金","雄鹿","太阳","独行侠","雷霆","快船"});}
inline bool cnSupplementClub(const char *name){return cnBigClub(name)||cnMember(name,{"巴塞罗那","巴萨","拜仁慕尼黑","拜仁","多特蒙德","巴黎圣日耳曼","巴黎","国际米兰","AC米兰","尤文图斯","马德里竞技","马竞"});}
inline bool cnLeague(const char *description,const char *league) {
  // 巴西甲 is a Brazilian league, not 西甲. Provider labels start with the league.
  return std::strncmp(description,league,std::strlen(league))==0;
}
inline int cnSupplementFootball(const char *description) {
  for(const char *league:{"英超","西甲","德甲","意甲","法甲","欧冠","世界杯","世预赛","欧洲杯","欧国联","亚洲杯","美洲杯","非洲杯"})
    if(cnLeague(description,league))return 2;
  // Last resort across professional cups/leagues; no youth/amateur/replays.
  for(const char *league:{"亚冠","欧联","欧协联","足总杯","英联杯","国王杯","德国杯","意大利杯","法国杯","英冠","葡超","荷甲","土超","巴西甲","阿甲","中超","沙特联","日职联","韩K联","美职联","国家/地区友谊赛"})
    if(cnLeague(description,league))return 3;
  return -1;
}
inline const char *cnCsName(const char *name) {
  if(std::strcmp(name,"Natus Vincere")==0)return "NAVI";
  if(std::strcmp(name,"BB")==0 || std::strcmp(name,"BetBoom Team")==0)return "BetBoom";
  if(std::strcmp(name,"Team Spirit")==0)return "Spirit";
  if(std::strcmp(name,"Team Vitality")==0)return "Vitality";
  if(std::strcmp(name,"FaZe Clan")==0)return "FaZe";
  if(std::strcmp(name,"Ninjas in Pyjamas")==0)return "NIP";
  return name;
}
inline bool cnHotCs(const char *name){return cnMember(cnCsName(name),{"NAVI","G2","Vitality","Spirit","FaZe","MOUZ","FURIA","Falcons","Liquid","Astralis","Virtus.pro","The MongolZ","NIP","Complexity","Heroic","BIG","ENCE","BetBoom","GamerLegion","fnatic","Fnatic","Aurora","Legacy"});}
// 0: original focus; 1/2: B supplements; 3: extra professional events.
struct DomesticEvent { SportsEvent event; uint8_t priority=0; };
class DomesticSports {
  char date_[11],tomorrow_[11];
  // Small deque blocks avoid a second large contiguous allocation during TLS.
  std::deque<DomesticEvent> rows_;
  bool overflow_=false;
  static bool before(const DomesticEvent &a,const DomesticEvent &b) {
    if(a.event.day!=b.event.day)return a.event.day<b.event.day;
    if(a.priority!=b.priority)return a.priority<b.priority;
    const auto stage=[](const SportsEvent &e){return e.live?0:std::strcmp(e.status,"未开始")==0?1:2;};
    if(stage(a.event)!=stage(b.event))return stage(a.event)<stage(b.event);
    const int time=std::strcmp(a.event.start,b.event.start);
    return time?time<0:std::strcmp(a.event.id,b.event.id)<0;
  }
  void select() {
    std::sort(rows_.begin(),rows_.end(),before);
    int focus[2]={},extra[2]={};
    for(const auto &row:rows_)if(!row.priority)++focus[row.event.day];
    // Keep only the best needed supplements while streaming, never a second feed.
    const int target[2]={16,8};
    rows_.erase(std::remove_if(rows_.begin(),rows_.end(),[&](const DomesticEvent &row){
      return row.priority && focus[row.event.day]+(++extra[row.event.day])>target[row.event.day];
    }),rows_.end());
  }
  bool add(DomesticEvent row,const char *stamp) {
    if(std::strlen(stamp)!=19 || stamp[10]!=' ')return false;
    const int day=std::strncmp(stamp,date_,10)==0?0:std::strncmp(stamp,tomorrow_,10)==0?1:-1;
    if(day<0)return true;
    row.event.day=uint8_t(day);
    std::strcpy(row.event.start,stamp);
    for(auto &old:rows_) {
      if(sportsSameFixture(old.event,row.event)) {
        const uint8_t priority=std::min(old.priority,row.priority);
        if(sportsFixtureQuality(row.event)>=sportsFixtureQuality(old.event)) {
          old.event=row.event;
        }
        old.priority=priority;select();
        return true;
      }
    }
    if(rows_.size()>=kMaxSportsEvents) {
      if(row.priority)return true;
      auto extra=std::find_if(rows_.rbegin(),rows_.rend(),[](const DomesticEvent &e){return e.priority!=0;});
      if(extra==rows_.rend()){overflow_=true;return false;}
      *extra=row;
    } else {
#ifdef ESP_PLATFORM
      if(heap_caps_get_free_size(MALLOC_CAP_8BIT)<4096 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<2048) {
        std::puts("[sports] selection heap low; keeping previous feed");overflow_=true;return false;
      }
#endif
      rows_.push_back(row);
    }
    select();return true;
  }
 public:
  DomesticSports(const char *date,const tm &local) {
    std::snprintf(date_,sizeof(date_),"%s",date);tm tomorrow=local;++tomorrow.tm_mday;std::mktime(&tomorrow);
    std::strftime(tomorrow_,sizeof(tomorrow_),"%Y-%m-%d",&tomorrow);
  }
  const char *tomorrow() const{return tomorrow_;}
  size_t count() const{return rows_.size();}
  bool overflow() const{return overflow_;}
  bool tencent(const cJSON *item,int source) {
    const char *left=cnField(item,"leftName"),*right=cnField(item,"rightName"),*description=cnField(item,"matchDesc");
    const char *type=cnField(item,"matchType"),*id=cnField(item,"mid"),*title=cnField(item,"title");
    if(!*id)return false;
    uint8_t priority=0;
    if(source==1) {
      if(std::strcmp(type,"1")!=0 || !*left || !*right)return true;
      const bool premier=std::strstr(description,"英超");
      const bool knockout=std::strstr(description,"半决赛") || std::strstr(description,"决赛");
      const bool focus=cnFavorite(left)||cnFavorite(right)||(premier&&cnBigClub(left)&&cnBigClub(right))||(cnNation(left)&&cnNation(right))||(knockout&&(cnNation(left)||cnNation(right)));
      if(!focus) {
        const int tier=cnSupplementFootball(description);if(tier<0)return true;
        for(const char *age:{"U16","U17","U18","U19","U20","U21","U23"})
          if(std::strstr(left,age)||std::strstr(right,age)||std::strstr(description,age))return true;
        if(std::strstr(description,"青年")||std::strstr(description,"后备")||std::strstr(description,"业余"))return true;
        priority=tier==2&&(cnSupplementClub(left)||cnSupplementClub(right)||cnNation(left)||cnNation(right))?1:uint8_t(tier);
      }
    } else if(source==2) {
      if(std::strcmp(cnField(item,"competitionId"),"100000")!=0 || !*left || !*right)return true; // Exclude replay programs.
      const bool playoff=std::strstr(description,"季后赛")||std::strstr(description,"总决赛");
      if(!(cnMember(left,{"凯尔特人"})||cnMember(right,{"凯尔特人"})||(cnHotNba(left)&&cnHotNba(right))||playoff))priority=cnHotNba(left)||cnHotNba(right)?1:2;
    } else {
      if(std::strcmp(cnField(item,"competitionId"),"100360")!=0 || !std::strstr(title,"正赛") || std::strstr(title,"回顾") || std::strstr(title,"录像"))return true;
    }
    DomesticEvent row;SportsEvent &e=row.event;
    row.priority=priority;
    std::snprintf(e.id,sizeof(e.id),"%s-%s",source==1?"fb":source==2?"nba":"f1",id);
    std::snprintf(e.title,sizeof(e.title),source==4?"%s":"%s vs %s",source==4?title:left,right);
    const char *period=cnField(item,"matchPeriod");
    const bool started=std::strcmp(period,"0")!=0 && *period;
    const bool ended=std::strcmp(period,"2")==0;
    e.live=started&&!ended;
    const char *stamp=cnField(item,"startTime");
    if(!started)std::snprintf(e.value,sizeof(e.value),"%.5s",std::strlen(stamp)>=16?stamp+11:"--:--");
    else if(source==4)std::snprintf(e.value,sizeof(e.value),"%s",std::strstr(cnField(item,"quarter"),"圈")?cnField(item,"quarter"):"赛况待更新");
    else {
      const char *a=cnField(item,"leftGoal"),*b=cnField(item,"rightGoal");
      std::snprintf(e.value,sizeof(e.value),*a&&*b?"%s : %s":"比分待更新",a,b);
    }
    std::snprintf(e.status,sizeof(e.status),"%s",!started?"未开始":ended?"已结束":source==2&&*cnField(item,"quarter")?cnField(item,"quarter"):"进行中");
    const char *league="足球";
    for(const char *label:{"英超","西甲","德甲","意甲","法甲","欧冠","亚冠","欧联","欧协联","足总杯","英联杯","国王杯","英冠","葡超","荷甲","土超","巴西甲","阿甲","中超","日职联","韩K联","美职联"})
      if(cnLeague(description,label)){league=label;break;}
    std::snprintf(e.meta,sizeof(e.meta),"%s",source==2?"NBA":source==4?"F1 · 正赛":cnNation(left)||cnNation(right)?"国家队":league);
    return add(row,stamp);
  }
  bool cs2(const cJSON *item) {
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(item,"t1_info"),*b=cJSON_GetObjectItemCaseSensitive(item,"t2_info"),*t=cJSON_GetObjectItemCaseSensitive(item,"tt_info");
    const char *left=cnCsName(cnField(a,"disp_name")),*right=cnCsName(cnField(b,"disp_name"));
    const char *grade=cnField(t,"grade_label");
    if(!*cnField(item,"id"))return false;
    const bool top=std::strstr(grade,"S")||std::strstr(grade,"A级");
    const bool hot=cnHotCs(left)||cnHotCs(right);
    if(!top && !(hot && std::strstr(grade,"B级")))return true;
    if(!*left || !*right || std::strcmp(left,"TBD")==0 || std::strcmp(right,"TBD")==0)return true;
    DomesticEvent row;SportsEvent &e=row.event;
    row.priority=top?(hot?0:2):3;
    std::snprintf(e.id,sizeof(e.id),"cs-%s",cnField(item,"id"));
    std::snprintf(e.title,sizeof(e.title),"%s vs %s",left,right);
    const char *status=cnField(item,"status"),*stamp=cnField(item,"plan_ts");
    const bool started=std::strcmp(status,"1")==0 || std::strcmp(status,"2")==0;
    e.live=std::strcmp(status,"1")==0;
    if(!started)std::snprintf(e.value,sizeof(e.value),"%.5s",std::strlen(stamp)>=16?stamp+11:"--:--");
    else {const char *x=cnField(item,"t1_score"),*y=cnField(item,"t2_score");std::snprintf(e.value,sizeof(e.value),*x&&*y?"大比分 %s:%s":"比分待更新",x,y);}
    std::snprintf(e.status,sizeof(e.status),"%s",!started?"未开始":e.live?"进行中":"已结束");
    std::snprintf(e.meta,sizeof(e.meta),"CS2 · BO%s",cnField(item,"format"));
    return add(row,stamp);
  }
  std::string json(int64_t updated,int errors) {
    select();
#ifdef ESP_PLATFORM
    int count[2]={},focus[2]={};
    for(const auto &row:rows_){++count[row.event.day];if(!row.priority)++focus[row.event.day];}
    std::printf("[sports] selected today=%d focus=%d tomorrow=%d focus=%d\n",count[0],focus[0],count[1],focus[1]);
#endif
    const auto write=[&](SportsJsonWriter &out) {
      out.text("{\"version\":3,\"date\":");out.string(date_);
      out.text(",\"updated\":");out.number(updated);out.text(",\"errors\":");out.number(errors);
      out.text(",\"events\":[");bool first=true;
      for(const auto &row:rows_) {
        const SportsEvent &e=row.event;if(!first)out.byte(',');first=false;
        out.text("{\"id\":");out.string(e.id);out.text(",\"day\":");out.number(e.day);
        out.text(",\"live\":");out.text(e.live?"true":"false");
        out.text(",\"title\":");out.string(e.title);out.text(",\"value\":");out.string(e.value);
        out.text(",\"status\":");out.string(e.status);
        char meta[sizeof(e.meta)];std::snprintf(meta,sizeof(meta),row.priority?"%s · 补充":"%s",e.meta);
        out.text(",\"meta\":");out.string(meta);out.text(",\"start\":");out.string(e.start);out.byte('}');
      }
      out.text("]}");
    };
    SportsJsonWriter measured;write(measured);
#ifdef ESP_PLATFORM
    if(!sportsJsonFitsHeap(measured.size(),heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))) {
      std::puts("[sports] JSON heap low; keeping previous feed");return {};
    }
#endif
    std::string body(measured.size(),'\0');SportsJsonWriter out(body.data());write(out);
    return body;
  }
};
