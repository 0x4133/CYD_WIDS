// screen_file_view.cpp — minimal text reader for .txt/.log/.csv/.md.
// Reads up to VIEW_MAX bytes into RAM, hard-wraps at VIEW_COLS columns, then
// lets the user scroll by page with UP/DN. BACK returns to Files tab.
#include "ui.h"
#include "config.h"
#include <SD.h>

#define VIEW_MAX       8192     // cap on bytes loaded; truncates larger files
#define VIEW_COLS      38
#define VIEW_ROWS_MAX  96       // wrapped-line slots (≈ 3.8KB at 38 cols)
#define VIEW_ROW_H     12

static char    path[96] = {0};
static char    buf[VIEW_MAX + 1] = {0};
static uint16_t rowStart[VIEW_ROWS_MAX]; // byte offset of each wrapped row
static int     rowCount = 0;
static int     scrollOff = 0;
static int     visible   = 0;
static bool    truncated = false;
static int     lastScrollDraw = -1;   // dirty marker for content
static int     lastRowCountDraw = -1;

static void reflow() {
  rowCount = 0;
  int n = strlen(buf);
  int i = 0;
  while (i < n && rowCount < VIEW_ROWS_MAX) {
    rowStart[rowCount++] = (uint16_t)i;
    int lineEnd = i;
    int colsThisLine = 0;
    while (lineEnd < n && buf[lineEnd] != '\n' && colsThisLine < VIEW_COLS) {
      lineEnd++; colsThisLine++;
    }
    if (lineEnd < n && buf[lineEnd] == '\n') { i = lineEnd + 1; }
    else                                      { i = lineEnd; }
  }
}

void fileViewSetPath(const char* p) {
  strlcpy(path, p ? p : "", sizeof(path));
  buf[0] = 0;
  rowCount = 0;
  scrollOff = 0;
  truncated = false;
  lastScrollDraw   = -1;
  lastRowCountDraw = -1;
  if (!path[0]) return;
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  int n = f.read((uint8_t*)buf, VIEW_MAX);
  truncated = (f.available() > 0);
  f.close();
  if (n < 0) n = 0;
  buf[n] = 0;
  // Sanitize control bytes except newline/tab so drawString doesn't choke.
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (c != '\n' && c != '\t' && (c < 32 || c > 126)) buf[i] = '.';
  }
  reflow();
}

void enterScreenFileView() {
  uiDrawFooterButtons("BACK", "UP", "DN");
  visible = (LIST_BOT - LIST_TOP) / VIEW_ROW_H;
  scrollOff = 0;
  lastScrollDraw   = -1;
  lastRowCountDraw = -1;
}

void drawScreenFileView() {
  // Content only changes on scroll / new file — skip when unchanged.
  if (scrollOff == lastScrollDraw && rowCount == lastRowCountDraw) return;
  lastScrollDraw   = scrollOff;
  lastRowCountDraw = rowCount;

  tft.fillRect(0, LIST_TOP, SCR_W, LIST_BOT - LIST_TOP, TFT_BLACK);
  tft.setTextColor(TFT_SILVER, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  if (!path[0] || rowCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("(empty or unreadable)", SCR_W/2, LIST_TOP + 40, 2);
    return;
  }
  int shown = min(visible, rowCount - scrollOff);
  char slice[VIEW_COLS + 1];
  for (int i = 0; i < shown; i++) {
    int row = scrollOff + i;
    int start = rowStart[row];
    int end   = (row + 1 < rowCount) ? rowStart[row + 1] : (int)strlen(buf);
    if (end > start && buf[end - 1] == '\n') end--;
    int len = end - start;
    if (len > VIEW_COLS) len = VIEW_COLS;
    memcpy(slice, buf + start, len);
    slice[len] = 0;
    tft.drawString(slice, 2, LIST_TOP + i * VIEW_ROW_H, 1);
  }
  int y = LIST_BOT - 12;
  tft.fillRect(0, y, SCR_W, 12, 0x0841);
  tft.setTextColor(TFT_SILVER, 0x0841);
  tft.setTextDatum(ML_DATUM);
  char info[64];
  snprintf(info, sizeof(info), "%d/%d%s", scrollOff + 1, rowCount,
           truncated ? "  (truncated)" : "");
  tft.drawString(info, 4, y + 6, 1);
}

bool touchScreenFileView(int sx, int sy) {
  int btn = uiFooterHit(sx, sy);
  if (btn == 0) { uiGoto(UI_FILES); return true; }
  if (btn == 1) {
    if (scrollOff > 0) { scrollOff -= visible; if (scrollOff < 0) scrollOff = 0; uiRedraw(); }
    return true;
  }
  if (btn == 2) {
    if (scrollOff + visible < rowCount) {
      scrollOff += visible;
      if (scrollOff > rowCount - 1) scrollOff = rowCount - 1;
      uiRedraw();
    }
    return true;
  }
  return false;
}
