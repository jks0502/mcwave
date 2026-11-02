#pragma once
#ifdef ESP_PLATFORM
void startPowerRecovery();
void feedPowerRecovery();
#ifdef MCWAVE_RWDT_DIAGNOSTIC
bool powerResetWasRtcWatchdog();
void freezeForRtcWatchdogTest();
#endif
#else
inline void startPowerRecovery(){}
inline void feedPowerRecovery(){}
#endif
