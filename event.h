// event.h
#pragma once
#include <Arduino.h>

enum Severity : uint8_t { SEV_INFO = 0, SEV_MED = 1, SEV_HIGH = 2, SEV_CRIT = 3 };

// M1 only emits NEW_WIFI / NEW_BLE / BASELINE_LOCKED / SD_DROP.
// Plan 2 will extend this enum with EVIL_TWIN, DEAUTH_FLOOD, etc.
enum EventKind : uint8_t {
  EV_NEW_WIFI = 0,
  EV_NEW_BLE  = 1,
  EV_BASELINE_LOCKED = 2,
  EV_SD_DROP  = 3,
};

struct Event {
  uint32_t tMs;      // millis() at emit
  EventKind kind;
  Severity sev;
  char     ssid[33];
  char     bssid[18];
  char     name[33];
  char     mac[18];
  int16_t  rssi;
  uint8_t  channel;
};

inline const char* severityName(Severity s) {
  switch (s) {
    case SEV_INFO: return "INFO";
    case SEV_MED:  return "MED";
    case SEV_HIGH: return "HIGH";
    case SEV_CRIT: return "CRIT";
  }
  return "?";
}

inline const char* eventKindName(EventKind k) {
  switch (k) {
    case EV_NEW_WIFI:        return "NEW_WIFI";
    case EV_NEW_BLE:         return "NEW_BLE";
    case EV_BASELINE_LOCKED: return "BASELINE_LOCKED";
    case EV_SD_DROP:         return "SD_DROP";
  }
  return "?";
}
