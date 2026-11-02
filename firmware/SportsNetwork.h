#pragma once
#include "SportsData.h"
void startSportsNetwork();
void requestSports(const char *date);
bool pollSports(SportsData &data, bool &failed, bool *freshResult=nullptr);
