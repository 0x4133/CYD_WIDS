// screen_contacts.cpp — merged view of saved contacts + heard ESP-NOW peers.
// Tap a row to name/rename (empty input on a saved contact removes it).
#include "ui.h"
#include "config.h"
#include "contacts.h"
#include "espnow_chat.h"
#include "keyboard.h"

#define CROW_H     24
#define MAX_PEERS  8

static const int LIST_Y  = LIST_TOP;
static const int CLIST_H  = LIST_BOT - LIST_TOP;
static const int VISIBLE = CLIST_H / CROW_H;

struct Row { bool saved; uint8_t mac[6]; int contactIdx; uint32_t lastMs; };

static Row rows[MAX_CONTACTS + MAX_PEERS];
static int rowCount = 0;
static int scrollOff = 0;
static int selected  = -1;  // row index, or -1
static int lastRowsDrawn = 0;
// Keyed by visible slot, not entry idx — only VISIBLE rows are on screen.
#define CONTACTS_SLOT_MAX  8
static char lastRowText[CONTACTS_SLOT_MAX][72] = {{0}};
static bool lastEmptyDrawn = false;

static void buildRows() {
  rowCount = 0;
  for (int i = 0; i < contactsCount() && rowCount < (int)(sizeof(rows)/sizeof(rows[0])); i++) {
    const Contact* c = contactsGet(i);
    Row& r = rows[rowCount++];
    r.saved = true; r.contactIdx = i;
    memcpy(r.mac, c->mac, 6);
    r.lastMs = c->lastSeenMs;
  }
  uint8_t peerMacs[MAX_PEERS][6];
  uint32_t peerLast[MAX_PEERS];
  int np = chatPeerList(peerMacs, peerLast, MAX_PEERS);
  for (int i = 0; i < np && rowCount < (int)(sizeof(rows)/sizeof(rows[0])); i++) {
    if (contactsFindByMac(peerMacs[i]) >= 0) continue;  // already a contact
    Row& r = rows[rowCount++];
    r.saved = false; r.contactIdx = -1;
    memcpy(r.mac, peerMacs[i], 6);
    r.lastMs = peerLast[i];
  }
}

static void drawRow(int slot, int idx) {
  const Row& r = rows[idx];
  int y = LIST_Y + slot * CROW_H;
  uint16_t bg = (idx == selected) ? TFT_NAVY : TFT_BLACK;
  char macs[18];
  snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
    r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
  uint16_t col = r.saved ? TFT_CYAN : TFT_DARKGREY;
  char line[64];
  if (r.saved) {
    const Contact* c = contactsGet(r.contactIdx);
    char flag = (c && c->hasKey) ? 'K' : ' ';
    snprintf(line, sizeof(line), "*%c %-12s %s", flag, c ? c->name : "?", macs);
  } else {
    uint32_t ago = r.lastMs ? (millis() - r.lastMs) / 1000 : 0;
    snprintf(line, sizeof(line), "+ %s  %lus ago", macs, (unsigned long)ago);
  }
  char key[72];
  snprintf(key, sizeof(key), "%u|%s", (unsigned)bg, line);
  if (slot < CONTACTS_SLOT_MAX && strcmp(key, lastRowText[slot]) == 0) return;
  tft.fillRect(0, y, SCR_W, CROW_H, bg);
  tft.setTextColor(col, bg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(line, 4, y + CROW_H/2, 1);
  if (slot < CONTACTS_SLOT_MAX)
    strlcpy(lastRowText[slot], key, sizeof(lastRowText[slot]));
}

static void refreshFooter() {
  bool selSaved = (selected >= 0 && selected < rowCount && rows[selected].saved);
  if (selSaved) uiDrawFooterButtons("BACK", "SEND", "REN");
  else          uiDrawFooterButtons("BACK", "UP", "DN");
}

void enterScreenContacts() {
  scrollOff = 0;
  selected  = -1;
  lastRowsDrawn = 0;
  memset(lastRowText, 0, sizeof(lastRowText));
  lastEmptyDrawn = false;
  refreshFooter();
}

void drawScreenContacts() {
  buildRows();
  if (scrollOff > rowCount - VISIBLE) scrollOff = max(0, rowCount - VISIBLE);
  int shown = min(VISIBLE, rowCount - scrollOff);
  for (int i = 0; i < shown; i++) drawRow(i, scrollOff + i);
  for (int i = shown; i < lastRowsDrawn; i++) {
    int y = LIST_Y + i * CROW_H;
    tft.fillRect(0, y, SCR_W, CROW_H, TFT_BLACK);
    if (i < CONTACTS_SLOT_MAX) lastRowText[i][0] = 0;
  }
  if (rowCount == 0) {
    if (!lastEmptyDrawn) {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("no contacts and no peers heard yet",
                     SCR_W/2, LIST_Y + 40, 2);
      lastEmptyDrawn = true;
    }
  } else {
    lastEmptyDrawn = false;
  }
  lastRowsDrawn = shown;
}

static void promptFor(Row& r) {
  char buf[CONTACT_NAME_MAX + 1] = {0};
  if (r.saved) {
    const Contact* c = contactsGet(r.contactIdx);
    if (c) strlcpy(buf, c->name, sizeof(buf));
  }
  if (!keyboardPrompt("Contact name", buf, sizeof(buf))) {
    uiGoto(UI_CONTACTS);
    return;
  }
  if (r.saved) {
    if (buf[0] == 0) contactsRemove(r.contactIdx);
    else             contactsRename(r.contactIdx, buf);
  } else {
    if (buf[0] != 0) {
      contactsAdd(r.mac, buf);
      chatRequestKey(r.mac);  // kick off X25519 exchange; reply fills hasKey
    }
  }
  uiGoto(UI_CONTACTS);   // full redraw after modal
}

bool touchScreenContacts(int sx, int sy) {
  int btn = uiFooterHit(sx, sy);
  bool selSaved = (selected >= 0 && selected < rowCount && rows[selected].saved);

  if (btn == 0) { uiGoto(UI_ESPNOW); return true; }
  if (btn >= 0 && selSaved) {
    // Actions on the currently-selected contact.
    if (btn == 1) {
      const Contact* c = contactsGet(rows[selected].contactIdx);
      if (!c) return true;
      if (!c->hasKey) {
        // Key exchange hasn't completed — re-fire KEY_REQ so they can retry.
        Serial.println("[CTX] no key yet — re-requesting");
        chatRequestKey(c->mac);
        return true;
      }
      filesEnterPickMode(c->mac, c->name);
      uiGoto(UI_FILES);
      return true;
    }
    if (btn == 2) { promptFor(rows[selected]); return true; }
  } else if (btn == 1) {
    if (scrollOff > 0) { scrollOff--; uiRedraw(); }
    return true;
  } else if (btn == 2) {
    if (scrollOff + VISIBLE < rowCount) { scrollOff++; uiRedraw(); }
    return true;
  }

  if (sy >= LIST_Y && sy < LIST_Y + VISIBLE * CROW_H) {
    int slot = (sy - LIST_Y) / CROW_H;
    int idx = scrollOff + slot;
    if (idx >= 0 && idx < rowCount) {
      if (!rows[idx].saved) {
        // Heard peer — keep the old flow: tapping goes to name prompt.
        promptFor(rows[idx]);
      } else if (selected == idx) {
        // Second tap on saved contact → rename.
        promptFor(rows[idx]);
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
