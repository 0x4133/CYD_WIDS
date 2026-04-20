// alerts.cpp
#include "alerts.h"
#include "sd_writer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t mtx;
static Alert ring[ALERT_LOG_MAX];
static int   ringHead  = 0;   // next slot to write
static int   ringCount = 0;
static volatile uint32_t total  = 0;
static volatile uint32_t lastMs = 0;

static const char* kNames[] = {
  "NONE", "NEW-WIFI", "NEW-BLE", "NEW-ESPNOW",
  "DEAUTH", "DISASSOC", "PROBE-FLOOD"
};

void alertsBegin() {
  mtx = xSemaphoreCreateMutex();
  ringHead = 0;
  ringCount = 0;
  total = 0;
  lastMs = 0;
}

void alertRaise(AlertType t, const uint8_t mac[6], uint16_t extra, const char* label) {
  if (!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  Alert& a = ring[ringHead];
  a.type = t;
  if (mac) memcpy(a.mac, mac, 6); else memset(a.mac, 0, 6);
  a.extra = extra;
  a.tMs   = millis();
  if (label) strlcpy(a.label, label, ALERT_LABEL_MAX);
  else a.label[0] = 0;
  ringHead = (ringHead + 1) % ALERT_LOG_MAX;
  if (ringCount < ALERT_LOG_MAX) ringCount++;
  total++;
  lastMs = a.tMs;
  xSemaphoreGive(mtx);

  char macs[18];
  snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
    a.mac[0],a.mac[1],a.mac[2],a.mac[3],a.mac[4],a.mac[5]);
  String row = String(a.tMs) + "|ALERT|" + alertTypeName(t) +
               "|mac=" + macs + ",extra=" + String(extra) +
               ",label=" + String(a.label);
  sdWriterEnqueue("/alerts.log", row);
  Serial.printf("[ALERT] %s mac=%s extra=%u label=%s\n",
    alertTypeName(t), macs, extra, a.label);
}

int alertSnapshot(Alert* out, int maxOut) {
  if (!mtx) return 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(ringCount, maxOut);
  for (int i = 0; i < n; i++) {
    int idx = (ringHead - 1 - i + ALERT_LOG_MAX) % ALERT_LOG_MAX;
    out[i] = ring[idx];
  }
  xSemaphoreGive(mtx);
  return n;
}

void alertsClear() {
  if (!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  ringHead = 0;
  ringCount = 0;
  xSemaphoreGive(mtx);
}

uint32_t alertLastMs()     { return lastMs; }
uint32_t alertTotalCount() { return total;  }

const char* alertTypeName(AlertType t) {
  int i = (int)t;
  if (i < 0 || i > 6) return "?";
  return kNames[i];
}
