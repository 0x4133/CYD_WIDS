// wifi_survey.h
#pragma once
#include <Arduino.h>

struct WifiSeen {
  char ssid[33];
  char bssid[18];
  int16_t rssi;
  uint8_t channel;
  uint8_t auth;
};

void    wifiSurveyBegin();
void    wifiSurveyStart();              // kicks off async scan
bool    wifiSurveyPoll();               // true exactly once when scan completes
int     wifiSurveyResults(WifiSeen* out, int maxOut);  // returns count
const char* wifiAuthName(uint8_t a);
