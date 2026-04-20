// detect.h
#pragma once
#include <Arduino.h>

// Unified 802.11 management-frame watcher. Runs as the promiscuous RX
// callback and:
//   - parses deauth / disassoc / probe-request subtypes
//   - maintains rolling per-MAC counters
//   - raises alerts when thresholds trip
//   - forwards vendor-action frames to espnowIngestFrame() for ESP-NOW sniff
void detectBegin();
void detectEnable(bool on);   // user-visible on/off; persistent across scans
bool detectEnabled();

// Internal hooks used by the radio scheduler to briefly drop promiscuous
// during an STA scan. Do NOT clear the user-visible `enabled` flag —
// detectResume() restores the underlying radio state from it.
void detectPause();
void detectResume();

// Heap-guard tick. Call periodically from loop(). When the user asked for
// detect-on but free heap falls below the critical floor, the promiscuous
// driver is paused (user flag preserved); when heap climbs back over the
// resume threshold, it's re-enabled. Prevents the WiFi driver from panicking
// on its internal `promis buf: out of memory` path.
void detectHeapTick();
