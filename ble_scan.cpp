// ble_scan.cpp
#include "ble_scan.h"
#include "config.h"
#include <NimBLEDevice.h>

static QueueHandle_t q = nullptr;

class Cb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    BleSeen s{};
    strlcpy(s.mac, d->getAddress().toString().c_str(), sizeof(s.mac));
    if (d->haveName()) strlcpy(s.name, d->getName().c_str(), sizeof(s.name));
    else               strlcpy(s.name, "(no name)",         sizeof(s.name));
    s.rssi = d->getRSSI();
    if (d->haveServiceUUID())
      strlcpy(s.svcUuid, d->getServiceUUID().toString().c_str(), sizeof(s.svcUuid));
    xQueueSend(q, &s, 0);  // drop if full — better than blocking NimBLE's task
  }
};
static Cb cb;

void bleScanBegin() {
  q = xQueueCreate(BLE_QUEUE_LEN, sizeof(BleSeen));
  // Larger dedupe cache → fewer NimBLEAdvertisedDevice allocations under
  // advert-flooded environments. 200 is the NimBLE-supported ceiling.
  NimBLEDevice::setScanDuplicateCacheSize(200);
  NimBLEDevice::init("CYD-WIDS");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&cb, false);
  // Passive scan: we listen only — no SCAN_REQ sent, so devices don't reply
  // with SCAN_RSP. Roughly halves advert traffic and the NimBLE allocations
  // per tick. For a WIDS we only need BDADDR + raw adv payload anyway.
  scan->setActiveScan(false);
  // Duty cycle at ~50% (window/interval) instead of ~99%. Gives the NimBLE
  // task idle time to drain its event queue before new adverts land and
  // keeps heap from being starved by a tight advert storm.
  scan->setInterval(160);  // 100ms (units of 0.625ms)
  scan->setWindow(80);     // 50ms
  scan->start(0, false);   // continuous, non-blocking
}

void bleScanPause() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan) scan->stop();
}

void bleScanResume() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan) scan->start(0, false);
}

QueueHandle_t bleScanQueue() { return q; }

#define BLE_HEAP_CRIT    10000
#define BLE_HEAP_RESUME  18000

void bleScanHeapTick() {
  static bool heapPaused = false;
  uint32_t h = ESP.getFreeHeap();
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return;
  if (!heapPaused && h < BLE_HEAP_CRIT) {
    Serial.printf("[BLE] heap-pause heap=%u<%u\n", h, BLE_HEAP_CRIT);
    scan->stop();
    heapPaused = true;
  } else if (heapPaused && h > BLE_HEAP_RESUME) {
    Serial.printf("[BLE] heap-resume heap=%u>%u\n", h, BLE_HEAP_RESUME);
    scan->start(0, false);
    heapPaused = false;
  }
}
