#pragma once
#include <stdint.h>
#include "CampusPortal.h"
void startCampusNetwork();
void resetCampusNetwork(); // Caller holds NetworkGuard during a profile switch.
uint32_t campusRetryRemaining(uint32_t now); // Caller holds NetworkGuard.
bool pollCampusNetwork(bool linked,uint32_t now);
bool campusAuthenticated();
bool campusClockReady();
const char *campusNetworkStatus();
const char *campusNetworkHint();
