// detect.cpp
#include "detect.h"
#include "alerts.h"
#include "espnow_scan.h"
#include "config.h"
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 802.11 subtypes we care about (frame_ctrl[0] & 0xFC):
//   0x40  probe request
//   0xA0  disassoc
//   0xC0  deauth
//   0xD0  action (ESP-NOW rides here)
#define WIFI_ST_PROBE     0x40
#define WIFI_ST_DISASSOC  0xA0
#define WIFI_ST_DEAUTH    0xC0
#define WIFI_ST_ACTION    0xD0

// Rolling-window thresholds over WINDOW_MS:
//   deauth  / disassoc: alert above FLOOD_MGMT (per MAC or aggregate)
//   probe requests:     alert above PROBE_FLOOD (aggregate)
#define DETECT_WINDOW_MS    60000UL
#define FLOOD_MGMT             10
#define PROBE_FLOOD           200
#define ALERT_COOLDOWN_MS   15000UL   // per-type cooldown, aggregate

typedef struct {
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];   // receiver
  uint8_t addr2[6];   // sender
  uint8_t addr3[6];   // bssid
  uint8_t seq_ctrl[2];
} __attribute__((packed)) WifiMacHdr;

typedef struct {
  uint8_t  mac[6];
  uint16_t count;
  uint32_t windowStart;
} MacCounter;

#define MAC_TABLE_N 16

static SemaphoreHandle_t mtx;
static MacCounter deauthTbl [MAC_TABLE_N];
static MacCounter disassocTbl[MAC_TABLE_N];
static uint32_t probeCount   = 0;
static uint32_t probeWindow  = 0;

static uint32_t lastAlertDeauth   = 0;
static uint32_t lastAlertDisassoc = 0;
static uint32_t lastAlertProbe    = 0;

static volatile bool enabled = false;
// Tracks whether the driver is *actually* promiscuous right now. Diverges from
// `enabled` when the heap-guard has auto-paused: user wants it on, but free
// heap dropped below DETECT_HEAP_CRIT so we dropped it to protect the WiFi
// driver from its own `promis buf: out of memory` assert path.
static bool    radioOn = false;

// Heap thresholds. CRIT is where we pull the plug; RESUME has hysteresis so
// we don't flap on and off each tick. Chosen from observed min=7620 crashes
// and the ~20 KB headroom the driver seems to need for promiscuous buffers.
#define DETECT_HEAP_CRIT    15000
#define DETECT_HEAP_RESUME  22000

static MacCounter* slotFor(MacCounter* tbl, const uint8_t* mac, uint32_t now) {
  // Find existing slot, recycle expired one, or pick oldest.
  MacCounter* oldest = &tbl[0];
  for (int i = 0; i < MAC_TABLE_N; i++) {
    if (memcmp(tbl[i].mac, mac, 6) == 0) return &tbl[i];
    if (tbl[i].count == 0) { oldest = &tbl[i]; break; }
    if (tbl[i].windowStart < oldest->windowStart) oldest = &tbl[i];
  }
  memcpy(oldest->mac, mac, 6);
  oldest->count = 0;
  oldest->windowStart = now;
  return oldest;
}

static bool tickAndCheckFlood(MacCounter* tbl, const uint8_t* mac,
                              uint32_t now, uint16_t threshold,
                              uint16_t* outCount) {
  MacCounter* s = slotFor(tbl, mac, now);
  if (now - s->windowStart > DETECT_WINDOW_MS) {
    s->count = 0; s->windowStart = now;
  }
  s->count++;
  *outCount = s->count;
  return s->count == threshold;  // fire exactly once per window
}

static void IRAM_ATTR sniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* pl = p->payload;
  int plen = p->rx_ctrl.sig_len;
  if (plen < (int)sizeof(WifiMacHdr)) return;

  uint8_t st = pl[0] & 0xFC;
  const WifiMacHdr* hdr = (const WifiMacHdr*)pl;

  if (st == WIFI_ST_ACTION) {
    // Vendor-specific action → ESP-NOW sniffer handles it
    espnowIngestFrame(p);
    return;
  }

  if (xSemaphoreTake(mtx, 0) != pdTRUE) return;
  uint32_t now = millis();
  bool fireDeauth = false, fireDisassoc = false, fireProbe = false;
  uint16_t cnt = 0;
  uint8_t  srcMac[6]; memcpy(srcMac, hdr->addr2, 6);

  if (st == WIFI_ST_DEAUTH) {
    fireDeauth = tickAndCheckFlood(deauthTbl, srcMac, now, FLOOD_MGMT, &cnt);
  } else if (st == WIFI_ST_DISASSOC) {
    fireDisassoc = tickAndCheckFlood(disassocTbl, srcMac, now, FLOOD_MGMT, &cnt);
  } else if (st == WIFI_ST_PROBE) {
    if (now - probeWindow > DETECT_WINDOW_MS) {
      probeCount = 0; probeWindow = now;
    }
    probeCount++;
    if (probeCount == PROBE_FLOOD) { fireProbe = true; cnt = probeCount; }
  }
  uint8_t ch = p->rx_ctrl.channel;
  xSemaphoreGive(mtx);

  // Alert outside the mutex — alertRaise enqueues to SD and logs to Serial.
  if (fireDeauth && now - lastAlertDeauth > ALERT_COOLDOWN_MS) {
    lastAlertDeauth = now;
    char lbl[24]; snprintf(lbl, sizeof(lbl), "ch%u cnt%u", ch, cnt);
    alertRaise(ALERT_DEAUTH, srcMac, cnt, lbl);
  }
  if (fireDisassoc && now - lastAlertDisassoc > ALERT_COOLDOWN_MS) {
    lastAlertDisassoc = now;
    char lbl[24]; snprintf(lbl, sizeof(lbl), "ch%u cnt%u", ch, cnt);
    alertRaise(ALERT_DISASSOC, srcMac, cnt, lbl);
  }
  if (fireProbe && now - lastAlertProbe > ALERT_COOLDOWN_MS) {
    lastAlertProbe = now;
    char lbl[24]; snprintf(lbl, sizeof(lbl), "ch%u cnt%u", ch, cnt);
    alertRaise(ALERT_PROBE_FLOOD, srcMac, cnt, lbl);
  }
}

void detectBegin() {
  mtx = xSemaphoreCreateMutex();
  memset(deauthTbl,   0, sizeof(deauthTbl));
  memset(disassocTbl, 0, sizeof(disassocTbl));
  probeCount = 0;
  probeWindow = millis();
  enabled = false;
}

static void applyRadio(bool on) {
  if (on == radioOn) return;
  if (on) {
    esp_wifi_set_promiscuous_rx_cb(&sniffCb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  } else {
    esp_wifi_set_promiscuous(false);
  }
  radioOn = on;
}

void detectEnable(bool on) {
  if (on == enabled) return;
  enabled = on;
  if (on && ESP.getFreeHeap() < DETECT_HEAP_CRIT) {
    Serial.printf("[DETECT] on requested but heap=%u<%u — deferring\n",
      ESP.getFreeHeap(), DETECT_HEAP_CRIT);
    return;
  }
  applyRadio(on);
  Serial.printf("[DETECT] %s\n", on ? "on" : "off");
}

bool detectEnabled() { return enabled; }

void detectPause() {
  // Driver-level off, regardless of user-visible flag. Safe to call twice.
  applyRadio(false);
}

void detectResume() {
  // Restore to whatever the user asked for, gated by heap so we don't re-arm
  // the promiscuous buffers right as the driver is about to assert.
  if (enabled && ESP.getFreeHeap() < DETECT_HEAP_CRIT) return;
  applyRadio(enabled);
}

void detectHeapTick() {
  if (!enabled) return;
  uint32_t h = ESP.getFreeHeap();
  if (radioOn && h < DETECT_HEAP_CRIT) {
    Serial.printf("[DETECT] auto-pause heap=%u<%u\n", h, DETECT_HEAP_CRIT);
    applyRadio(false);
  } else if (!radioOn && h > DETECT_HEAP_RESUME) {
    Serial.printf("[DETECT] auto-resume heap=%u>%u\n", h, DETECT_HEAP_RESUME);
    applyRadio(true);
  }
}
