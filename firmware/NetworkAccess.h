#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Keep the two TLS requests from competing for the ESP32's small internal heap.
class NetworkGuard {
  bool held_;
 public:
  explicit NetworkGuard(TickType_t wait=portMAX_DELAY);
  ~NetworkGuard();
  explicit operator bool() const { return held_; }
};
