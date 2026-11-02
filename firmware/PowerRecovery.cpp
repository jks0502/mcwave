#include "PowerRecovery.h"
#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp32/rom/rtc.h>
#include <esp_private/esp_clk.h>
#include <hal/brownout_hal.h>
#include <hal/brownout_ll.h>
#include <hal/wdt_hal.h>
#include <soc/rtc.h>
#include <soc/rtc_cntl_reg.h>
#include "PowerRecoveryPolicy.h"

namespace {
wdt_hal_context_t rtcWatchdog=RWDT_HAL_CONTEXT_DEFAULT();
bool lastWasRtcWatchdog=false;
#ifndef MCWAVE_BROWNOUT_LEVEL
// IDF v5.5.4 ESP32 Kconfig.power: level 5 = 2.70V +/-0.05 (estimate).
#define MCWAVE_BROWNOUT_LEVEL 5
#endif
#ifdef MCWAVE_RWDT_DIAGNOSTIC
constexpr uint32_t timeoutMs=60000;
#else
constexpr uint32_t timeoutMs=mcwavePower::kRtcTimeoutMs;
#endif
}

void startPowerRecovery() {
  // SDK starts with a software brownout interrupt. Classic ESP32 rev3 supports
  // hardware reset; keep SDK flash/RF power-down choices, bypass its ISR.
  brownout_hal_config_t cfg={};
  cfg.threshold=MCWAVE_BROWNOUT_LEVEL;cfg.enabled=true;cfg.reset_enabled=true;
  cfg.flash_power_down=RTCCNTL.brown_out.close_flash_ena;
  cfg.rf_power_down=RTCCNTL.brown_out.pd_rf_ena;
  brownout_ll_intr_enable(false);
  brownout_hal_config(&cfg);
  brownout_ll_intr_clear();
  Serial.printf("[power] brownout level=%u enabled=%u reset=%u intr=%u flash_pd=%u rf_pd=%u reg=0x%08lx\n",
    unsigned(RTCCNTL.brown_out.thres),unsigned(RTCCNTL.brown_out.ena),unsigned(RTCCNTL.brown_out.rst_ena),
    unsigned(RTCCNTL.int_ena.rtc_brown_out),unsigned(RTCCNTL.brown_out.close_flash_ena),unsigned(RTCCNTL.brown_out.pd_rf_ena),
    (unsigned long)REG_READ(RTC_CNTL_BROWN_OUT_REG));

  // IDF startup_funcs.c disables the bootloader RWDT before app_main because
  // CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE is unset. Take over explicitly.
  const bool wasEnabled=wdt_hal_is_enabled(&rtcWatchdog);
  const uint32_t period=esp_clk_slowclk_cal_get();uint32_t ticks=0;
  ESP_ERROR_CHECK(mcwavePower::rtcTicks(timeoutMs,period,ticks)?ESP_OK:ESP_ERR_INVALID_STATE);
  wdt_hal_init(&rtcWatchdog,WDT_RWDT,0,false);
  wdt_hal_write_protect_disable(&rtcWatchdog);
  wdt_hal_set_flashboot_en(&rtcWatchdog,false);
  wdt_hal_config_stage(&rtcWatchdog,WDT_STAGE0,ticks,WDT_STAGE_ACTION_RESET_RTC);
  wdt_hal_enable(&rtcWatchdog);
  wdt_hal_write_protect_enable(&rtcWatchdog);
  Serial.printf("[power] rwdt timeout_ms=%lu cal_q19=%lu slow_hz=%lu source=%u ticks=%lu action=%u enabled=%u previous_enabled=%u\n",
    (unsigned long)timeoutMs,(unsigned long)period,(unsigned long)((1000000ULL<<19)/period),unsigned(rtc_clk_slow_src_get()),
    (unsigned long)ticks,unsigned(RTCCNTL.wdt_config0.stg0),unsigned(wdt_hal_is_enabled(&rtcWatchdog)),unsigned(wasEnabled));

  const uint32_t why=esp_reset_reason(),raw0=rtc_get_reset_reason(0),raw1=rtc_get_reset_reason(1);
  lastWasRtcWatchdog=mcwavePower::isRtcWatchdog(raw0)||mcwavePower::isRtcWatchdog(raw1);
  mcwavePower::BootCounts counts;
  Preferences storage;bool saved=false;
  if(storage.begin("boot-v1",false)) {
    if(storage.getBytesLength("counts")==sizeof(counts)) {
      mcwavePower::BootCounts previous;
      if(storage.getBytes("counts",&previous,sizeof(previous))==sizeof(previous)&&previous.magic==mcwavePower::kCounterMagic)counts=previous;
    }
    counts.record(why,raw0,raw1);
    saved=storage.putBytes("counts",&counts,sizeof(counts))==sizeof(counts); // One commit per boot, never per loop.
    storage.end();
  }
  Serial.printf("[boot-count] stored=%u total=%lu reason=%lu %s count=%lu raw0=%lu raw1=%lu brownout=%lu rwdt=%lu\n",
    unsigned(saved),(unsigned long)counts.total,(unsigned long)why,mcwavePower::reasonName(why),
    (unsigned long)counts.reason[why<mcwavePower::kReasonCount?why:0],(unsigned long)raw0,(unsigned long)raw1,
    (unsigned long)counts.reason[ESP_RST_BROWNOUT],(unsigned long)counts.rwdt);
}

void feedPowerRecovery() {
  wdt_hal_write_protect_disable(&rtcWatchdog);
  wdt_hal_feed(&rtcWatchdog);
  wdt_hal_write_protect_enable(&rtcWatchdog);
}

#ifdef MCWAVE_RWDT_DIAGNOSTIC
bool powerResetWasRtcWatchdog(){return lastWasRtcWatchdog;}
void freezeForRtcWatchdogTest() {
  // Diagnostic only: isolate RWDT from both MWDTs. Otherwise the interrupt
  // watchdog can reset the chip within a second, before the 60-second RWDT.
  wdt_hal_context_t mwdt={};mwdt.inst=WDT_MWDT0;mwdt.mwdt_dev=&TIMERG0;
  wdt_hal_write_protect_disable(&mwdt);wdt_hal_disable(&mwdt);wdt_hal_write_protect_enable(&mwdt);
  mwdt.inst=WDT_MWDT1;mwdt.mwdt_dev=&TIMERG1;
  wdt_hal_write_protect_disable(&mwdt);wdt_hal_disable(&mwdt);wdt_hal_write_protect_enable(&mwdt);
  feedPowerRecovery();
  Serial.println("[diag] RWDT armed 60s; MWDT0/1 disabled; disabling interrupts now");Serial.flush();
  portDISABLE_INTERRUPTS();
  for(;;)asm volatile("nop");
}
#endif
#endif
