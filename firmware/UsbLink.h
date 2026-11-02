#pragma once
#include <stdint.h>

// mcwave 7.0.0: when the Windows gateway is attached over USB, it provides the clock and the
// network. The firmware keeps doing its own TLS: each HTTPS connection goes to a loopback relay
// on the ESP32 itself, which tunnels the bytes over the serial link (see UsbFrame.h).
constexpr uint16_t kUsbRelayPort=49321;
void startUsbLink();
bool usbLinkActive();       // the gateway has spoken within the last 15 seconds
bool usbLinkClockReady();   // the gateway has set the clock (stays true after it is unplugged)
// Names the next loopback connection's real destination. False when no gateway is attached.
bool usbLinkPrepareConnect(const char *host,uint16_t port);
