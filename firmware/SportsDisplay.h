#pragma once
#include "Display.h"
#include "SportsData.h"
#include "SportsRefresh.h"
#include "PackedAssets.h"

class SportsDisplay {
  Display &screen;
 public:
  explicit SportsDisplay(Display &display):screen(display) {}
  static int width(const char *text,int size) {
    int w=0;while(*text){SportsGlyphInfo g;w+=findSportsGlyph(Display::codepoint(text),size,g)?g.width:size;}return w;
  }
  void text(int x,int y,const char *value,int size,int limit=400) {
    const bool truncate=width(value,size)>limit-x;
    while(*value) {
      const char *next=value;const uint32_t cp=Display::codepoint(next);
      SportsGlyphInfo g;
      if(!findSportsGlyph(cp,size,g)&&!findSportsGlyph('?',size,g))break;
      if(x+g.width>limit || (truncate && *next && x+g.width+width("...",size)>limit)){
        if(x+width("...",size)<=limit)text(x,y,"...",size,400);break;
      }
      // Bits come from an inflated block; a block that fails its CRC is skipped.
      if(const uint8_t *bits=sportsGlyphBits(g))screen.bitmap(x,y,g.width,g.size,bits);
      x+=g.width;value=next;
    }
  }
  void right(int edge,int y,const char *value,int size){text(edge-width(value,size),y,value,size,edge);}
  void render(const tm *local,const SportsData &data,const SportsPages &pages,uint8_t page,bool connected,bool failed,const char *networkStatus=nullptr,const char *footer="短按切页 · 长按翻页") {
    screen.clear();screen.roundedRect(1,1,398,298,18);
    text(18,16,"赛事速览",19);
    char today[11]={};if(local)std::strftime(today,sizeof(today),"%Y-%m-%d",local);
    const bool expired=local&&data.valid&&std::strcmp(today,data.date)!=0;
    tm storedDate={};int year=0,month=0,day=0;
    char dateStamp[17];std::snprintf(dateStamp,sizeof(dateStamp),"%sT00:00",data.date);int64_t storedMinute=0;
    const bool hasStoredDate=data.valid && weatherMinute(dateStamp,storedMinute) && std::sscanf(data.date,"%d-%d-%d",&year,&month,&day)==3;
    if(hasStoredDate){storedDate.tm_year=year-1900;storedDate.tm_mon=month-1;storedDate.tm_mday=day;}
    const tm *groupDate=local&&!expired?local:hasStoredDate?&storedDate:nullptr;
    tm current=local?*local:tm{};
    const bool delayed=local && data.valid && std::mktime(&current)-data.updated>=sportsRefreshInterval(data)/1000+60;
    const bool cached=data.cached || delayed || expired || !local;
    const char *coverage=data.errors==1?"足球未更新":data.errors==2?"NBA未更新":data.errors==4?"F1未更新":data.errors==8?"CS2未更新":data.errors?"部分未更新":"已连接";
    right(382,23,networkStatus?networkStatus:!connected?"离线缓存":failed?"查询失败":data.errors?coverage:expired?"历史缓存":data.cached?"缓存待更新":delayed?"更新延迟":coverage,11);
    text(18,45,!local&&data.valid?"缓存赛事 · 等待校时":expired?"历史缓存 · 北京时间":"北京时间",10);
    char buf[100];
    tm updated={};const time_t stamp=time_t(data.updated);if(data.valid)localtime_r(&stamp,&updated);
    std::snprintf(buf,sizeof(buf),"%s %02d:%02d",!connected||failed||cached?"缓存":"更新",updated.tm_hour,updated.tm_min);
    right(382,45,data.valid?buf:"等待赛事数据",10);
    screen.rect(18,62,364,1);
    if(groupDate && data.valid && data.count) {
      screen.rect(200,76,1,191);
      int columnY[]={76,76},totals[]={0,0};for(int i=0;i<data.count;++i)++totals[data.events[i].day];
      const SportsPage &p=pages.pages[page%pages.count];
      for(int n=0;n<p.count;++n){const SportsGroup &g=p.groups[n];const int x=18+192*g.column,edge=x+172;
        int &y=columnY[g.column];const bool continued=g.start&&data.events[g.start-1].day==g.day;
        const char *group=(!local||expired)?(g.day?(continued?"缓存次日 · 续":"缓存次日"):(continued?"缓存当日 · 续":"缓存当日")):
                          g.day?(continued?"明日 · 续":"明日赛事"):(continued?"今日 · 续":"今日赛事");
        text(x,y,group,13,edge);
        tm date=*groupDate;date.tm_mday+=g.day;std::mktime(&date);
        std::snprintf(buf,sizeof(buf),"%02d/%02d · %d场",date.tm_mon+1,date.tm_mday,totals[g.day]);right(edge,y+3,buf,9);y+=23;
        for(int i=0;i<g.count;++i){const SportsEvent &e=data.events[g.start+i];
          int size=12;while(width(e.title,size)>172&&size>10)--size;text(x,y,e.title,size,edge);
          bool chinese=false;for(const unsigned char *s=(const unsigned char*)e.value;*s;++s)if(*s>=128)chinese=true;
          const int valueSize=chinese?12:17;const int valueWidth=width(e.value,valueSize);
          text(x,y+14,e.value,valueSize,edge);
          char status[48];std::snprintf(status,sizeof(status),"%s",(!connected||failed||cached)&&e.live?"缓存比分":e.status);
          const int statusLeft=edge-width(status,9);
          text(statusLeft>x+valueWidth+4?statusLeft:x+valueWidth+5,y+18,status,9,edge);
          text(x,y+32,e.meta,9,edge);screen.rect(x,y+41,172,1);y+=42;
        }
      }
    } else {
      text(18,76,"今日／明日赛事",13);
      const char *message=!local?"等待网络校时":expired?"缓存已过期，等待更新":!data.valid?(failed?"赛事查询失败":"正在获取赛事"):data.errors?"部分赛事暂时无法获取":"今日／明日暂无可用赛事";
      text(80,158,message,12,382);
      if(!connected)text(105,183,"请开启手机热点",11,382);
      else if(failed||data.errors)text(94,183,"稍后重试，保留已有数据",11,382);
    }
    screen.rect(18,277,364,1);text(18,283,footer,10);
    std::snprintf(buf,sizeof(buf),"%d / %d",page+1,pages.count);text(232,283,buf,11);
    right(382,283,kFirmwareVersion,10);
  }
};
