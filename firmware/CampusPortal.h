#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include "cJSON.h"

// Protocol reference: https://github.com/NN708/seu-wlan-login (MIT).
// The application timezone is fixed to Beijing (CST-8).
inline time_t portalHttpTime(const char *value) {
  char weekday[4]={},month[4]={},zone[4]={};int day=0,year=0,hour=-1,minute=-1,second=-1,used=0;
  if(std::sscanf(value,"%3s, %d %3s %d %d:%d:%d %3s%n",weekday,&day,month,&year,&hour,&minute,&second,zone,&used)!=8 ||
     value[used] || std::strcmp(zone,"GMT") || year<2024 || year>2099 || day<1 || day>31 ||
     hour<0 || hour>23 || minute<0 || minute>59 || second<0 || second>59)return 0;
  static const char names[]="JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *found=std::strstr(names,month);if(!found||(found-names)%3)return 0;
  tm date={};date.tm_year=year-1900;date.tm_mon=int(found-names)/3;date.tm_mday=day;date.tm_hour=hour;date.tm_min=minute;date.tm_sec=second;
  const int originalMonth=date.tm_mon;
  const time_t stamp=std::mktime(&date);
  if(stamp==time_t(-1)||date.tm_year!=year-1900||date.tm_mon!=originalMonth||date.tm_mday!=day)return 0;
  return stamp+8*3600;
}

enum class CampusState { Connecting, MissingCredentials, Checking, Authenticating, Rejected, Failed, Online };
inline const char *campusStatus(CampusState state) {
  switch(state) {
    case CampusState::Connecting:return "连接校园网";
    case CampusState::MissingCredentials:return "请填账号";
    case CampusState::Checking:return "等待认证";
    case CampusState::Authenticating:return "正在认证";
    case CampusState::Rejected:return "认证失败";
    case CampusState::Failed:return "认证重试";
    default:return "已认证";
  }
}
inline const char *campusHint(CampusState state) {
  switch(state) {
    case CampusState::Connecting:return "等待校园网连接";
    case CampusState::MissingCredentials:return "请填写校园网账号密码";
    case CampusState::Rejected:return "请检查账号密码或设备限制";
    case CampusState::Failed:return "校园网认证失败，稍后重试";
    default:return "正在校园网认证";
  }
}
inline std::string portalEncode(const char *value) {
  static const char hex[]="0123456789ABCDEF";std::string encoded;
  for(const unsigned char *p=reinterpret_cast<const unsigned char *>(value);*p;++p) {
    const unsigned char c=*p;
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')encoded+=char(c);
    else {encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}
  }
  return encoded;
}
inline bool portalIp(const char *ip) {
  if(!ip||!*ip)return false;
  for(int part=0;part<4;++part) {
    int value=0,digits=0;while(*ip>='0'&&*ip<='9'){value=value*10+(*ip++-'0');if(++digits>3||value>255)return false;}
    if(!digits || (part<3?*ip++!='.':*ip!=0))return false;
  }
  return true;
}
// Never log a portal-controlled message verbatim: it may echo credentials.
inline const char *portalMessageKind(const cJSON *msg) {
  if(!msg)return "absent";
  if(!cJSON_IsString(msg))return "nontext_redacted";
  std::string text;
  for(size_t i=0;msg->valuestring[i]&&i<192;++i){
    const unsigned char c=static_cast<unsigned char>(msg->valuestring[i]);
    text+=char(c>='A'&&c<='Z'?c+('a'-'A'):c);
  }
  if(text.find("password")!=std::string::npos||text.find("密码")!=std::string::npos||text.find("用户名")!=std::string::npos)return "credential_hint";
  if(text.find("limit")!=std::string::npos||text.find("online")!=std::string::npos||text.find("终端")!=std::string::npos||text.find("设备")!=std::string::npos||text.find("在线")!=std::string::npos)return "device_or_online_hint";
  if(text.find("disabled")!=std::string::npos||text.find("禁用")!=std::string::npos||text.find("停用")!=std::string::npos)return "account_state_hint";
  return text.empty()?"empty":"other_redacted";
}
struct PortalReply {int result=-1;std::string ip;const char *message="absent";};
inline bool parsePortal(const std::string &body,const char *callback,PortalReply &reply) {
  const size_t begin=body.find('{'),end=body.rfind('}');
  if(body.size()>4096||begin==std::string::npos||end==std::string::npos||begin>end)return false;
  std::string prefix=body.substr(0,begin),suffix=body.substr(end+1);
  auto clean=[](std::string &s){s.erase(0,s.find_first_not_of(" \r\n\t"));const size_t last=s.find_last_not_of(" \r\n\t");if(last!=std::string::npos)s.erase(last+1);};
  clean(prefix);clean(suffix);
  if(prefix!=std::string(callback)+"(" || (suffix!=")"&&suffix!=");"))return false;
  cJSON *root=cJSON_Parse(body.substr(begin,end-begin+1).c_str());if(!root)return false;
  const cJSON *result=cJSON_GetObjectItemCaseSensitive(root,"result"),*ip=cJSON_GetObjectItemCaseSensitive(root,"v46ip");
  PortalReply next;
  if(cJSON_IsNumber(result)&&result->valuedouble==result->valueint&&(result->valueint==0||result->valueint==1))next.result=result->valueint;
  else if(cJSON_IsString(result)&&(std::strcmp(result->valuestring,"0")==0||std::strcmp(result->valuestring,"1")==0))next.result=result->valuestring[0]-'0';
  if(cJSON_IsString(ip))next.ip=ip->valuestring;
  next.message=portalMessageKind(cJSON_GetObjectItemCaseSensitive(root,"msg"));
  cJSON_Delete(root);if(next.result<0)return false;reply=next;return true;
}

class CampusSession {
  bool attempted_=false,linked_=false;
  uint32_t checked_=0,verified_=0;
  uint8_t checkFailures_=0;
  bool rejected_=false;
  uint32_t rejectedAt_=0;
 public:
  CampusState state=CampusState::Connecting;
  uint32_t loginReports=0;
  int loginResult=-1;
  const char *loginMessage="absent";
  uint32_t retryRemaining(uint32_t now)const {
    const uint32_t age=uint32_t(now-rejectedAt_);
    return rejected_&&age<300000?300000-age:0;
  }
  void resetLink(){linked_=false;attempted_=false;checkFailures_=0;state=CampusState::Connecting;}
  bool online()const{return state==CampusState::Online;}
  // query returns false on HTTP/TLS/size errors. A rejected login uses a longer backoff.
  template<class Query> bool poll(bool linked,uint32_t now,const char *username,const char *password,Query query,bool njit=false) {
    const CampusState before=state;
    if(!linked){resetLink();return state!=before;}
    if(retryRemaining(now)){state=CampusState::Rejected;return state!=before;}
    rejected_=false;
    if(!linked_){linked_=true;attempted_=false;state=CampusState::Checking;}
    const uint32_t interval=state==CampusState::Online&&!checkFailures_?60000:state==CampusState::Rejected?300000:30000;
    if(attempted_&&uint32_t(now-checked_)<interval)return state!=before;
    attempted_=true;checked_=now;
    state=CampusState::Checking;std::string body;PortalReply status;
    const char *statusUrl=njit?"http://172.31.255.156/drcom/chkstatus?callback=dr1002":"https://w.seu.edu.cn/drcom/chkstatus?callback=dr1002";
    if(!query(statusUrl,body)||!parsePortal(body,"dr1002",status)) {
      // A failed HTTPS check does not prove the existing authenticated session
      // disappeared. Retry in 30s; expire the grace after 3 failures or 3min.
      if(checkFailures_<255)++checkFailures_;
      state=before==CampusState::Online&&checkFailures_<3&&uint32_t(now-verified_)<180000?CampusState::Online:CampusState::Failed;
      return state!=before;
    }
    checkFailures_=0;
    if(status.result==1){state=CampusState::Online;verified_=now;return state!=before;}
    if(!*username||!*password){state=CampusState::MissingCredentials;return state!=before;}
    if(!portalIp(status.ip.c_str())){state=CampusState::Failed;return state!=before;}
    state=CampusState::Authenticating;
    // NJIT's live a44.js uses portal/login (method 1, account_prefix=1).
    const std::string url=std::string(njit?"http://172.31.255.156:801/eportal/portal/login?callback=dr1003&login_method=1&user_account=%2C0%2C":"https://w.seu.edu.cn:801/eportal/?c=Portal&a=login&callback=dr1003&login_method=1&user_account=%2C0%2C")+
                          portalEncode(username)+"&user_password="+portalEncode(password)+"&wlan_user_ip="+portalEncode(status.ip.c_str());
    PortalReply login;
    if(!query(url.c_str(),body)||!parsePortal(body,"dr1003",login)){state=CampusState::Failed;return state!=before;}
    ++loginReports;loginResult=login.result;loginMessage=login.message;
    if(login.result!=1){rejected_=true;rejectedAt_=now;state=CampusState::Rejected;return state!=before;}
    rejected_=false;
    // A login HTTP 200 or success message alone is not proof of an active session.
    if(!query(statusUrl,body)||!parsePortal(body,"dr1002",status)||status.result!=1)
      state=CampusState::Failed;
    else {state=CampusState::Online;verified_=now;}
    return state!=before;
  }
};
