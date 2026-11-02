#pragma once
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "ClockConfig.h"
#include "WeatherData.h"
#include "RainForecast.h"
// edison keeps the Nailong-era assets (font, frames, sayings); mcwave 6.1.0 has capybaras.
#ifdef CLOCK_NAILONG_ASSETS
#include "../EdisonAlbum/nailong/Fonts.h"
#else
#include "Fonts.h"
#endif
#include "FontLookup.h"
#include "PackedAssets.h"

class Display {
 public:
  uint8_t pixels[kWidth * kHeight / 8];
  void clear() { std::memset(pixels, 0xff, sizeof(pixels)); }
  void pixel(int x, int y) {
    if (x >= 0 && x < kWidth && y >= 0 && y < kHeight)
      pixels[y * (kWidth / 8) + x / 8] &= ~(0x80 >> (x % 8));
  }
  void rect(int x, int y, int w, int h) {
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) pixel(x + i, y + j);
  }
  void roundedRect(int x, int y, int w, int h, int radius, int stroke = 1) {
    auto inside = [](int px, int py, int rw, int rh, int r) {
      if (px < 0 || py < 0 || px >= rw || py >= rh) return false;
      int dx = px < r ? r-px : px >= rw-r ? px-(rw-r-1) : 0;
      int dy = py < r ? r-py : py >= rh-r ? py-(rh-r-1) : 0;
      return dx*dx + dy*dy <= r*r;
    };
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i)
        if (inside(i,j,w,h,radius) &&
            !inside(i-stroke,j-stroke,w-2*stroke,h-2*stroke,radius-stroke))
          pixel(x+i,y+j);
  }
  void circle(int x, int y, int radius) {
    for (int j = -radius; j <= radius; ++j)
      for (int i = -radius; i <= radius; ++i)
        if (i*i+j*j <= radius*radius) pixel(x+i,y+j);
  }
  void bitmap(int x, int y, int width, int height, const uint8_t *data) {
    const int stride = (width+7)/8;
    for (int j = 0; j < height; ++j)
      for (int i = 0; i < width; ++i)
        if (data[j*stride+i/8] & (0x80 >> (i%8))) pixel(x+i,y+j);
  }
  static uint32_t codepoint(const char *&s) {
    uint32_t cp = static_cast<unsigned char>(*s++);
    if (cp >= 0xc0 && cp < 0xe0 && s[0]) {
      cp = ((cp & 31) << 6) | (static_cast<unsigned char>(*s++) & 63);
    } else if (cp >= 0xe0 && cp < 0xf0 && s[0] && s[1]) {
      cp = ((cp & 15) << 12) | ((static_cast<unsigned char>(s[0]) & 63) << 6) |
           (static_cast<unsigned char>(s[1]) & 63);
      s += 2;
    }
    return cp;
  }
  static const Glyph *glyph(uint32_t cp, int size) {
    return findFontGlyph(kGlyphs,cp,size);
  }
  static int textWidth(const char *s, int size) {
    int width = 0;
    while (*s) {
      const Glyph *g = glyph(codepoint(s),size);
      width += g ? g->width : size;
    }
    return width;
  }
  void text(int x, int y, const char *s, int size = 13) {
    while (*s) {
      const Glyph *g = glyph(codepoint(s),size);
      if (!g) { x += size; continue; }
      bitmap(x,y,g->width,g->size,kFontBits+g->offset);
      x += g->width;
    }
  }
  void icon(int code, int x, int y) {
    if(code==-1) {
      // Missing condition is a question mark, never a fabricated sun/cloud.
      static constexpr uint8_t question[]={14,17,1,2,4,0,4};
      for(int row=0;row<7;++row)for(int col=0;col<5;++col)
        if(question[row]&(16>>col))rect(x+9+col*4,y+row*4,3,3);
      return;
    }
    if (code == 0 || code == 1) {
      circle(x+16,y+16,8);
      rect(x+15,y,2,5); rect(x+15,y+27,2,5);
      rect(x,y+15,5,2); rect(x+27,y+15,5,2);
      if (code == 0) return;
    }
    circle(x+10,y+18,8); circle(x+20,y+13,10); circle(x+30,y+19,7);
    rect(x+10,y+18,20,9);
    if (code >= 51) {
      for (int i = 0; i < 3; ++i) {
        rect(x+9+i*10,y+31,2,6);
        if ((code >= 71 && code <= 77) || code == 85 || code == 86)
          rect(x+7+i*10,y+33,6,2);
      }
    } else if (code == 45 || code == 48) {
      rect(x+5,y+31,30,1); rect(x+5,y+35,30,1);
    }
  }
  void render(const tm *local, const WeatherData &weather, bool connected,
              bool queryFailed, bool stale,
              const char *saying = "给你一点点勇气，慢一点也没关系。", uint8_t pose = 0,
              const char *networkStatus=nullptr,const char *networkHint=nullptr,
              const RainForecast *rain=nullptr,bool rainFailed=false) {
    clear();
    roundedRect(1,1,398,298,20);
    const char *status=networkStatus?networkStatus:connected ? "已连接" : "未连接";
    const int statusX=networkStatus?382-textWidth(status,11):329;
    circle(statusX-8,30,2);text(statusX,24,status,11);
    text(22,23,"南京 / 江宁区",14);
    if(const uint8_t *frame=nailongFrame(pose))bitmap(273,44,100,112,frame);
    char buf[96];
    if (local) {
      std::strftime(buf,sizeof(buf),"%H:%M",local);
      text(22,54,buf,72);
      const char *days[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
      std::snprintf(buf,sizeof(buf),"%04d-%02d-%02d · %s",
                    local->tm_year+1900,local->tm_mon+1,local->tm_mday,days[local->tm_wday]);
      text(22,144,buf,12);
    } else {
      text(22,54,"--:--",72);
      text(22,144,"等待网络校时",12);
    }
    roundedRect(20,174,360,64,16);
    if (weather.valid) {
      icon(weather.code,32,181);
      std::snprintf(buf,sizeof(buf),"%.1f°C",weather.temperature);
      const int temperatureSize = textWidth(buf,32) > 107 ? 26 : 32;
      text(82,temperatureSize == 32 ? 180 : 183,buf,temperatureSize);
      const char *condition = weatherLabel(weather.code);
      text(82,219,condition,13);
      const int divider=82+textWidth(condition,13)+8;
      if(rain) {
        rect(divider,220,1,12);
        const RainReadout forecast=rainReadout(*rain,local,weather.code,!connected || rainFailed);
        int x=divider+9;
        text(x,219,forecast.label,13);x+=textWidth(forecast.label,13);
        if(forecast.time[0]) {
          x+=4;text(x,219,forecast.time,13);x+=textWidth(forecast.time,13);
        }
        text(x+6,219,forecast.probability,13);
      } else {
        char trend[96];weatherTrend(weather,local,connected && !queryFailed && !stale,trend,sizeof(trend));
        text(divider+4,219,trend,13);
      }
      text(342-textWidth("湿度",13)/2,183,"湿度",13);
      std::snprintf(buf,sizeof(buf),"%d%%",weather.humidity);
      text(342-textWidth(buf,13)/2,201,buf,13);
      const char *label = !connected || queryFailed ? "缓存" :
                          stale ? "较旧数据" : "天气更新";
      if (local && (std::atoi(weather.dataTime+5) != local->tm_mon+1 ||
                    std::atoi(weather.dataTime+8) != local->tm_mday)) {
        std::snprintf(buf,sizeof(buf),"%s %.5s %.5s",label,weather.dataTime+5,weather.dataTime+11);
      } else {
        std::snprintf(buf,sizeof(buf),"%s %.5s",label,weather.dataTime+11);
      }
    } else {
      text(34,186,"--°C",32);
      text(145,198,connected ? "等待天气数据" : "等待网络连接",14);
      std::snprintf(buf,sizeof(buf),"%s",networkHint ? networkHint : !connected ? "请开启手机热点" :
                    !local ? "正在网络校时" : queryFailed ? "查询失败，稍后重试" : "正在查询天气");
    }
    text(24,279,buf,11);
    text(24,253,saying,13);
    std::snprintf(buf,sizeof(buf),"%s · 中央气象台/中国天气",kFirmwareVersion);
    text(382-textWidth(buf,11),279,buf,11);
  }
};
