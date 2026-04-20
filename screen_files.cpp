// screen_files.cpp — SD card browser with select + OPEN/SEND/DEL + rename.
// Selection model: first tap selects a file (navy highlight); second tap on
// the same file opens the keyboard to rename it. Footer swaps between
// browse buttons [UP/DN/..] and action buttons [OPEN/SEND/DEL] based on
// whether a file row is currently selected.
#include "ui.h"
#include "config.h"
#include "sd_writer.h"
#include "keyboard.h"
#include "contacts.h"
#include "espnow_chat.h"
#include "file_xfer.h"
#include <SD.h>

#define FROW_H      22
#define MAX_ENTRIES 40
#define NAME_MAX    32

struct Entry { char name[NAME_MAX + 1]; uint32_t size; bool isDir; };

static const int LIST_Y = LIST_TOP;
static const int FLIST_H = LIST_BOT - LIST_TOP;
static const int VISIBLE = FLIST_H / FROW_H;

static Entry ents[MAX_ENTRIES];
static int   entN = 0;
static int   scrollOff = 0;
static int   selected  = -1;          // entry index, or -1
static int   lastRowsDrawn = 0;
static char  cwd[96] = "/";
static uint32_t lastScan = 0;
// Dirty-cache so 1.5s redraws don't flicker unchanged rows/footer.
#define FILES_VISIBLE_MAX  8
static char  lastRowText[FILES_VISIBLE_MAX][72] = {{0}};
static char  lastFooterLine[72] = {0};
static int   lastScrollDraw = -1;
static int   lastEntNDraw   = -1;
static bool  lastNoSdDrawn  = false;
static bool  lastEmptyDrawn = false;

// Pick mode: when set, selecting a file invokes fileXferStartSend against
// pickTarget and returns to UI_CONTACTS. Entered via filesEnterPickMode().
static bool    pickMode = false;
static uint8_t pickTargetMac[6];
static char    pickTargetName[CONTACT_NAME_MAX + 1];

static bool isTextExt(const char* name) {
  const char* d = strrchr(name, '.');
  if (!d) return false;
  return (strcasecmp(d, ".txt") == 0 || strcasecmp(d, ".log") == 0 ||
          strcasecmp(d, ".csv") == 0 || strcasecmp(d, ".md")  == 0);
}

static void pathJoin(char* dst, size_t cap, const char* a, const char* b) {
  if (strcmp(a, "/") == 0) snprintf(dst, cap, "/%s", b);
  else                     snprintf(dst, cap, "%s/%s", a, b);
}

static void scanDir() {
  entN = 0;
  if (!sdWriterReady()) return;
  File dir = SD.open(cwd);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  while (entN < MAX_ENTRIES) {
    File f = dir.openNextFile();
    if (!f) break;
    Entry& e = ents[entN++];
    const char* n = f.name();
    const char* last = strrchr(n, '/');
    strlcpy(e.name, last ? last + 1 : n, sizeof(e.name));
    e.size  = f.size();
    e.isDir = f.isDirectory();
    f.close();
  }
  dir.close();
  lastScan = millis();
}

static void fmtSize(uint32_t sz, char* out, size_t outN) {
  if (sz < 1024)             snprintf(out, outN, "%luB",  (unsigned long)sz);
  else if (sz < 1024 * 1024) snprintf(out, outN, "%luK",  (unsigned long)(sz / 1024));
  else                       snprintf(out, outN, "%.1fM", sz / (1024.0f * 1024.0f));
}

static void drawRow(int slot, int idx) {
  int y = LIST_Y + slot * FROW_H;
  uint16_t bg = (idx == selected) ? TFT_NAVY : TFT_BLACK;
  const Entry& e = ents[idx];
  uint16_t col = e.isDir ? TFT_CYAN : TFT_WHITE;
  char line[72], sz[12];
  if (e.isDir) {
    snprintf(line, sizeof(line), "[ ] %s/", e.name);
  } else {
    fmtSize(e.size, sz, sizeof(sz));
    snprintf(line, sizeof(line), "    %-22s %s", e.name, sz);
  }
  int l = strlen(line);
  while (l > 4 && tft.textWidth(line, 1) > SCR_W - 6) line[--l] = 0;
  char key[72];
  snprintf(key, sizeof(key), "%u|%s", (unsigned)bg, line);
  if (slot < FILES_VISIBLE_MAX && strcmp(key, lastRowText[slot]) == 0) return;
  tft.fillRect(0, y, SCR_W, FROW_H, bg);
  tft.setTextColor(col, bg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(line, 4, y + FROW_H/2, 1);
  if (slot < FILES_VISIBLE_MAX) strlcpy(lastRowText[slot], key, sizeof(lastRowText[slot]));
}

static void refreshFooter() {
  if (pickMode) {
    uiDrawFooterButtons("PICK", "..", "CANCEL");
  } else if (selected >= 0 && !ents[selected].isDir) {
    uiDrawFooterButtons("OPEN", "SEND", "DEL");
  } else {
    uiDrawFooterButtons("UP", "DN", "..");
  }
}

void filesEnterPickMode(const uint8_t targetMac[6], const char* targetName) {
  pickMode = true;
  memcpy(pickTargetMac, targetMac, 6);
  strlcpy(pickTargetName, targetName ? targetName : "?", sizeof(pickTargetName));
}

void enterScreenFiles() {
  refreshFooter();
  scrollOff = 0;
  selected  = -1;
  lastScan  = 0;
  lastRowsDrawn = 0;
  memset(lastRowText, 0, sizeof(lastRowText));
  lastFooterLine[0] = 0;
  lastScrollDraw = -1;
  lastEntNDraw   = -1;
  lastNoSdDrawn  = false;
  lastEmptyDrawn = false;
  scanDir();
}

void drawScreenFiles() {
  if (millis() - lastScan > 3000) scanDir();

  if (!sdWriterReady()) {
    if (!lastNoSdDrawn) {
      tft.fillRect(0, LIST_Y, SCR_W, FLIST_H, TFT_BLACK);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("no SD card", SCR_W/2, LIST_Y + 40, 2);
      lastNoSdDrawn = true;
    }
    lastRowsDrawn = 0;
    return;
  }
  lastNoSdDrawn = false;

  if (scrollOff > entN - VISIBLE) scrollOff = max(0, entN - VISIBLE);
  int shown = min(VISIBLE, entN - scrollOff);
  for (int i = 0; i < shown; i++) drawRow(i, scrollOff + i);
  for (int i = shown; i < lastRowsDrawn; i++) {
    int y = LIST_Y + i * FROW_H;
    tft.fillRect(0, y, SCR_W, FROW_H, TFT_BLACK);
    if (i < FILES_VISIBLE_MAX) lastRowText[i][0] = 0;
  }
  // Scroll bar — only redraw when scroll position or list size changed.
  if (entN > VISIBLE) {
    if (scrollOff != lastScrollDraw || entN != lastEntNDraw) {
      const int barW = 10;
      const int barX = SCR_W - barW - 1;
      const int trackY = LIST_Y;
      const int trackH = VISIBLE * FROW_H;
      tft.fillRect(barX, trackY, barW, trackH, 0x1082);  // dim track
      tft.drawRect(barX, trackY, barW, trackH, TFT_DARKGREY);
      int thumbH = max(12, trackH * VISIBLE / entN);
      int thumbY = trackY + (trackH - thumbH) * scrollOff / (entN - VISIBLE);
      tft.fillRect(barX + 1, thumbY + 1, barW - 2, thumbH - 2, TFT_SILVER);
      lastScrollDraw = scrollOff;
      lastEntNDraw   = entN;
    }
  } else {
    lastScrollDraw = -1;
    lastEntNDraw   = -1;
  }
  if (entN == 0) {
    if (!lastEmptyDrawn) {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      char buf[64]; snprintf(buf, sizeof(buf), "%s — empty", cwd);
      tft.drawString(buf, SCR_W/2, LIST_Y + 40, 2);
      lastEmptyDrawn = true;
    }
  } else {
    lastEmptyDrawn = false;
    int y = LIST_BOT - 12;
    char buf[96];
    if (pickMode) snprintf(buf, sizeof(buf), "pick → %s  cwd:%s", pickTargetName, cwd);
    else          snprintf(buf, sizeof(buf), "cwd:%s  (%d)", cwd, entN);
    if (strcmp(buf, lastFooterLine) != 0) {
      tft.fillRect(0, y, SCR_W, 12, 0x0841);
      tft.setTextColor(TFT_SILVER, 0x0841);
      tft.setTextDatum(ML_DATUM);
      tft.drawString(buf, 4, y + 6, 1);
      strlcpy(lastFooterLine, buf, sizeof(lastFooterLine));
    }
  }
  lastRowsDrawn = shown;
}

static void cdInto(const char* sub) {
  char tmp[96]; pathJoin(tmp, sizeof(tmp), cwd, sub);
  strlcpy(cwd, tmp, sizeof(cwd));
  scrollOff = 0; selected = -1;
  scanDir();
}

static void cdUp() {
  if (strcmp(cwd, "/") == 0) return;
  char* slash = strrchr(cwd, '/');
  if (!slash) return;
  if (slash == cwd) cwd[1] = 0;
  else              *slash = 0;
  scrollOff = 0; selected = -1;
  scanDir();
}

static void doDelete() {
  if (selected < 0 || ents[selected].isDir) return;
  char path[128]; pathJoin(path, sizeof(path), cwd, ents[selected].name);
  SD.remove(path);
  selected = -1;
  lastScan = 0;  // force rescan
}

static void doRename() {
  if (selected < 0 || ents[selected].isDir) return;
  char buf[NAME_MAX + 1];
  strlcpy(buf, ents[selected].name, sizeof(buf));
  if (!keyboardPrompt("Rename to", buf, sizeof(buf))) { uiGoto(UI_FILES); return; }
  if (!buf[0] || strcmp(buf, ents[selected].name) == 0) { uiGoto(UI_FILES); return; }
  char oldP[128], newP[128];
  pathJoin(oldP, sizeof(oldP), cwd, ents[selected].name);
  pathJoin(newP, sizeof(newP), cwd, buf);
  if (SD.exists(newP)) { uiGoto(UI_FILES); return; }  // refuse to clobber
  SD.rename(oldP, newP);
  selected = -1;
  lastScan = 0;
  uiGoto(UI_FILES);
}

static void doOpen() {
  if (selected < 0 || ents[selected].isDir) return;
  if (!isTextExt(ents[selected].name)) return;
  char path[128]; pathJoin(path, sizeof(path), cwd, ents[selected].name);
  extern void fileViewSetPath(const char* path);
  fileViewSetPath(path);
  uiGoto(UI_FILE_VIEW);
}

static void doSend() {
  if (selected < 0 || ents[selected].isDir) return;
  if (pickMode) return;  // pickMode uses PICK button instead
  // No target picked — route user through Contacts so they choose one.
  uiGoto(UI_CONTACTS);
}

static void doPick() {
  if (selected < 0 || ents[selected].isDir) return;
  char path[128]; pathJoin(path, sizeof(path), cwd, ents[selected].name);
  bool ok = fileXferStartSend(pickTargetMac, path);
  Serial.printf("[FILES] pick %s -> %s %s\n", path, pickTargetName, ok ? "ok" : "fail");
  pickMode = false;
  selected = -1;
  refreshFooter();
  // Stay on Files screen so the xfer overlay bar is visible immediately.
  uiRedraw();
}

bool touchScreenFiles(int sx, int sy) {
  int btn = uiFooterHit(sx, sy);
  bool fileSelected = (selected >= 0 && selected < entN && !ents[selected].isDir);

  if (btn >= 0) {
    if (pickMode) {
      if (btn == 0) { doPick(); return true; }
      if (btn == 1) { cdUp(); refreshFooter(); uiRedraw(); return true; }
      if (btn == 2) { pickMode = false; uiGoto(UI_CONTACTS); return true; }
    } else if (fileSelected) {
      if (btn == 0) { doOpen(); return true; }
      if (btn == 1) { doSend(); return true; }
      if (btn == 2) { doDelete(); refreshFooter(); uiRedraw(); return true; }
    } else {
      if (btn == 0) { if (scrollOff > 0) { scrollOff--; uiRedraw(); } return true; }
      if (btn == 1) { if (scrollOff + VISIBLE < entN) { scrollOff++; uiRedraw(); } return true; }
      if (btn == 2) { cdUp(); refreshFooter(); uiRedraw(); return true; }
    }
  }

  // Scroll-bar hit takes precedence over row selection so taps on the bar
  // don't inadvertently select whatever file is beneath it. Tap above the
  // thumb pages up, tap below pages down.
  const int barW  = 10;
  const int barX  = SCR_W - barW - 1;
  const int trackY = LIST_Y;
  const int trackH = VISIBLE * FROW_H;
  if (entN > VISIBLE && sx >= barX && sx < barX + barW &&
      sy >= trackY && sy < trackY + trackH) {
    int thumbH = max(12, trackH * VISIBLE / entN);
    int thumbY = trackY + (trackH - thumbH) * scrollOff / (entN - VISIBLE);
    if (sy < thumbY)               scrollOff = max(0, scrollOff - VISIBLE);
    else if (sy >= thumbY + thumbH) scrollOff = min(entN - VISIBLE, scrollOff + VISIBLE);
    uiRedraw();
    return true;
  }

  if (sy >= LIST_Y && sy < LIST_Y + VISIBLE * FROW_H && sx < barX) {
    int slot = (sy - LIST_Y) / FROW_H;
    int idx = scrollOff + slot;
    if (idx >= 0 && idx < entN) {
      if (ents[idx].isDir) {
        cdInto(ents[idx].name);
        refreshFooter();
        uiRedraw();
      } else if (selected == idx) {
        // Second tap on same file → rename.
        doRename();
      } else {
        selected = idx;
        refreshFooter();
        uiRedraw();
      }
      return true;
    }
  }
  return false;
}
