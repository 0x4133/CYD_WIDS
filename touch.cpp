// touch.cpp
#include "touch.h"
#include "config.h"
#include <XPT2046_Bitbang.h>
#include <math.h>

static XPT2046_Bitbang ts(TOUCH_MOSI_PIN, TOUCH_MISO_PIN, TOUCH_CLK_PIN, TOUCH_CS_PIN);

// Defaults match the old working (but biased) mapping: sx from yRaw, sy from
// inverted xRaw. The wizard replaces these with a real affine fit.
static TouchCal cal = {
  0.0f,     0.0899f,  -21.6f,
  -0.0649f, 0.0f,      253.0f,
};
static const int Z_THRESH = 300;

void touchBegin() { ts.begin(); }

bool touchReadRaw(int& rx, int& ry, int& rz) {
  TouchPoint p = ts.getTouch();
  rx = p.xRaw; ry = p.yRaw; rz = p.zRaw;
  return p.zRaw > 0;
}

bool touchRead(int& sx, int& sy) {
  TouchPoint p = ts.getTouch();
  // Log only on real touch events, plus a heartbeat once every 5s so we can
  // tell the controller is alive. Previously this printed every 500ms whether
  // touched or not, drowning the serial buffer in "x=0 y=0 z=0" lines.
  static uint32_t tLastIdle = 0;
  if (p.zRaw >= Z_THRESH) {
    Serial.printf("[TRAW] x=%d y=%d z=%d\n", p.xRaw, p.yRaw, p.zRaw);
  } else if (millis() - tLastIdle > 5000) {
    tLastIdle = millis();
    Serial.printf("[TRAW] idle z=%d\n", p.zRaw);
  }
  if (p.zRaw < Z_THRESH) return false;
  float fx = cal.ax * p.xRaw + cal.bx * p.yRaw + cal.cx;
  float fy = cal.ay * p.xRaw + cal.by * p.yRaw + cal.cy;
  sx = constrain((int)lroundf(fx), 0, SCR_W - 1);
  sy = constrain((int)lroundf(fy), 0, SCR_H - 1);
  return true;
}

void touchSetCal(const TouchCal& c) { cal = c; }
TouchCal touchGetCal() { return cal; }
