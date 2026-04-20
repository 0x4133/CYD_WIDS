// baseline.h
#pragma once
#include "wifi_survey.h"
#include "ble_scan.h"

enum BaselineState : uint8_t { BL_LEARN = 0, BL_MONITOR = 1 };

void baselineBegin();
void baselineTick(const WifiSeen* wifi, int nWifi,
                  const BleSeen*  ble,  int nBle);

BaselineState baselineStateNow();
uint32_t baselineMsRemaining();
uint32_t baselineLockMs();

bool baselineContainsWifi(const char* bssid);
bool baselineContainsBle (const char* mac);

int baselineWifiCount();
int baselineBleCount();

void baselineRelearn();
