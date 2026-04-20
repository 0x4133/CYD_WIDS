// ui.cpp
#include "ui.h"
#include "config.h"
#include "alerts.h"
#include "radio_scheduler.h"
#include "baseline.h"
#include "sd_writer.h"

#define ALERT_FLASH_MS 3000

TFT_eSPI tft = TFT_eSPI();
static UiScreen cur = UI_HOME;
static struct { int x, y, w, h; } fBtn[3];

// Forward declarations — defined in screen_*.cpp
void drawScreenHome();      void enterScreenHome();      bool touchScreenHome(int, int);
void drawScreenWifi();      void enterScreenWifi();      bool touchScreenWifi(int, int);
void drawScreenBle();       void enterScreenBle();       bool touchScreenBle(int, int);
void drawScreenEspnow();    void enterScreenEspnow();    bool touchScreenEspnow(int, int);
void drawScreenFiles();     void enterScreenFiles();     bool touchScreenFiles(int, int);
void drawScreenContacts();  void enterScreenContacts();  bool touchScreenContacts(int, int);
void drawScreenFileView();  void enterScreenFileView();  bool touchScreenFileView(int, int);
bool xferOverlayVisible();
bool xferOverlayModal();
void xferOverlayDraw();
int  xferOverlayTouch(int, int);

void uiBegin() {
  pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, HIGH);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
}

UiScreen uiCurrent() { return cur; }

// Status strip dirty-cache: skip repaint when the composed line and bg
// haven't changed. Without this, the 1.5s refresh flashes the strip
// (fillRect → drawString) even when nothing is new.
static char     statusLast[96] = {0};
static uint16_t statusLastBg   = 0xFFFF;
static void invalidateStatusCache() {
  statusLast[0] = 0;
  statusLastBg  = 0xFFFF;
}

static bool screenIsComms(UiScreen s) {
  // Comms screens own the radio: while the user is here, scans are paused so
  // chat + file transfers run on a stable, channel-pinned link.
  return s == UI_ESPNOW || s == UI_FILES || s == UI_CONTACTS;
}

void uiGoto(UiScreen s) {
  bool wasComms = screenIsComms(cur);
  bool nowComms = screenIsComms(s);
  cur = s;
  if (nowComms && !wasComms)      radioPauseForFocus();
  else if (wasComms && !nowComms) radioResumeAfterFocus();
  tft.fillScreen(TFT_BLACK);   // clear only on screen change
  invalidateStatusCache();      // screen-level clear invalidates strip cache
  uiDrawTabs();                 // tabs are static between screen changes
  switch (cur) {                // per-screen static chrome (footer, etc.)
    case UI_HOME:     enterScreenHome();     break;
    case UI_WIFI:     enterScreenWifi();     break;
    case UI_BLE:      enterScreenBle();      break;
    case UI_ESPNOW:   enterScreenEspnow();   break;
    case UI_FILES:    enterScreenFiles();    break;
    case UI_CONTACTS:  enterScreenContacts(); break;
    case UI_FILE_VIEW: enterScreenFileView(); break;
  }
  uiRedraw();
}

static void tabIconHome(int cx, int cy, uint16_t c) {
  // roof (triangle) + body (rect)
  tft.drawLine(cx - 7, cy + 1, cx,     cy - 6, c);
  tft.drawLine(cx,     cy - 6, cx + 7, cy + 1, c);
  tft.drawRect(cx - 5, cy + 1, 11, 7, c);
  tft.drawRect(cx - 1, cy + 4, 3, 4, c);  // door
}
static void tabIconWifi(int cx, int cy, uint16_t c) {
  // dot + 3 arcs opening downward (signal rising)
  int by = cy + 6;
  tft.fillCircle(cx, by, 1, c);
  for (int r = 3; r <= 9; r += 3) tft.drawCircleHelper(cx, by, r, 0x9, c);
}
static void tabIconBle(int cx, int cy, uint16_t c) {
  // stylized B / bluetooth bowtie (spine + two crossed lines)
  tft.drawLine(cx, cy - 7, cx,     cy + 7, c);
  tft.drawLine(cx, cy - 7, cx + 5, cy - 2, c);
  tft.drawLine(cx, cy + 7, cx + 5, cy + 2, c);
  tft.drawLine(cx - 5, cy - 3, cx + 5, cy + 3, c);
  tft.drawLine(cx - 5, cy + 3, cx + 5, cy - 3, c);
}
static void tabIconEspnow(int cx, int cy, uint16_t c) {
  // broadcast: center dot + 4 diagonal rays
  tft.fillCircle(cx, cy, 2, c);
  tft.drawLine(cx - 7, cy - 7, cx - 3, cy - 3, c);
  tft.drawLine(cx + 7, cy - 7, cx + 3, cy - 3, c);
  tft.drawLine(cx - 7, cy + 7, cx - 3, cy + 3, c);
  tft.drawLine(cx + 7, cy + 7, cx + 3, cy + 3, c);
  // a couple of outer arcs for "waves"
  tft.drawCircleHelper(cx, cy,  6, 0xF, c);
}
static void tabIconFiles(int cx, int cy, uint16_t c) {
  // folder: tab on top, body below
  tft.drawRect(cx - 7, cy - 5, 6, 3, c);
  tft.drawRect(cx - 8, cy - 3, 16, 10, c);
}

void uiDrawTabs() {
  typedef void (*IconFn)(int, int, uint16_t);
  const IconFn icons[UI_TAB_COUNT] = {
    tabIconHome, tabIconWifi, tabIconBle, tabIconEspnow, tabIconFiles,
  };
  int w = SCR_W / UI_TAB_COUNT;
  for (int i = 0; i < UI_TAB_COUNT; i++) {
    bool active = (cur == i);
    uint16_t bg = active ? TFT_NAVY : 0x2104;
    uint16_t fg = active ? TFT_WHITE : TFT_SILVER;
    tft.fillRect(i*w, 0, w, TAB_H, bg);
    icons[i](i*w + w/2, TAB_H/2, fg);
  }
  tft.drawFastHLine(0, TAB_H - 1, SCR_W, TFT_DARKGREY);
}

void uiDrawStatusStrip() {
  bool learning = baselineStateNow() == BL_LEARN;
  uint32_t since = millis() - alertLastMs();
  bool flash = alertLastMs() != 0 && since < ALERT_FLASH_MS;
  uint16_t bg;
  if (flash)         bg = TFT_RED;
  else if (learning) bg = TFT_DARKGREEN;
  else               bg = TFT_MAROON;
  char buf[80];
  if (flash) {
    snprintf(buf, sizeof(buf),
      "! ALERT  total:%lu  (%lus ago)",
      (unsigned long)alertTotalCount(),
      (unsigned long)(since/1000));
  } else if (learning) {
    uint32_t s = baselineMsRemaining() / 1000;
    snprintf(buf, sizeof(buf),
      "LEARN %02lu:%02lu  bl:%d/%d  alerts:%lu  drop:%lu",
      s/60, s%60, baselineWifiCount(), baselineBleCount(),
      (unsigned long)alertTotalCount(),
      (unsigned long)sdWriterDropped());
  } else {
    snprintf(buf, sizeof(buf),
      "MONITOR  bl:%d/%d  alerts:%lu  SD:%s  drop:%lu",
      baselineWifiCount(), baselineBleCount(),
      (unsigned long)alertTotalCount(),
      sdWriterReady() ? "ok" : "none",
      (unsigned long)sdWriterDropped());
  }
  if (bg == statusLastBg && strcmp(buf, statusLast) == 0) return;
  tft.fillRect(0, TAB_H, SCR_W, STATUS_H, bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, 4, TAB_H + STATUS_H/2, 1);
  strlcpy(statusLast, buf, sizeof(statusLast));
  statusLastBg = bg;
}

void uiDrawFooterButtons(const char* b0, const char* b1, const char* b2) {
  tft.fillRect(0, SCR_H - FOOTER_H, SCR_W, FOOTER_H, 0x2104);
  const char* lbls[3] = { b0, b1, b2 };
  int bw = 60, bh = 26, gap = 4, pad = 4;
  int y = SCR_H - FOOTER_H + (FOOTER_H - bh) / 2;
  for (int i = 0; i < 3; i++) {
    if (!lbls[i] || !lbls[i][0]) { fBtn[i] = {0,0,0,0}; continue; }
    int x = SCR_W - pad - (i+1) * bw - i * gap;
    fBtn[i] = { x, y, bw, bh };
    tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREEN);
    tft.drawRoundRect(x, y, bw, bh, 4, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(lbls[i], x + bw/2, y + bh/2, 1);
  }
}

int uiFooterHit(int sx, int sy) {
  for (int i = 0; i < 3; i++) {
    if (fBtn[i].w == 0) continue;
    if (sx >= fBtn[i].x && sx < fBtn[i].x + fBtn[i].w &&
        sy >= fBtn[i].y && sy < fBtn[i].y + fBtn[i].h) return i;
  }
  return -1;
}

void uiRedraw() {
  uiDrawStatusStrip();
  switch (cur) {
    case UI_HOME:     drawScreenHome();     break;
    case UI_WIFI:     drawScreenWifi();     break;
    case UI_BLE:      drawScreenBle();      break;
    case UI_ESPNOW:   drawScreenEspnow();   break;
    case UI_FILES:    drawScreenFiles();    break;
    case UI_CONTACTS:  drawScreenContacts(); break;
    case UI_FILE_VIEW: drawScreenFileView(); break;
  }
  if (xferOverlayVisible()) xferOverlayDraw();
}

void uiOnTouch(int sx, int sy) {
  // File-transfer overlay gets first crack. Modal offer swallows everything
  // except the ACCEPT/DECLINE buttons; the active-transfer bar only catches
  // taps on CANCEL and lets the rest fall through to the underlying screen.
  int ov = xferOverlayTouch(sx, sy);
  if (ov == 1) return;
  if (ov == 0) { uiRedraw(); return; }

  // Hit-region is taller than the drawn tab row because top-edge raw
  // values from the XPT2046 can't quite reach y=0 in practice.
  if (sy < TAB_H + 12) {
    int w = SCR_W / UI_TAB_COUNT;
    int idx = sx / w;
    if (idx < UI_TAB_COUNT) uiGoto((UiScreen)idx);
    return;
  }
  switch (cur) {
    case UI_HOME:      if (touchScreenHome    (sx, sy)) return; break;
    case UI_WIFI:      if (touchScreenWifi    (sx, sy)) return; break;
    case UI_BLE:       if (touchScreenBle     (sx, sy)) return; break;
    case UI_ESPNOW:    if (touchScreenEspnow  (sx, sy)) return; break;
    case UI_FILES:     if (touchScreenFiles   (sx, sy)) return; break;
    case UI_CONTACTS:  if (touchScreenContacts(sx, sy)) return; break;
    case UI_FILE_VIEW: if (touchScreenFileView(sx, sy)) return; break;
  }
}
