#include "TlsRecordPool.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

static TlsRecordPool records;
static portMUX_TYPE poolMux=portMUX_INITIALIZER_UNLOCKED;
static uint32_t pooled=0,fallback=0,failures=0;
static size_t failedBytes=0;
extern "C" void *__real_esp_mbedtls_mem_calloc(size_t,size_t);
extern "C" void __real_esp_mbedtls_mem_free(void *);
extern "C" void *__wrap_esp_mbedtls_mem_calloc(size_t count,size_t size) {
  if(size&&count>std::numeric_limits<size_t>::max()/size)return nullptr;
  portENTER_CRITICAL(&poolMux);
  void *ptr=records.claim(count,size);
  if(ptr)++pooled;else ++fallback;
  portEXIT_CRITICAL(&poolMux);
  if(ptr)return ptr;
  ptr=__real_esp_mbedtls_mem_calloc(count,size);
  if(!ptr&&count&&size){portENTER_CRITICAL(&poolMux);++failures;failedBytes=count*size;portEXIT_CRITICAL(&poolMux);}
  return ptr;
}
extern "C" void __wrap_esp_mbedtls_mem_free(void *ptr) {
  if(!ptr)return;
  portENTER_CRITICAL(&poolMux);const bool ours=records.release(ptr);portEXIT_CRITICAL(&poolMux);
  if(!ours)__real_esp_mbedtls_mem_free(ptr);
}
void reportTlsMemory() {
  static uint32_t reportedMs=0,reportedFailures=0;
  portENTER_CRITICAL(&poolMux);const uint32_t p=pooled,f=fallback,failed=failures;const size_t bytes=failedBytes;portEXIT_CRITICAL(&poolMux);
  if(!p&&!failed)return;
  if(reportedMs&&uint32_t(millis()-reportedMs)<60000&&failed==reportedFailures)return;
  reportedMs=millis();reportedFailures=failed;
  Serial.printf("[tls] pooled=%lu fallback=%lu alloc_failures=%lu failed_bytes=%u byte_heap=%u largest=%u\n",p,f,failed,unsigned(bytes),heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
#endif
