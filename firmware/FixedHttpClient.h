#pragma once
#include <HTTPClient.h>
#include "FixedHttpBody.h"

// Keep the installed HTTPClient's request/header/TLS behavior. Only replace
// its allocating writeToStream body loop. NetworkGuard protects the buffer.
class FixedHttpClient:public HTTPClient {
 public:
  int writeToStream(Stream *stream) {
    if(!stream)return returnError(HTTPC_ERROR_NO_STREAM);
    if(!connected())return returnError(HTTPC_ERROR_NOT_CONNECTED);
    if(_transferEncoding!=HTTPC_TE_IDENTITY && _transferEncoding!=HTTPC_TE_CHUNKED)
      return returnError(HTTPC_ERROR_ENCODING);
    struct Clock {
      uint32_t now(){return millis();}
      void pause(){delay(1);}
    } clock;
    // At most two minutes per body (the largest, rain probability ~715KB, takes seconds normally).
    FixedHttpBody<NetworkClient,Stream,Clock> body(*_client,*stream,clock,_tcpTimeout,120000);
    const int result=body.read(_size,_transferEncoding==HTTPC_TE_CHUNKED);
    if(result<0)return returnError(result);
    if(_transferEncoding==HTTPC_TE_CHUNKED && _size<=0)_size=result;
    disconnect(true);return result;
  }
};
