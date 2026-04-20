// espnow_scan.h
#pragma once
#include <Arduino.h>
#include <esp_wifi_types.h>

struct EspNowFrame {
  uint8_t  mac[6];
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  len;
  uint32_t tMs;
};

void espnowScanBegin();
// Called by detect.cpp when it sees a mgmt ACTION frame. Filters for
// vendor-specific (OUI 18:FE:34) and appends to the ring if matched.
void espnowIngestFrame(const wifi_promiscuous_pkt_t* p);
int  espnowScanSnapshot(EspNowFrame* out, int maxOut);   // newest first
uint32_t espnowScanSeenCount();    // total ESP-NOW-tagged frames seen
