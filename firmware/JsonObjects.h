#pragma once
#include <string>
#include <cctype>
#include "cJSON.h"

// NetworkGuard protects this workspace until consumers finish parsing it.
// Keep both readers' existing size limits, but allocate the larger buffer once.
inline char *jsonWorkspace(){static char bytes[16001];return bytes;}

// Read one array item at a time. Large public calendars never enter RAM as a
// complete JSON document; nested arrays remain inside their parent item.
class JsonObjects {
  using Callback=bool (*)(const cJSON *,void *);
  Callback callback_;void *context_;
  char prefix_[1025]={};
  size_t itemSize_=0,prefixSize_=0;
  int braces_=0,arrays_=0,start_=0;
  bool quoted_=false,escaped_=false,capturing_=false,ok_=true;
  size_t bytes_=0;
 public:
  JsonObjects(Callback callback,void *context):callback_(callback),context_(context) {}
  bool put(char c) {
    if(!ok_ || ++bytes_>3UL*1024*1024)return ok_=false;
    if(prefixSize_<1024 && !capturing_ && !std::isspace((unsigned char)c)){prefix_[prefixSize_++]=c;prefix_[prefixSize_]=0;}
    if(!quoted_ && !capturing_ && arrays_ && c=='{'){capturing_=true;start_=braces_;itemSize_=0;}
    if(capturing_){if(itemSize_>=8192)return ok_=false;buffer()[itemSize_++]=c;buffer()[itemSize_]=0;}
    if(quoted_) {
      if(escaped_)escaped_=false;
      else if(c=='\\')escaped_=true;
      else if(c=='"')quoted_=false;
    } else {
      if(c=='"')quoted_=true;
      else if(c=='{')++braces_;
      else if(c=='}')--braces_;
      else if(c=='[')++arrays_;
      else if(c==']')--arrays_;
    }
    if(braces_<0 || arrays_<0)return ok_=false;
    if(capturing_ && !quoted_ && braces_==start_) {
      cJSON *object=cJSON_Parse(buffer());
      ok_=object && callback_(object,context_);
      cJSON_Delete(object);capturing_=false;itemSize_=0;
    }
    return ok_;
  }
  bool complete() const {return ok_ && bytes_ && !quoted_ && !capturing_ && braces_==0 && arrays_==0;}
  bool finish(bool cs2=false) const {
    const char *token=cs2?"\"success\":true":"\"code\":0";
    return complete() && std::strstr(prefix_,token);
  }
 private:
  // NetworkGuard serializes all readers on the board. Reuse one bounded item
  // buffer instead of growing std::string during a TLS request.
  static char *buffer(){return jsonWorkspace();}
};

// Extract just the JSON assignment from the weather site's JavaScript/HTML.
class JsonAssignment {
  const char *needle_;
  size_t jsonSize_=0;
  size_t matched_=0;
  int depth_=0;
  bool found_=false,quoted_=false,escaped_=false,done_=false,ok_=true;
 public:
  explicit JsonAssignment(const char *needle):needle_(needle) { buffer()[0]='\0'; }
  bool put(char c) {
    if(done_)return ok_;
    if(!found_) {
      if(c==needle_[matched_])++matched_;
      else matched_=c==needle_[0]?1:0;
      if(matched_==std::strlen(needle_))found_=true;
      return true;
    }
    if(!depth_ && c!='{')return true;
    if(jsonSize_>=16000){done_=true;return ok_=false;}
    char *json=buffer();json[jsonSize_++]=c;json[jsonSize_]='\0';
    if(quoted_) {
      if(escaped_)escaped_=false;
      else if(c=='\\')escaped_=true;
      else if(c=='"')quoted_=false;
    } else {
      if(c=='"')quoted_=true;
      else if(c=='{')++depth_;
      else if(c=='}' && --depth_==0)done_=true;
    }
    return true;
  }
  bool complete() const { return ok_ && done_ && jsonSize_>0; }
  const char *json() const { return buffer(); }
 private:
  static char *buffer() { return jsonWorkspace(); }
};
