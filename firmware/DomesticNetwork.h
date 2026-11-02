#pragma once
#include <string>
#include "WeatherData.h"
#include "RainForecast.h"
bool fetchDomesticWeather(WeatherData &weather);
bool fetchDomesticRain(RainForecast &rain);
std::string fetchDomesticSports(const char *date);
