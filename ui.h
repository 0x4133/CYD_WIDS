// ui.h
#pragma once
#include <TFT_eSPI.h>

// Tab-accessible screens (indices used as tab slots)
enum UiScreen : uint8_t {
  UI_HOME = 0, UI_WIFI = 1, UI_BLE = 2, UI_ESPNOW = 3, UI_FILES = 4,
  // Non-tab screens (reached from footer buttons, not in the tab row):
  UI_CONTACTS  = 5,
  UI_FILE_VIEW = 6,   // text viewer reached from Files tab
};
#define UI_TAB_COUNT 5

// Enter file-pick mode: Files tab will forward the chosen file to
// fileXferStartSend(targetMac,...) and return to UI_CONTACTS.
void filesEnterPickMode(const uint8_t targetMac[6], const char* targetName);
// Prime the text viewer with a path before calling uiGoto(UI_FILE_VIEW).
void fileViewSetPath(const char* path);

extern TFT_eSPI tft;
void uiBegin();
void uiRedraw();
void uiOnTouch(int sx, int sy);

UiScreen uiCurrent();
void uiGoto(UiScreen s);

void uiDrawTabs();
void uiDrawStatusStrip();
void uiDrawFooterButtons(const char* b0, const char* b1, const char* b2);
int  uiFooterHit(int sx, int sy);
