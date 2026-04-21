// cyd_wids.ino — M1 entry point
// Required libraries: TFT_eSPI (User_Setup.h preconfigured for CYD),
//                     XPT2046_Bitbang_Slim, NimBLE-Arduino.
// Board: ESP32 Dev Module, Huge APP partition, 4MB flash, DIO.

#include "config.h"
#include "event.h"
#include "sd_writer.h"
#include "radio_scheduler.h"
#include "wifi_survey.h"
#include "ble_scan.h"
#include "baseline.h"
#include "touch.h"
#include "touch_cal.h"
#include "espnow_scan.h"
#include "espnow_chat.h"
#include "alerts.h"
#include "detect.h"
#include "contacts.h"
#include "crypto.h"
#include "file_xfer.h"
#include "light_meter.h"
#include "sd_http.h"
#include "ui.h"
#include <esp_system.h>

uint32_t lastTouch  = 0;
uint32_t lastRedraw = 0;

// Map esp_reset_reason_t → short name for boot log. Tells us whether the
// last boot was a cold power-on, a brownout, a watchdog, a panic (backtrace
// would have been printed on the last boot's tail), etc. Essential for
// diagnosing "random restarts".
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

#define BOOT_HEAP(label) \
  Serial.printf("[BOOT] %-10s free=%u largest=%u\n", \
    (label), ESP.getFreeHeap(), ESP.getMaxAllocHeap())

void setup() {
  Serial.begin(921600);
  delay(200);
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] CYD WIDS M1 reset=%s(%d) free=%u largest=%u\n",
    resetReasonStr(rr), (int)rr, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  uiBegin();           BOOT_HEAP("ui");
  sdWriterBegin();     BOOT_HEAP("sd");
  touchBegin();        BOOT_HEAP("touch");
  lightMeterBegin();   BOOT_HEAP("light");
  sdHttpBegin();       BOOT_HEAP("sdhttp");

  // Touch calibration: load from SD, else run the wizard once and save.
  TouchCal tc;
  if (touchCalLoad(tc)) {
    touchSetCal(tc);
  } else if (sdWriterReady()) {
    Serial.println("[CAL] no saved cal — running wizard");
    if (touchCalRunWizard(tc)) {
      touchSetCal(tc);
      touchCalSave(tc);
    } else {
      Serial.println("[CAL] wizard not verified — using defaults");
    }
  } else {
    Serial.println("[CAL] SD not ready — using defaults");
  }
  BOOT_HEAP("cal");

  alertsBegin();       BOOT_HEAP("alerts");
  // cryptoBegin must run before contactsBegin: contacts derives + caches the
  // per-peer AES key on load, which needs cryptoReady().
  cryptoBegin();       BOOT_HEAP("crypto");
  contactsBegin();     BOOT_HEAP("contacts");
  baselineBegin();     BOOT_HEAP("baseline");
  wifiSurveyBegin();   BOOT_HEAP("wifi");
  bleScanBegin();      BOOT_HEAP("ble");
  espnowScanBegin();   BOOT_HEAP("espnowScan");
  chatBegin();         BOOT_HEAP("chat");
  fileXferBegin();     BOOT_HEAP("fileXfer");
  detectBegin();       BOOT_HEAP("detect");
  radioBegin();        BOOT_HEAP("radio");

  uiGoto(UI_HOME);
  BOOT_HEAP("ready");
}

void loop() {
  int tx, ty;
  if (touchRead(tx, ty) && millis() - lastTouch > TOUCH_DEBOUNCE_MS) {
    lastTouch = millis();
    Serial.printf("[T] %d,%d screen=%d\n", tx, ty, uiCurrent());
    uiOnTouch(tx, ty);
  }

  static uint32_t tBL = 0;
  if (millis() - tBL > 2000) {
    tBL = millis();
    // Static, not stack: these arrays total ~11 KB, which overflows the
    // Arduino loopTask's 8 KB stack and corrupts the heap.
    static WifiSeen w[MAX_WIFI];
    static BleSeen  b[MAX_BLE];
    int nw = radioSnapshotWifi(w, MAX_WIFI);
    int nb = radioSnapshotBle (b, MAX_BLE);
    baselineTick(w, nw, b, nb);
  }

  if (millis() - lastRedraw > 1500) {
    lastRedraw = millis();
    uiRedraw();
  }

  lightMeterSample();
  sdHttpTick();

  static uint32_t hb = 0;
  if (millis() - hb > HEARTBEAT_MS) {
    hb = millis();
    // max = largest allocatable contiguous block (fragmentation indicator).
    // min = lowest free-heap seen since boot (watermark for OOM risk).
    // If free drops and max drops faster, heap is fragmenting — not just
    // being consumed.
    Serial.printf("[HB] t=%lus heap=%u max=%u min=%u bl=%d drop=%lu\n",
      millis()/1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
      ESP.getMinFreeHeap(), baselineStateNow(),
      (unsigned long)sdWriterDropped());
  }

  // Auto-ack alerts that have dwelled unresolved for long enough.
  static uint32_t tAutoAck = 0;
  if (millis() - tAutoAck > 1000) {
    tAutoAck = millis();
    alertsAutoAckTick();
  }

  // ESP-NOW presence beacon — every ~5s, lets peers discover us silently.
  // Skip under heap pressure: each hello round-trips through the ESP-NOW tx
  // path (mbedtls ctx, driver buffers) and we'd rather drop the discovery
  // ping than starve an inbound encrypted chat frame.
  static uint32_t tHello = 0;
  if (millis() - tHello > 5000 && !radioPausedForXfer()
      && ESP.getFreeHeap() > 12000) {
    tHello = millis();
    chatSendHello();
  }

  // Messaging health snapshot — every 10s. Shows channel, ok/fail, pending
  // state, and whether the send-callback is still firing. Offset from HB
  // so they don't stack up in one frame.
  static uint32_t tChatDiag = 500;
  if (millis() - tChatDiag > 10000) {
    tChatDiag = millis();
    chatDiagPrint();
  }

  // File transfer sender pump + timeout watchdog.
  fileXferTick();

  // Auto-pause/resume the promiscuous sniffer when heap swings across the
  // critical floor. Log shows `wifi:promis buf: out of memory` precedes panics
  // when detect runs at steady-state heap <8 KB.
  static uint32_t tDetHeap = 0;
  if (millis() - tDetHeap > 1000) {
    tDetHeap = millis();
    detectHeapTick();
    bleScanHeapTick();
  }

  delay(10);
}
