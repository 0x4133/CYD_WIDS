// screen_espnow.cpp
#include "ui.h"
#include "config.h"
#include "espnow_scan.h"
#include "espnow_chat.h"
#include "contacts.h"
#include "detect.h"
#include "keyboard.h"

// Sections of the working area (LIST_TOP ... LIST_BOT = 28+18 ... 240-32 = 46..208):
//   stats band     : 46..62   (h=16)
//   last sniff     : 62..78   (h=16)
//   chat log       : 80..176  (h=96, ~4 lines @ 24)
//   compose strip  : 180..208 (h=28)
static const int STATS_Y   = LIST_TOP;                 // 46
static const int SNIFF_Y   = LIST_TOP + 16;            // 62
static const int CHAT_Y    = LIST_TOP + 34;            // 80
static const int CHAT_H    = 96;
static const int COMPOSE_Y = LIST_BOT - 28;            // 180
static const int COMPOSE_H = 28;

static char draft[CHAT_MSG_MAX + 1] = {0};

// Per-region caches to suppress 1.5s-tick flicker when content is unchanged.
static char lastStats[80]   = {0};
static char lastSniff[80]   = {0};
static char lastChat[4][96] = {{0}};
static int  lastChatRows    = 0;
static char lastCompose[80] = {0};

static void macShort(const uint8_t* m, char* out /*10*/) {
  snprintf(out, 10, "%02X%02X:%02X", m[3], m[4], m[5]);
}

static void drawStats() {
  char buf[64];
  snprintf(buf, sizeof(buf), "sniff:%s seen:%lu peers:%d rx:%lu ch:%d",
    detectEnabled() ? "ON" : "off",
    (unsigned long)espnowScanSeenCount(),
    chatPeerCount(),
    (unsigned long)chatRecvCount(),
    ESPNOW_CHANNEL);
  if (strcmp(buf, lastStats) == 0) return;
  tft.fillRect(0, STATS_Y, SCR_W, 16, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, 4, STATS_Y + 8, 1);
  strlcpy(lastStats, buf, sizeof(lastStats));
}

static void drawLastSniff() {
  // Chat target line — tap the band to cycle to the next keyed contact.
  char buf[64];
  uint16_t col = TFT_YELLOW;
  if (chatHasTarget()) {
    uint8_t m[6]; chatGetTarget(m);
    int ci = contactsFindByMac(m);
    const char* nm = (ci >= 0) ? contactsGet(ci)->name : "?";
    snprintf(buf, sizeof(buf), "TO: %s  %02X%02X:%02X  (tap to cycle)",
             nm, m[3], m[4], m[5]);
    col = TFT_CYAN;
  } else {
    snprintf(buf, sizeof(buf), "TO: (none) — tap to pick a keyed contact");
    col = TFT_DARKGREY;
  }
  if (strcmp(buf, lastSniff) == 0) return;
  tft.fillRect(0, SNIFF_Y, SCR_W, 16, TFT_BLACK);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, 4, SNIFF_Y + 8, 1);
  strlcpy(lastSniff, buf, sizeof(lastSniff));
}

// Advance chatTarget to the next contact with hasKey=true. If the current
// target is the last keyed contact (or unset), wrap/pick the first.
static void cycleTarget() {
  int nc = contactsCount();
  if (nc == 0) return;
  int startIdx = -1;
  if (chatHasTarget()) {
    uint8_t cur[6]; chatGetTarget(cur);
    startIdx = contactsFindByMac(cur);
  }
  for (int step = 1; step <= nc; step++) {
    int i = ((startIdx >= 0 ? startIdx : -1) + step + nc) % nc;
    const Contact* c = contactsGet(i);
    if (c && c->hasKey) { chatSetTarget(c->mac); return; }
  }
  // No keyed contacts — clear any stale target so the UI reflects reality.
  chatSetTarget(nullptr);
}

static void drawChat() {
  ChatMsg msgs[4];
  int n = chatSnapshot(msgs, 4);
  // Build all row keys first so we can compare and skip per-row.
  char keys[4][96] = {{0}};
  char lines[4][80] = {{0}};
  uint16_t cols[4] = {0};
  int rowsNow = n;
  for (int i = n - 1; i >= 0; i--) {
    int row = (n - 1 - i);
    char who[17];
    if (msgs[i].mine) strcpy(who, "me");
    else {
      int ci = contactsFindByMac(msgs[i].mac);
      if (ci >= 0) strlcpy(who, contactsGet(ci)->name, sizeof(who));
      else         macShort(msgs[i].mac, who);
    }
    cols[row] = msgs[i].mine ? TFT_CYAN : TFT_GREEN;
    snprintf(lines[row], sizeof(lines[row]), "%s: %s", who, msgs[i].text);
    if (tft.textWidth(lines[row], 2) > SCR_W - 8) {
      int l = strlen(lines[row]);
      while (l > 4 && tft.textWidth(lines[row], 2) > SCR_W - 8) { lines[row][--l] = 0; }
    }
    snprintf(keys[row], sizeof(keys[row]), "%u|%s", (unsigned)cols[row], lines[row]);
  }
  tft.setTextDatum(ML_DATUM);
  for (int row = 0; row < rowsNow; row++) {
    if (strcmp(keys[row], lastChat[row]) == 0) continue;
    int y = CHAT_Y + row * 24 + 12;
    tft.fillRect(0, y - 11, SCR_W, 24, TFT_BLACK);
    tft.setTextColor(cols[row], TFT_BLACK);
    tft.drawString(lines[row], 4, y, 2);
    strlcpy(lastChat[row], keys[row], sizeof(lastChat[row]));
  }
  // Clear rows that went away (e.g., chat cleared).
  for (int row = rowsNow; row < lastChatRows; row++) {
    int y = CHAT_Y + row * 24 + 12;
    tft.fillRect(0, y - 11, SCR_W, 24, TFT_BLACK);
    lastChat[row][0] = 0;
  }
  lastChatRows = rowsNow;
}

static void drawCompose() {
  char key[80];
  if (draft[0] == 0) strcpy(key, "~empty");
  else {
    const char* show = draft;
    int len = strlen(draft);
    if (len > 38) show = draft + (len - 38);
    snprintf(key, sizeof(key), "t|%s", show);
  }
  if (strcmp(key, lastCompose) == 0) return;
  tft.fillRect(0, COMPOSE_Y, SCR_W, COMPOSE_H, 0x1082);
  tft.drawRect(0, COMPOSE_Y, SCR_W, COMPOSE_H, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  if (draft[0] == 0) {
    tft.setTextColor(TFT_DARKGREY, 0x1082);
    tft.drawString("tap here to type...", 6, COMPOSE_Y + COMPOSE_H/2, 2);
  } else {
    tft.setTextColor(TFT_WHITE, 0x1082);
    tft.drawString(key + 2, 6, COMPOSE_Y + COMPOSE_H/2, 2);
  }
  strlcpy(lastCompose, key, sizeof(lastCompose));
}

void enterScreenEspnow() {
  uiDrawFooterButtons("PEERS", "SNIFF", "SEND");
  lastStats[0] = 0;
  lastSniff[0] = 0;
  lastCompose[0] = 0;
  memset(lastChat, 0, sizeof(lastChat));
  lastChatRows = 0;
}

void drawScreenEspnow() {
  drawStats();
  drawLastSniff();
  drawChat();
  drawCompose();
}

bool touchScreenEspnow(int sx, int sy) {
  // Compose strip → keyboard
  if (sy >= COMPOSE_Y && sy < COMPOSE_Y + COMPOSE_H) {
    keyboardPrompt("Message", draft, sizeof(draft));
    uiGoto(UI_ESPNOW);   // full redraw after modal
    return true;
  }
  // Tap on the TO: strip → cycle to the next keyed contact.
  if (sy >= SNIFF_Y && sy < SNIFF_Y + 16) {
    cycleTarget();
    uiRedraw();
    return true;
  }
  int btn = uiFooterHit(sx, sy);
  if (btn == 0) { uiGoto(UI_CONTACTS); return true; }
  if (btn == 1) { detectEnable(!detectEnabled()); uiRedraw(); return true; }
  if (btn == 2) {
    if (draft[0]) { chatSend(draft); draft[0] = 0; }
    uiRedraw();
    return true;
  }
  return false;
}
