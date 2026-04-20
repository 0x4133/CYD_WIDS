// wifi_survey.cpp
#include "wifi_survey.h"
#include "config.h"
#include <WiFi.h>

static bool scanning = false;

void wifiSurveyBegin() {
  // Must be called on the main Arduino task (loopTask on core 1).
  // WiFi driver callbacks bind to the task context that calls init;
  // calling this from a custom task corrupts memory on scan.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  delay(100);
}

static int lastResult = 0;

void wifiSurveyStart() {
  if (scanning) return;
  WiFi.scanDelete();
  scanning = true;
  // Blocking scan. We're on core 1 now (main-loop core), the WiFi driver
  // task has core 0 to itself. Hidden off — the async+hidden combo corrupts
  // memory on this Arduino core revision.
  lastResult = WiFi.scanNetworks(false /*blocking*/, false /*no hidden*/);
}

bool wifiSurveyPoll() {
  if (!scanning) return false;
  scanning = false;
  return true;
}

int wifiSurveyResults(WifiSeen* out, int maxOut) {
  int n = WiFi.scanComplete();
  if (n < 0) return 0;
  int c = min(n, maxOut);
  for (int i = 0; i < c; i++) {
    String s = WiFi.SSID(i);
    strlcpy(out[i].ssid, s.c_str(), sizeof(out[i].ssid));
    String b = WiFi.BSSIDstr(i);
    strlcpy(out[i].bssid, b.c_str(), sizeof(out[i].bssid));
    out[i].rssi    = WiFi.RSSI(i);
    out[i].channel = WiFi.channel(i);
    out[i].auth    = WiFi.encryptionType(i);
  }
  WiFi.scanDelete();
  return c;
}

const char* wifiAuthName(uint8_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:         return "OPEN";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:return "WPA2/3";
    default:                     return "?";
  }
}
