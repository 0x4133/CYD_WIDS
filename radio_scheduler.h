// radio_scheduler.h
#pragma once
#include "wifi_survey.h"
#include "ble_scan.h"

void radioBegin();

// Copy current live view under mutex. Returns count copied.
int radioSnapshotWifi(WifiSeen* out, int maxOut);
int radioSnapshotBle (BleSeen*  out, int maxOut);

uint32_t radioLastWifiScanMs();
uint32_t radioLastBleSeenMs();

// File-transfer quiescing: while paused, radio task skips WiFi scans and
// BLE drain so ESP-NOW gets full airtime. HELLO beacon should also be
// suppressed by callers while paused. Idempotent.
void radioPauseForXfer();
void radioResumeAfterXfer();
bool radioPausedForXfer();

// Focus quiescing: UI-driven. While the user sits on the chat / files /
// contacts screens, pause scans so ESP-NOW traffic has the radio. Composes
// with the xfer pause: scans resume only when both are clear.
void radioPauseForFocus();
void radioResumeAfterFocus();
bool radioPausedForFocus();
