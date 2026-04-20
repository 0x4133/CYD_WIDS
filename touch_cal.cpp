// touch_cal.cpp
#include "touch_cal.h"
#include "touch.h"
#include "config.h"
#include "ui.h"
#include <SD.h>
#include <math.h>

static const char* CAL_PATH = "/touch_cal.bin";
static const char  MAGIC[4] = { 'T','C','A','L' };
static const uint8_t VERSION = 2;  // v1 was 4-int16; v2 is 6-float affine

#pragma pack(push, 1)
struct FileRec {
  char magic[4];
  uint8_t version;
  uint8_t _pad;
  float ax, bx, cx;
  float ay, by, cy;
  uint16_t crc;
};
#pragma pack(pop)

static uint16_t xorCrc(const uint8_t* p, size_t n) {
  uint16_t c = 0xA5A5;
  for (size_t i = 0; i < n; i++) c = (uint16_t)((c << 1) ^ p[i]);
  return c;
}

bool touchCalLoad(TouchCal& out) {
  if (!SD.exists(CAL_PATH)) return false;
  File f = SD.open(CAL_PATH, FILE_READ);
  if (!f) return false;
  FileRec r;
  int got = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (got != (int)sizeof(r)) return false;
  if (memcmp(r.magic, MAGIC, 4) != 0) return false;
  if (r.version != VERSION) return false;
  uint16_t want = xorCrc((const uint8_t*)&r, sizeof(r) - 2);
  if (r.crc != want) return false;
  out.ax = r.ax; out.bx = r.bx; out.cx = r.cx;
  out.ay = r.ay; out.by = r.by; out.cy = r.cy;
  Serial.printf("[CAL] loaded ax=%.4f bx=%.4f cx=%.1f | ay=%.4f by=%.4f cy=%.1f\n",
    out.ax, out.bx, out.cx, out.ay, out.by, out.cy);
  return true;
}

bool touchCalSave(const TouchCal& cal) {
  FileRec r;
  memcpy(r.magic, MAGIC, 4);
  r.version = VERSION;
  r._pad = 0;
  r.ax = cal.ax; r.bx = cal.bx; r.cx = cal.cx;
  r.ay = cal.ay; r.by = cal.by; r.cy = cal.cy;
  r.crc = xorCrc((const uint8_t*)&r, sizeof(r) - 2);
  SD.remove(CAL_PATH);
  File f = SD.open(CAL_PATH, FILE_WRITE);
  if (!f) { Serial.println("[CAL] save: open failed"); return false; }
  int wrote = f.write((const uint8_t*)&r, sizeof(r));
  f.close();
  bool ok = (wrote == (int)sizeof(r));
  Serial.printf("[CAL] save %s\n", ok ? "ok" : "fail");
  return ok;
}

static void drawCross(int x, int y, uint16_t col) {
  tft.drawFastHLine(x - 12, y, 24, col);
  tft.drawFastVLine(x, y - 12, 24, col);
  tft.drawCircle(x, y, 6, col);
}

static bool waitForTap(int& rxOut, int& ryOut) {
  int rx, ry, rz;
  uint32_t tStart = millis();
  while (!(touchReadRaw(rx, ry, rz) && rz >= 400)) {
    delay(20);
    if (millis() - tStart > 30000) return false;
  }
  int sumX = 0, sumY = 0, n = 0;
  while (touchReadRaw(rx, ry, rz) && rz >= 300 && n < 40) {
    sumX += rx; sumY += ry; n++;
    delay(10);
  }
  if (n == 0) return false;
  rxOut = sumX / n; ryOut = sumY / n;
  // wait for release
  while (touchReadRaw(rx, ry, rz) && rz >= 200) delay(20);
  delay(150);
  return true;
}

// Solve 3x3 linear system for affine coefficients of one screen axis.
static bool solveAxis(float x0, float y0, float x1, float y1, float x2, float y2,
                      float s0, float s1, float s2,
                      float& a, float& b, float& c) {
  float det = x0 * (y1 - y2) - y0 * (x1 - x2) + (x1 * y2 - x2 * y1);
  if (fabsf(det) < 1.0f) return false;  // degenerate or near-colinear points
  a = (s0 * (y1 - y2) - y0 * (s1 - s2) + (s1 * y2 - s2 * y1)) / det;
  b = (x0 * (s1 - s2) - s0 * (x1 - x2) + (x1 * s2 - x2 * s1)) / det;
  c = (x0 * (y1 * s2 - s1 * y2) - y0 * (x1 * s2 - s1 * x2)
       + s0 * (x1 * y2 - y1 * x2)) / det;
  return true;
}

bool touchCalRunWizard(TouchCal& out) {
  const int m = 28;
  struct Pt { int sx, sy; int rx, ry; };
  Pt pts[3] = {
    { m,           m,           0, 0 },
    { SCR_W - m,   m,           0, 0 },
    { SCR_W - m,   SCR_H - m,   0, 0 },
  };

  TouchCal prev = touchGetCal();

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch Calibration", SCR_W/2, 30, 2);
  tft.drawString("Tap each crosshair", SCR_W/2, 56, 2);

  for (int i = 0; i < 3; i++) {
    drawCross(pts[i].sx, pts[i].sy, TFT_YELLOW);
    if (!waitForTap(pts[i].rx, pts[i].ry)) {
      Serial.printf("[CAL] pt%d timeout\n", i);
      out = prev;
      return false;
    }
    Serial.printf("[CAL] pt%d sx=%d sy=%d rx=%d ry=%d\n",
      i, pts[i].sx, pts[i].sy, pts[i].rx, pts[i].ry);
    drawCross(pts[i].sx, pts[i].sy, TFT_GREEN);
  }

  TouchCal c;
  bool okX = solveAxis(pts[0].rx, pts[0].ry, pts[1].rx, pts[1].ry, pts[2].rx, pts[2].ry,
                       pts[0].sx, pts[1].sx, pts[2].sx, c.ax, c.bx, c.cx);
  bool okY = solveAxis(pts[0].rx, pts[0].ry, pts[1].rx, pts[1].ry, pts[2].rx, pts[2].ry,
                       pts[0].sy, pts[1].sy, pts[2].sy, c.ay, c.by, c.cy);
  if (!okX || !okY) {
    Serial.println("[CAL] degenerate point set");
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Points too close", SCR_W/2, SCR_H/2, 2);
    delay(1200);
    out = prev;
    return false;
  }
  Serial.printf("[CAL] fit ax=%.4f bx=%.4f cx=%.1f | ay=%.4f by=%.4f cy=%.1f\n",
    c.ax, c.bx, c.cx, c.ay, c.by, c.cy);

  // Apply temporarily and verify with a center tap.
  touchSetCal(c);
  tft.fillScreen(TFT_BLACK);
  tft.drawString("Tap the target to verify", SCR_W/2, 30, 2);
  int vx = SCR_W / 2, vy = SCR_H / 2;
  drawCross(vx, vy, TFT_CYAN);

  int tx, ty;
  uint32_t tStart = millis();
  bool hit = false, gotAny = false;
  int landX = -1, landY = -1;
  while (millis() - tStart < 10000) {
    if (touchRead(tx, ty)) {
      int hx = tx, hy = ty;
      while (touchRead(tx, ty)) { hx = tx; hy = ty; delay(20); }
      landX = hx; landY = hy;
      gotAny = true;
      int dx = hx - vx, dy = hy - vy;
      if (dx*dx + dy*dy < 30*30) { hit = true; break; }
      break;
    }
    delay(20);
  }

  if (!hit) {
    Serial.printf("[CAL] verify %s landed=(%d,%d) target=(%d,%d)\n",
      gotAny ? "miss" : "timeout", landX, landY, vx, vy);
    touchSetCal(prev);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Verify failed", SCR_W/2, SCR_H/2 - 10, 2);
    tft.drawString("Keeping previous cal", SCR_W/2, SCR_H/2 + 12, 2);
    delay(1400);
    out = prev;
    return false;
  }

  Serial.println("[CAL] verify ok");
  tft.fillScreen(TFT_BLACK);
  tft.drawString("Calibration OK", SCR_W/2, SCR_H/2, 2);
  delay(700);
  out = c;
  return true;
}
