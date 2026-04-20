// alerts.h
#pragma once
#include <Arduino.h>

enum AlertType : uint8_t {
  ALERT_NONE         = 0,
  ALERT_NEW_WIFI     = 1,
  ALERT_NEW_BLE      = 2,
  ALERT_NEW_ESPNOW   = 3,
  ALERT_DEAUTH       = 4,   // deauth flood on our listening channel
  ALERT_DISASSOC     = 5,   // disassoc flood on our listening channel
  ALERT_PROBE_FLOOD  = 6,   // unusual probe-request volume
};

#define ALERT_LOG_MAX   16
#define ALERT_LABEL_MAX 24

struct Alert {
  AlertType type;
  uint8_t   mac[6];
  uint16_t  extra;                  // count / channel / rssi, type-dependent
  uint32_t  tMs;
  uint32_t  ackMs;
  bool      acked;
  char      label[ALERT_LABEL_MAX]; // SSID/name snippet, best-effort
};

void alertsBegin();
void alertRaise(AlertType t, const uint8_t mac[6], uint16_t extra, const char* label);
int  alertSnapshot(Alert* out, int maxOut);  // newest first
bool alertAckBySnapshotIndex(int idx, const char* reason);
void alertsAutoAckTick();
void alertsClear();               // wipe the in-memory ring (UI button)
uint32_t alertLastMs();
uint32_t alertTotalCount();
const char* alertTypeName(AlertType t);
