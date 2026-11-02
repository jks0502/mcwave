#ifdef ESP_PLATFORM
#include "lwip/tcpip.h"

#if !LWIP_TCPIP_CORE_LOCKING
#error "The DNS cache workaround requires lwIP TCPIP core locking."
#endif

// Arduino 3.3.10-cn NetworkManager clears DNS on an IP-state change without
// the core lock. Clearing pending DNS can invoke SNTP callbacks that use raw
// UDP APIs. Wrap only the clear operation, never the blocking DNS lookup.
extern "C" void __real_dns_clear_cache();
extern "C" void __wrap_dns_clear_cache() {
  if(sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {
    __real_dns_clear_cache();return;
  }
  LOCK_TCPIP_CORE();
  __real_dns_clear_cache();
  UNLOCK_TCPIP_CORE();
}
#endif
