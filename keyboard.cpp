// keyboard.cpp
#include "keyboard.h"
#include "touch.h"
#include "ui.h"
#include "config.h"

// Sentinel chars in the key table for non-printable keys.
#define K_SHIFT  '\x01'
#define K_SPACE  '\x02'
#define K_BKSP   '\x03'
#define K_OK     '\x04'
#define K_CANCEL '\x05'

struct Key { int x, y, w, h; char c; const char* label; };

static const char* ROWS[4] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm,.?",
};

static bool shift = false;
static Key keys[64];
static int keyN = 0;

static char shiftChar(char c) {
  if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
  return c;
}

static void buildKeys() {
  keyN = 0;
  const int kw = 28, kh = 28, gap = 2;
  // letter/digit rows
  int yTop[4] = { 46, 76, 106, 136 };
  for (int r = 0; r < 4; r++) {
    int n = strlen(ROWS[r]);
    int rowW = n * kw + (n - 1) * gap;
    int x0 = (SCR_W - rowW) / 2;
    for (int i = 0; i < n; i++) {
      keys[keyN++] = { x0 + i * (kw + gap), yTop[r], kw, kh, ROWS[r][i], nullptr };
    }
  }
  // action row: SHIFT | SPACE | BKSP
  int ay = 168, ah = 26;
  keys[keyN++] = {  10, ay,  60, ah, K_SHIFT, "SHIFT" };
  keys[keyN++] = {  76, ay, 168, ah, K_SPACE, "SPACE" };
  keys[keyN++] = { 250, ay,  60, ah, K_BKSP,  "BKSP"  };
  // bottom: CANCEL | OK
  int by = 200, bh = 30;
  keys[keyN++] = {  10, by, 145, bh, K_CANCEL, "CANCEL" };
  keys[keyN++] = { 165, by, 145, bh, K_OK,     "OK"     };
}

static void drawKey(const Key& k, bool pressed) {
  uint16_t bg, fg;
  if (k.c == K_OK)           { bg = TFT_DARKGREEN; fg = TFT_WHITE; }
  else if (k.c == K_CANCEL)  { bg = TFT_MAROON;    fg = TFT_WHITE; }
  else if (k.c == K_SHIFT)   { bg = shift ? TFT_NAVY : 0x2945; fg = TFT_WHITE; }
  else                       { bg = pressed ? TFT_NAVY : 0x3186; fg = TFT_WHITE; }
  tft.fillRoundRect(k.x, k.y, k.w, k.h, 3, bg);
  tft.drawRoundRect(k.x, k.y, k.w, k.h, 3, TFT_DARKGREY);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  char buf[3] = {0};
  const char* s = k.label;
  if (!s) {
    buf[0] = shift ? shiftChar(k.c) : k.c;
    s = buf;
  }
  tft.drawString(s, k.x + k.w/2, k.y + k.h/2, 2);
}

static void drawAllKeys() {
  for (int i = 0; i < keyN; i++) drawKey(keys[i], false);
}

static void drawDraft(const char* title, const char* buf) {
  tft.fillRect(0, 0, SCR_W, 44, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(title, 4, 2, 1);
  tft.fillRect(0, 16, SCR_W, 26, 0x1082);
  tft.drawRect(0, 16, SCR_W, 26, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.setTextDatum(ML_DATUM);
  // scroll long drafts: show tail that fits
  const char* show = buf;
  int len = strlen(buf);
  if (len > 38) show = buf + (len - 38);
  tft.drawString(show, 6, 16 + 13, 2);
  // blinking cursor
  int cx = 6 + tft.textWidth(show, 2);
  if (cx > SCR_W - 8) cx = SCR_W - 8;
  tft.drawFastVLine(cx, 20, 18, TFT_WHITE);
}

static int hitKey(int sx, int sy) {
  for (int i = 0; i < keyN; i++) {
    if (sx >= keys[i].x && sx < keys[i].x + keys[i].w &&
        sy >= keys[i].y && sy < keys[i].y + keys[i].h) return i;
  }
  return -1;
}

bool keyboardPrompt(const char* title, char* out, int outSize) {
  if (!out || outSize < 1) return false;
  char buf[CHAT_MSG_MAX + 1];
  int maxLen = min((int)sizeof(buf) - 1, outSize - 1);
  strncpy(buf, out, maxLen);
  buf[maxLen] = 0;
  int len = strlen(buf);

  shift = false;
  buildKeys();

  tft.fillScreen(TFT_BLACK);
  drawDraft(title, buf);
  drawAllKeys();

  uint32_t lastTap = 0;
  for (;;) {
    int tx, ty;
    if (touchRead(tx, ty)) {
      if (millis() - lastTap < 180) { delay(10); continue; }
      lastTap = millis();
      int ki = hitKey(tx, ty);
      if (ki < 0) continue;
      Key& k = keys[ki];
      // brief press highlight
      drawKey(k, true);
      delay(40);

      if (k.c == K_OK) {
        strncpy(out, buf, outSize - 1);
        out[outSize - 1] = 0;
        return true;
      }
      if (k.c == K_CANCEL) return false;
      if (k.c == K_BKSP) {
        if (len > 0) { buf[--len] = 0; drawDraft(title, buf); }
      } else if (k.c == K_SHIFT) {
        shift = !shift;
        drawAllKeys();  // re-label letters
      } else if (k.c == K_SPACE) {
        if (len < maxLen) { buf[len++] = ' '; buf[len] = 0; drawDraft(title, buf); }
      } else {
        if (len < maxLen) {
          char ch = shift ? shiftChar(k.c) : k.c;
          buf[len++] = ch; buf[len] = 0;
          drawDraft(title, buf);
          if (shift) { shift = false; drawAllKeys(); }
        }
      }
      drawKey(k, false);
    }
    delay(10);
  }
}
