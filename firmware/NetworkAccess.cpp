#include "NetworkAccess.h"
#include "TlsRecordPool.h"
static SemaphoreHandle_t networkMutex() {
  static SemaphoreHandle_t mutex=xSemaphoreCreateMutex();
  return mutex;
}
NetworkGuard::NetworkGuard(TickType_t wait):held_(networkMutex() && xSemaphoreTake(networkMutex(),wait)==pdTRUE) {}
NetworkGuard::~NetworkGuard() {
  if(held_) {
#if defined(ARDUINO_ARCH_ESP32)
    reportTlsMemory();
#endif
    xSemaphoreGive(networkMutex());
  }
}
