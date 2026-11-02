#pragma once
// Kept as a small typed adapter so host tests exercise the exact SDK fields.
// Arduino STA.cpp zeroes this struct; apply AFTER begin(..., tryConnect=false).
template<class Config,class Auth,class Sae>
inline void applyMoyimiSecurity(Config &config,Auth wpa2Minimum,Sae bothMethods) {
  config.sta.threshold.authmode=wpa2Minimum;
  config.sta.sae_pwe_h2e=bothMethods;
  config.sta.pmf_cfg.capable=true;
  config.sta.pmf_cfg.required=false;
}
