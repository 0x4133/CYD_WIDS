// alerts.cpp
#include "alerts.h"
#include "config.h"
#include "sd_writer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t mtx;
static Alert ring[ALERT_LOG_MAX];
static int   ringHead  = 0;   // next slot to write
static int   ringCount = 0;
static volatile uint32_t total  = 0;
static volatile uint32_t lastMs = 0;

#define ALERT_TRACK_MAX 48
struct AlertTrack {
  AlertType type;
  uint8_t   mac[6];
  uint32_t  firstMs;
  uint32_t  lastMs;
  uint16_t  count;
  bool      acked;
};
static AlertTrack track[ALERT_TRACK_MAX];
static int trackN = 0;

static const char* kNames[] = {
  "NONE", "NEW-WIFI", "NEW-BLE", "NEW-ESPNOW",
  "DEAUTH", "DISASSOC", "PROBE-FLOOD"
};

static void fmtMac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static AlertTrack* trackFind(AlertType t, const uint8_t mac[6]) {
  for (int i = 0; i < trackN; i++) {
    if (track[i].type == t && memcmp(track[i].mac, mac, 6) == 0) return &track[i];
  }
  return nullptr;
}

static AlertTrack* trackFindOrAdd(AlertType t, const uint8_t mac[6], uint32_t nowMs) {
  AlertTrack* s = trackFind(t, mac);
  if (s) return s;
  int idx = -1;
  if (trackN < ALERT_TRACK_MAX) idx = trackN++;
  else {
    // Reuse the oldest slot.
    idx = 0;
    for (int i = 1; i < trackN; i++) {
      if (track[i].lastMs < track[idx].lastMs) idx = i;
    }
  }
  track[idx].type = t;
  memcpy(track[idx].mac, mac, 6);
  track[idx].firstMs = nowMs;
  track[idx].lastMs  = nowMs;
  track[idx].count   = 0;
  track[idx].acked   = false;
  return &track[idx];
}

void alertsBegin() {
  mtx = xSemaphoreCreateMutex();
  ringHead = 0;
  ringCount = 0;
  total = 0;
  lastMs = 0;
  trackN = 0;
  memset(track, 0, sizeof(track));
}

void alertRaise(AlertType t, const uint8_t mac[6], uint16_t extra, const char* label) {
  if (!mtx) return;
  uint8_t macSafe[6] = {0};
  if (mac) memcpy(macSafe, mac, 6);
  uint32_t nowMs = millis();

  xSemaphoreTake(mtx, portMAX_DELAY);
  AlertTrack* tr = trackFindOrAdd(t, macSafe, nowMs);
  tr->lastMs = nowMs;
  tr->count++;

  bool suppress = tr->acked;
  bool autoAcked = false;
  if (!suppress && tr->firstMs != 0 && (nowMs - tr->firstMs) >= ALERT_AUTO_ACK_DWELL_MS) {
    tr->acked = true;
    suppress = true;
    autoAcked = true;
  }

  if (suppress) {
    xSemaphoreGive(mtx);
    if (autoAcked) {
      char macs[18];
      fmtMac(macSafe, macs);
      String row = String(nowMs) + "|ALERT_AUTO_ACK|" + alertTypeName(t) +
                   "|mac=" + macs + ",dwell_ms=" + String(nowMs - tr->firstMs);
      sdWriterEnqueue("/alerts.log", row);
      Serial.printf("[ALERT] auto-ack %s mac=%s dwell=%lums\n",
        alertTypeName(t), macs, (unsigned long)(nowMs - tr->firstMs));
    }
    return;
  }

  Alert& a = ring[ringHead];
  a.type = t;
  memcpy(a.mac, macSafe, 6);
  a.extra = extra;
  a.tMs   = nowMs;
  a.tMs   = millis();
  a.ackMs = 0;
  a.acked = false;
  if (label) strlcpy(a.label, label, ALERT_LABEL_MAX);
  else a.label[0] = 0;
  ringHead = (ringHead + 1) % ALERT_LOG_MAX;
  if (ringCount < ALERT_LOG_MAX) ringCount++;
  total++;
  lastMs = a.tMs;
  xSemaphoreGive(mtx);

  char macs[18];
  fmtMac(a.mac, macs);
  String row = String(a.tMs) + "|ALERT|" + alertTypeName(t) +
               "|mac=" + macs + ",extra=" + String(extra) +
               ",label=" + String(a.label);
  sdWriterEnqueue("/alerts.log", row);
  Serial.printf("[ALERT] %s mac=%s extra=%u label=%s\n",
    alertTypeName(t), macs, extra, a.label);
}

static bool ackByRingIndexNoLock(int ringIdx, const char* reason) {
  if (ringIdx < 0 || ringIdx >= ALERT_LOG_MAX) return false;
  Alert& a = ring[ringIdx];
  if (a.type == ALERT_NONE || a.acked) return false;
  a.acked = true;
  a.ackMs = millis();
  char macs[18];
  snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
    a.mac[0],a.mac[1],a.mac[2],a.mac[3],a.mac[4],a.mac[5]);
  String row = String(a.ackMs) + "|ALERT_ACK|" + alertTypeName(a.type) +
               "|mac=" + macs + ",reason=" + String(reason ? reason : "manual");
  sdWriterEnqueue("/alerts.log", row);
  Serial.printf("[ALERT] ACK %s mac=%s reason=%s dwell=%lums\n",
    alertTypeName(a.type), macs, reason ? reason : "manual",
    (unsigned long)(a.ackMs - a.tMs));
  return true;
}

int alertSnapshot(Alert* out, int maxOut) {
  if (!mtx) return 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = 0;
  for (int i = 0; i < ringCount && n < maxOut; i++) {
    int idx = (ringHead - 1 - i + ALERT_LOG_MAX) % ALERT_LOG_MAX;
    const Alert& a = ring[idx];
    AlertTrack* tr = trackFind(a.type, a.mac);
    if (tr && tr->acked) continue;
    out[n++] = a;
  }
  xSemaphoreGive(mtx);
  return n;
}

bool alertAckBySnapshotIndex(int idx, const char* reason) {
  if (!mtx || idx < 0) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (idx >= ringCount) {
    xSemaphoreGive(mtx);
    return false;
  }
  int ringIdx = (ringHead - 1 - idx + ALERT_LOG_MAX) % ALERT_LOG_MAX;
  bool ok = ackByRingIndexNoLock(ringIdx, reason);
  xSemaphoreGive(mtx);
  return ok;
}

void alertsAutoAckTick() {
  if (!mtx) return;
  uint32_t now = millis();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < ringCount; i++) {
    int ringIdx = (ringHead - 1 - i + ALERT_LOG_MAX) % ALERT_LOG_MAX;
    Alert& a = ring[ringIdx];
    if (a.acked) continue;
    if (now - a.tMs >= ALERT_AUTO_ACK_DWELL_MS) {
      ackByRingIndexNoLock(ringIdx, "dwell");
    }
  }
  xSemaphoreGive(mtx);
}

void alertsClear() {
  if (!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  ringHead = 0;
  ringCount = 0;
  trackN = 0;
  memset(track, 0, sizeof(track));
  xSemaphoreGive(mtx);
}

bool alertAck(AlertType t, const uint8_t mac[6]) {
  if (!mtx || !mac) return false;
  bool changed = false;
  uint32_t nowMs = millis();
  xSemaphoreTake(mtx, portMAX_DELAY);
  AlertTrack* tr = trackFindOrAdd(t, mac, nowMs);
  if (!tr->acked) { tr->acked = true; changed = true; }
  tr->lastMs = nowMs;
  xSemaphoreGive(mtx);
  if (changed) {
    char macs[18];
    fmtMac(mac, macs);
    String row = String(nowMs) + "|ALERT_ACK|" + alertTypeName(t) + "|mac=" + macs;
    sdWriterEnqueue("/alerts.log", row);
  }
  return changed;
}

bool alertAckOne(const Alert& a) {
  return alertAck(a.type, a.mac);
}

bool alertIsAcked(AlertType t, const uint8_t mac[6]) {
  if (!mtx || !mac) return false;
  bool acked = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  AlertTrack* tr = trackFind(t, mac);
  acked = tr && tr->acked;
  xSemaphoreGive(mtx);
  return acked;
}

uint32_t alertLastMs()     { return lastMs; }
uint32_t alertTotalCount() { return total;  }

const char* alertTypeName(AlertType t) {
  int i = (int)t;
  if (i < 0 || i > 6) return "?";
  return kNames[i];
}
