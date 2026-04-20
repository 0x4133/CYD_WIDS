// espnow_scan.cpp
#include "espnow_scan.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ESP-NOW rides on vendor-specific action frames:
//   subtype  = ACTION (frame_ctrl[0] & 0xFC == 0xD0)
//   category = 127 (vendor specific)
//   OUI      = 18:FE:34 (Espressif)
// The caller (detect.cpp) has already screened for MGMT+ACTION; we do the
// category+OUI check and extract the source MAC from the 802.11 header.

static SemaphoreHandle_t mtx;
static EspNowFrame ring[MAX_ESPNOW_FRAMES];
static int ringHead = 0;
static int ringCount = 0;
static volatile uint32_t totalSeen = 0;

typedef struct {
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint8_t seq_ctrl[2];
} __attribute__((packed)) WifiMacHdr;

void espnowScanBegin() {
  mtx = xSemaphoreCreateMutex();
  ringHead = 0;
  ringCount = 0;
  totalSeen = 0;
}

void IRAM_ATTR espnowIngestFrame(const wifi_promiscuous_pkt_t* p) {
  if (!p || !mtx) return;
  const uint8_t* pl = p->payload;
  int plen = p->rx_ctrl.sig_len;
  if (plen < (int)sizeof(WifiMacHdr) + 5) return;

  const uint8_t* body = pl + sizeof(WifiMacHdr);
  if (body[0] != 0x7F) return;
  if (body[1] != 0x18 || body[2] != 0xFE || body[3] != 0x34) return;

  const WifiMacHdr* hdr = (const WifiMacHdr*)pl;
  if (xSemaphoreTake(mtx, 0) != pdTRUE) return;
  EspNowFrame& e = ring[ringHead];
  memcpy(e.mac, hdr->addr2, 6);
  e.rssi    = p->rx_ctrl.rssi;
  e.channel = p->rx_ctrl.channel;
  e.len     = (uint8_t)min(plen, 255);
  e.tMs     = millis();
  ringHead  = (ringHead + 1) % MAX_ESPNOW_FRAMES;
  if (ringCount < MAX_ESPNOW_FRAMES) ringCount++;
  totalSeen++;
  xSemaphoreGive(mtx);
}

uint32_t espnowScanSeenCount() { return totalSeen; }

int espnowScanSnapshot(EspNowFrame* out, int maxOut) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(ringCount, maxOut);
  for (int i = 0; i < n; i++) {
    int idx = (ringHead - 1 - i + MAX_ESPNOW_FRAMES) % MAX_ESPNOW_FRAMES;
    out[i] = ring[idx];
  }
  xSemaphoreGive(mtx);
  return n;
}
