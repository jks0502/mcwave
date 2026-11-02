#pragma once
#if defined(MCWAVE_MULTI_NETWORK)
namespace njitProfile {
#include "NjitSecrets.h"
}
namespace seuProfile {
#include "CampusSecrets.h"
}
#include "MultiNetworkSecrets.h"
#include "WifiProfiles.h"
inline bool campusUsesPortal(){return activeWifiProfile()!=1;}
inline bool campusUsesNjit(){return activeWifiProfile()==0;}
inline const char *campusUsername(){return campusUsesNjit()?njitProfile::kCampusUsername:seuProfile::kCampusUsername;}
inline const char *campusPassword(){return campusUsesNjit()?njitProfile::kCampusPassword:seuProfile::kCampusPassword;}
#else
#if defined(MCWAVE_NJIT)
#include "NjitSecrets.h"
constexpr bool kCampusNjit = true;
#else
constexpr bool kCampusNjit = false;
#if __has_include("CampusSecrets.h")
#include "CampusSecrets.h"
#else
#include "CampusSecrets.example.h"
#endif
#endif
inline bool campusUsesPortal(){return kCampusEnabled;}
inline bool campusUsesNjit(){return kCampusNjit;}
inline const char *campusUsername(){return kCampusUsername;}
inline const char *campusPassword(){return kCampusPassword;}
#endif
