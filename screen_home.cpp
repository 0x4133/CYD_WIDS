// screen_home.cpp — HOME is the alert dashboard.
#include "ui.h"
#include "config.h"
#include "alerts.h"
#include "baseline.h"
#include "radio_scheduler.h"
#include "touch.h"
#include "touch_cal.h"

#define HOME_ROWS   7
#define HOME_ROW_H  20
#define HDR_H       20

static int lastRowsDrawn = 0;
static int scrollOff    = 0;
static int selectedRow  = -1;  // visible row index [0..HOME_ROWS)
// Row/header text cache — suppresses repaint when nothing actually
// changed between 1.5s ticks (eliminates visible flicker).
static char lastHeader[64] = {0};
static char lastRowText[HOME_ROWS][64] = {{0}};

static uint16_t colorFor(AlertType t) {
  switch (t) {
    case ALERT_NEW_WIFI:    return TFT_YELLOW;
    case ALERT_NEW_BLE:     return TFT_GREEN;
    case ALERT_NEW_ESPNOW:  return TFT_CYAN;
    case ALERT_DEAUTH:
    case ALERT_DISASSOC:    return TFT_RED;
    case ALERT_PROBE_FLOOD: return TFT_MAGENTA;
    default:                return TFT_WHITE;
  }
}

static void drawHeader() {
  char buf[64];
  Alert all[ALERT_LOG_MAX];
  int n = alertSnapshot(all, ALERT_LOG_MAX);
  int unacked = 0;
  for (int i = 0; i < n; i++) if (!all[i].acked) unacked++;
  snprintf(buf, sizeof(buf), "ALERTS  total:%lu  open:%d",
    (unsigned long)alertTotalCount(), unacked);
  if (strcmp(buf, lastHeader) == 0) return;
  tft.fillRect(0, LIST_TOP, SCR_W, HDR_H, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, 4, LIST_TOP + HDR_H/2, 2);
  strlcpy(lastHeader, buf, sizeof(lastHeader));
}

static void drawRow(int row, const Alert& a) {
  int y = LIST_TOP + HDR_H + row * HOME_ROW_H;
  uint32_t ago = (millis() - a.tMs) / 1000;
  uint16_t bg = (row == selectedRow) ? TFT_NAVY : TFT_BLACK;
  char line[64];
  snprintf(line, sizeof(line),
    "%c%-8s %02X%02X:%02X %3lus %s",
    a.acked ? ' ' : '!',
    alertTypeName(a.type),
    a.mac[3], a.mac[4], a.mac[5],
    (unsigned long)ago, a.label);
  // trim to fit
  int l = strlen(line);
  while (l > 4 && tft.textWidth(line, 1) > SCR_W - 6) line[--l] = 0;
  // Key includes color so a type change (same text) still repaints.
  char key[64];
  snprintf(key, sizeof(key), "%d|%u|%s", (int)a.type, (unsigned)bg, line);
  if (row < HOME_ROWS && strcmp(key, lastRowText[row]) == 0) return;
  tft.fillRect(0, y, SCR_W, HOME_ROW_H, bg);
  uint16_t col = colorFor(a.type);
  tft.setTextColor(col, bg);
  tft.fillRect(0, y, SCR_W, HOME_ROW_H, TFT_BLACK);
  uint16_t col = a.acked ? TFT_DARKGREY : colorFor(a.type);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(line, 4, y + HOME_ROW_H/2, 1);
  if (row < HOME_ROWS) strlcpy(lastRowText[row], key, sizeof(lastRowText[row]));
}

void enterScreenHome() {
  uiDrawFooterButtons("CAL", "ACK", "BASE+");
  lastRowsDrawn = 0;
  scrollOff = 0;
  selectedRow = -1;
  lastHeader[0] = 0;
  memset(lastRowText, 0, sizeof(lastRowText));
}

void drawScreenHome() {
  Alert all[ALERT_LOG_MAX];
  int total = alertSnapshot(all, ALERT_LOG_MAX);
  if (scrollOff > max(0, total - HOME_ROWS)) scrollOff = max(0, total - HOME_ROWS);
  int shown = min(HOME_ROWS, total - scrollOff);
  if (shown <= 0) selectedRow = -1;
  else if (selectedRow >= shown) selectedRow = shown - 1;
  drawHeader(total);
  drawHeader();
  for (int i = 0; i < shown; i++) drawRow(i, all[scrollOff + i]);
  // clear rows freed up since last draw
  for (int i = shown; i < lastRowsDrawn; i++) {
    int y = LIST_TOP + HDR_H + i * HOME_ROW_H;
    tft.fillRect(0, y, SCR_W, HOME_ROW_H, TFT_BLACK);
    if (i < HOME_ROWS) lastRowText[i][0] = 0;
  }
  if (total == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(baselineStateNow() == BL_LEARN
                   ? "learning baseline..." : "no alerts — quiet",
                   SCR_W/2, LIST_TOP + HDR_H + 40, 2);
  }
  // Scroll bar on the right edge when the list overflows.
  if (total > HOME_ROWS) {
    const int barW = 10;
    const int barX = SCR_W - barW - 1;
    const int trackY = LIST_TOP + HDR_H;
    const int trackH = HOME_ROWS * HOME_ROW_H;
    tft.fillRect(barX, trackY, barW, trackH, 0x1082);
    tft.drawRect(barX, trackY, barW, trackH, TFT_DARKGREY);
    int thumbH = max(12, trackH * HOME_ROWS / total);
    int thumbY = trackY + (trackH - thumbH) * scrollOff / (total - HOME_ROWS);
    tft.fillRect(barX + 1, thumbY + 1, barW - 2, thumbH - 2, TFT_SILVER);
  }
  lastRowsDrawn = shown;
}

bool touchScreenHome(int sx, int sy) {
  int btn = uiFooterHit(sx, sy);
  if (btn == 0) {
    TouchCal tc;
    if (touchCalRunWizard(tc)) {
      touchSetCal(tc);
      touchCalSave(tc);
    }
    uiRedraw();
    return true;
  }
  if (btn == 1 || btn == 2) {
    Alert all[ALERT_LOG_MAX];
    int total = alertSnapshot(all, ALERT_LOG_MAX);
    int shown = min(HOME_ROWS, total - scrollOff);
    if (selectedRow >= 0 && selectedRow < shown) {
      const Alert& a = all[scrollOff + selectedRow];
      if (btn == 1) {
        alertAckOne(a);
      } else if (btn == 2) {
        if (baselineAddFromAlert(a)) alertAckOne(a);
      }
      uiRedraw();
      return true;
    }
  }

  // Tap a row to act on that alert:
  // left half => ACK, right half => add NEW-WIFI/NEW-BLE to baseline + ACK.
  const int rowTop = LIST_TOP + HDR_H;
  const int rowBot = rowTop + HOME_ROWS * HOME_ROW_H;
  const int barW = 10;
  const int barX = SCR_W - barW - 1;
  if (sy >= rowTop && sy < rowBot && sx < barX) {
    Alert all[ALERT_LOG_MAX];
    int total = alertSnapshot(all, ALERT_LOG_MAX);
    int shown = min(HOME_ROWS, total - scrollOff);
    int row = (sy - rowTop) / HOME_ROW_H;
    if (row >= 0 && row < shown) {
      int idx = scrollOff + row;
      if (sx < SCR_W / 2) {
        if (alertAckBySnapshotIndex(idx, "manual")) uiRedraw();
      } else {
        if (baselineAddFromAlert(all[idx])) {
          alertAckBySnapshotIndex(idx, "baseline");
          uiRedraw();
        }
      }
      return true;
    }
  }

  // Tap inside the scroll-bar area to page through alerts:
  // upper half scrolls toward newer, lower half toward older.
  const int trackY = LIST_TOP + HDR_H;
  const int trackH = HOME_ROWS * HOME_ROW_H;
  Alert probe[ALERT_LOG_MAX];
  int total = alertSnapshot(probe, ALERT_LOG_MAX);
  if (total > HOME_ROWS && sx >= barX && sy >= trackY && sy < trackY + trackH) {
    if (sy < trackY + trackH/2) scrollOff = max(0, scrollOff - HOME_ROWS);
    else                        scrollOff = min(total - HOME_ROWS, scrollOff + HOME_ROWS);
    selectedRow = -1;
    uiRedraw();
    return true;
  }
  const int rowTop = LIST_TOP + HDR_H;
  const int rowBottom = rowTop + HOME_ROWS * HOME_ROW_H;
  if (sy >= rowTop && sy < rowBottom) {
    int rel = (sy - rowTop) / HOME_ROW_H;
    int shown = min(HOME_ROWS, total - scrollOff);
    if (rel >= 0 && rel < shown) {
      selectedRow = rel;
      uiRedraw();
      return true;
    }
  }
  return false;
}
