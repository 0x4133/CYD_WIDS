// screen_ble.cpp
#include "ui.h"
#include "config.h"
#include "radio_scheduler.h"
#include "baseline.h"

static int scrollOff = 0;
static int lastRowsDrawn = 0;
static char lastRowText[LIST_ROWS][96] = {{0}};

static void drawRow(int slot, int y, const char* title, const char* sub, bool baselineHit) {
  char key[96];
  snprintf(key, sizeof(key), "%d|%s|%s", (int)baselineHit, title, sub);
  if (slot < LIST_ROWS && strcmp(key, lastRowText[slot]) == 0) return;
  tft.fillRect(0, y, SCR_W, ROW_H, TFT_BLACK);
  tft.drawFastHLine(0, y + ROW_H - 1, SCR_W, TFT_DARKGREY);
  uint16_t col = baselineHit ? TFT_WHITE : TFT_YELLOW;
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, 6, y + ROW_H/2 - 6, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(sub,   6, y + ROW_H/2 + 6, 1);
  if (slot < LIST_ROWS) strlcpy(lastRowText[slot], key, sizeof(lastRowText[slot]));
}

void enterScreenBle() {
  uiDrawFooterButtons("HOME", "UP", "DN");
  lastRowsDrawn = 0;
  memset(lastRowText, 0, sizeof(lastRowText));
}

void drawScreenBle() {
  static BleSeen b[MAX_BLE];
  int n = radioSnapshotBle(b, MAX_BLE);
  int first = scrollOff;
  int last  = min(n, first + LIST_ROWS);
  int rowsNow = last - first;
  if (n == 0) {
    if (lastRowText[0][0] != '~') {
      tft.fillRect(0, LIST_TOP, SCR_W, LIST_BOT - LIST_TOP, TFT_BLACK);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("scanning...", SCR_W/2, LIST_TOP + 40, 2);
      lastRowText[0][0] = '~';
    }
    lastRowsDrawn = 0;
    return;
  }
  for (int i = first; i < last; i++) {
    int slot = i - first;
    int y = LIST_TOP + slot * ROW_H;
    char title[64];
    snprintf(title, sizeof(title), "%s (%d)", b[i].name, b[i].rssi);
    drawRow(slot, y, title, b[i].mac, baselineContainsBle(b[i].mac));
  }
  for (int i = rowsNow; i < lastRowsDrawn; i++) {
    int y = LIST_TOP + i * ROW_H;
    tft.fillRect(0, y, SCR_W, ROW_H, TFT_BLACK);
    if (i < LIST_ROWS) lastRowText[i][0] = 0;
  }
  lastRowsDrawn = rowsNow;
}

bool touchScreenBle(int sx, int sy) {
  int btn = uiFooterHit(sx, sy);
  if (btn == 0) { uiGoto(UI_HOME); return true; }
  if (btn == 1) { if (scrollOff > 0) scrollOff--; uiRedraw(); return true; }
  if (btn == 2) { scrollOff++; uiRedraw(); return true; }
  return false;
}
