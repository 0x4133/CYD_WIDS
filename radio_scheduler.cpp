// radio_scheduler.cpp
#include "radio_scheduler.h"
#include "config.h"
#include "detect.h"
#include "sd_writer.h"
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static WifiSeen wifiLive[MAX_WIFI];
static int      wifiLiveN = 0;
static BleSeen  bleLive [MAX_BLE];
static int      bleLiveN  = 0;

static SemaphoreHandle_t mtx;
static volatile uint32_t tLastWifiScan = 0;
static volatile uint32_t tLastBleSeen  = 0;

static void drainBle() {
  BleSeen s;
  while (xQueueReceive(bleScanQueue(), &s, 0) == pdTRUE) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < bleLiveN; i++)
      if (strcmp(bleLive[i].mac, s.mac) == 0) { idx = i; break; }
    if (idx < 0 && bleLiveN < MAX_BLE) idx = bleLiveN++;
    if (idx >= 0) bleLive[idx] = s;
    tLastBleSeen = millis();
    xSemaphoreGive(mtx);

    String row = String(millis()) + "|BLE|mac=" + s.mac +
                 ",name=" + s.name + ",rssi=" + String(s.rssi) +
                 (s.svcUuid[0] ? String(",svc=") + s.svcUuid : String(""));
    sdWriterEnqueue("/scan_ble.log", row);
  }
}

static bool bleUp = false;
static volatile bool xferPaused = false;
static volatile bool focusPaused = false;

static inline bool radioAnyPaused() { return xferPaused || focusPaused; }

static void applyPause() {
  // Pause BLE + detect immediately so ESP-NOW has the radio.
  if (bleUp) bleScanPause();
  detectPause();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

static void applyResume() {
  detectResume();
  if (bleUp) bleScanResume();
}

void radioPauseForXfer() {
  if (xferPaused) return;
  bool wasAny = radioAnyPaused();
  xferPaused = true;
  if (!wasAny) applyPause();
  Serial.println("[RADIO] paused for xfer");
}

void radioResumeAfterXfer() {
  if (!xferPaused) return;
  xferPaused = false;
  if (!radioAnyPaused()) applyResume();
  Serial.println("[RADIO] resumed after xfer");
}

bool radioPausedForXfer() { return xferPaused; }

void radioPauseForFocus() {
  if (focusPaused) return;
  bool wasAny = radioAnyPaused();
  focusPaused = true;
  if (!wasAny) applyPause();
  Serial.println("[RADIO] paused for focus");
}

void radioResumeAfterFocus() {
  if (!focusPaused) return;
  focusPaused = false;
  if (!radioAnyPaused()) applyResume();
  Serial.println("[RADIO] resumed after focus");
}

bool radioPausedForFocus() { return focusPaused; }

// Don't start a WiFi scan when heap is already near the floor. scanNetworks
// allocates internal WiFi task buffers; on this board we've observed
// "wifi:wifi ipc: failed to post wifi task" followed by a null-deref crash
// when free heap dips under ~8 KB. Skipping the scan this cycle lets other
// tasks (SD writer, BLE queue drain) reclaim memory.
#define SURVEY_HEAP_MIN_BYTES 15000U

static void runSurvey() {
  uint32_t preHeap = ESP.getFreeHeap();
  if (preHeap < SURVEY_HEAP_MIN_BYTES) {
    Serial.printf("[RADIO] skip scan heap=%u<%u\n", preHeap, SURVEY_HEAP_MIN_BYTES);
    return;
  }
  // Time-slice: pause BLE while WiFi scans. Classic ESP32 shares a single
  // radio between WiFi and BT; concurrent scans inflate coex buffers and
  // can OOM on a board this tight on heap.
  if (bleUp) { bleScanPause(); vTaskDelay(pdMS_TO_TICKS(100)); }
  // Promiscuous must be off during STA scan — scanNetworks reprograms the
  // channel list and RX filter. Pause without clearing the user-visible flag
  // so detectResume() below restores the state the user asked for, even if
  // the scan failed partway.
  detectPause();
  Serial.printf("[RADIO] wifi scan start heap=%u\n", preHeap);
  wifiSurveyStart();
  while (!wifiSurveyPoll()) { vTaskDelay(pdMS_TO_TICKS(100)); }

  // Write results directly into the live store. The radio task is the only
  // writer; consumers only read under mtx, so iterating wifiLive unlocked
  // below for SD logging is safe (we're not modifying it here).
  xSemaphoreTake(mtx, portMAX_DELAY);
  wifiLiveN = wifiSurveyResults(wifiLive, MAX_WIFI);
  tLastWifiScan = millis();
  int n = wifiLiveN;
  xSemaphoreGive(mtx);

  Serial.printf("[RADIO] wifi scan done n=%d heap=%u\n", n, ESP.getFreeHeap());

  // Pin radio back to the ESP-NOW channel. scanNetworks leaves the radio on
  // whatever channel the last AP was found on; without this, chat RX misses
  // everything between scans.
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Restore promiscuous to the user-visible setting (regardless of scan result).
  detectResume();

  if (bleUp) { vTaskDelay(pdMS_TO_TICKS(100)); bleScanResume(); }

  for (int i = 0; i < n; i++) {
    String row = String(millis()) + "|WIFI|ssid=" + wifiLive[i].ssid +
                 ",bssid=" + wifiLive[i].bssid +
                 ",ch="    + String(wifiLive[i].channel) +
                 ",auth="  + wifiAuthName(wifiLive[i].auth) +
                 ",rssi="  + String(wifiLive[i].rssi);
    sdWriterEnqueue("/scan_wifi.log", row);
  }
}

static void radioTask(void*) {
  Serial.printf("[RADIO] task up heap=%u\n", ESP.getFreeHeap());
  vTaskDelay(pdMS_TO_TICKS(500));

  // Both WiFi and BLE are initialized on the main task in setup().
  // BLE scan is already running continuously; we just pause/resume it
  // around WiFi scans.
  bleUp = true;

  for (;;) {
    if (!radioAnyPaused()) runSurvey();
    uint32_t t0 = millis();
    while (millis() - t0 < SURVEY_PERIOD_MS) {
      if (!radioAnyPaused()) drainBle();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// Static stack + TCB in BSS so the 6 KB stack allocation doesn't carve up the
// heap's largest contiguous block. Dynamic xTaskCreate left heap largest=7156
// for the rest of the run, which aborted the WiFi driver whenever it needed
// >7 KB contiguous (panic PC 0x4008354b). Room for this in BSS was bought by
// halving MAX_BASELINE_BLE (6.5 KB freed).
#define RADIO_STACK_BYTES 6144
static StaticTask_t radioTaskBuf;
static StackType_t  radioStack[RADIO_STACK_BYTES / sizeof(StackType_t)];

void radioBegin() {
  mtx = xSemaphoreCreateMutex();
  TaskHandle_t h = xTaskCreateStaticPinnedToCore(
    radioTask, "radio", RADIO_STACK_BYTES, nullptr, 1,
    radioStack, &radioTaskBuf, 1);
  Serial.printf("[RADIO] create (static) h=%p heap=%u largest=%u\n",
    h, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

int radioSnapshotWifi(WifiSeen* out, int maxOut) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(wifiLiveN, maxOut);
  memcpy(out, wifiLive, sizeof(WifiSeen) * n);
  xSemaphoreGive(mtx);
  return n;
}
int radioSnapshotBle(BleSeen* out, int maxOut) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(bleLiveN, maxOut);
  memcpy(out, bleLive, sizeof(BleSeen) * n);
  xSemaphoreGive(mtx);
  return n;
}
uint32_t radioLastWifiScanMs() { return tLastWifiScan; }
uint32_t radioLastBleSeenMs()  { return tLastBleSeen;  }
