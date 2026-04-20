// ble_scan.h
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct BleSeen {
  char mac[18];
  char name[33];
  int16_t rssi;
  char svcUuid[40];   // first service UUID if any
};

void bleScanBegin();           // init NimBLE, start continuous scan
void bleScanPause();           // stop scan (e.g. while WiFi scans)
void bleScanResume();          // restart continuous scan
QueueHandle_t bleScanQueue();  // the queue producers feed; scheduler drains

// Heap-guard tick. Call periodically from loop(). When free heap falls below
// the critical floor the scan is stopped (NimBLE internally allocates per
// advert even with dedupe); when heap climbs back over the resume threshold
// the scan is restarted. Separate from bleScanPause so the radio scheduler's
// WiFi-scan coordination isn't affected.
void bleScanHeapTick();
