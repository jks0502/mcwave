#pragma once
// Optional per-device provisioned credentials; never committed to source control.
#if __has_include("DeviceIdentity.h")
#include "DeviceIdentity.h"
#else
#include "RemoteSecrets.h"
#endif
