#pragma once
#include "ScreenMirrorPolicy.h"
#include <mbedtls/sha256.h>
inline void screenFrameHash(const uint8_t *frame,char (&out)[17]){
  uint8_t digest[32];mbedtls_sha256(frame,15000,digest,0);
  static const char hex[]="0123456789abcdef";
  for(unsigned i=0;i<8;++i){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}out[16]=0;
}
