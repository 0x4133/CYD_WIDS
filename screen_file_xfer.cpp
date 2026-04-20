// screen_file_xfer.cpp — overlay UI for ESP-NOW file transfers.
// Two modes, drawn on top of whatever screen is active:
//   1. Offer modal:  AirDrop-style centered card, blocks touch except ACCEPT/DECLINE.
//   2. Progress bar: thin strip along the bottom with CANCEL, does NOT block other touches.
#include "ui.h"
#include "config.h"
#include "file_xfer.h"

#define MODAL_W   280
#define MODAL_H   140
#define BAR_H     28
#define BAR_PAD   2

static void fmtSize(uint32_t sz, char* out, size_t outN) {
  if (sz < 1024)             snprintf(out, outN, "%luB",  (unsigned long)sz);
  else if (sz < 1024 * 1024) snprintf(out, outN, "%luK",  (unsigned long)(sz / 1024));
  else                       snprintf(out, outN, "%.1fM", sz / (1024.0f * 1024.0f));
}

// Rect helpers for modal buttons.
static const int MODAL_X = (SCR_W - MODAL_W) / 2;
static const int MODAL_Y = (SCR_H - MODAL_H) / 2;
static const int ACC_X = MODAL_X + 20;
static const int ACC_Y = MODAL_Y + MODAL_H - 38;
static const int ACC_W = 100;
static const int BTN_H = 28;
static const int DEC_X = MODAL_X + MODAL_W - 20 - ACC_W;
static const int DEC_Y = ACC_Y;

// Progress-bar CANCEL button rect (bottom strip).
static const int BAR_Y       = SCR_H - BAR_H;
static const int CANCEL_W    = 60;
static const int CANCEL_X    = SCR_W - CANCEL_W - 2;

bool xferOverlayVisible() {
  XferStatus st;
  if (fileXferOfferPending()) return true;
  if (fileXferStatus(&st)) {
    switch (st.state) {
      case XS_OFFERING: case XS_SENDING:   case XS_REPAIR:
      case XS_WAIT_DONE: case XS_RECEIVING:
      // Terminal states are kept visible briefly so the user sees the result.
      case XS_DONE: case XS_FAILED: case XS_CANCELED:
        return true;
      default: return false;
    }
  }
  return false;
}

bool xferOverlayModal() { return fileXferOfferPending(); }

static void drawModal() {
  XferStatus st;
  if (!fileXferStatus(&st)) return;
  // Dim the background.
  for (int y = 0; y < SCR_H; y += 2) tft.drawFastHLine(0, y, SCR_W, 0x2104);
  // Card.
  tft.fillRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, TFT_BLACK);
  tft.drawRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, TFT_CYAN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Incoming file", MODAL_X + MODAL_W/2, MODAL_Y + 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char line[96], sz[12];
  snprintf(line, sizeof(line), "from %s", st.peerName);
  tft.drawString(line, MODAL_X + MODAL_W/2, MODAL_Y + 32, 2);
  tft.drawString(st.fileName, MODAL_X + MODAL_W/2, MODAL_Y + 54, 2);
  fmtSize(st.fileSize, sz, sizeof(sz));
  snprintf(line, sizeof(line), "%s  (%u chunks)", sz, st.chunksTotal);
  tft.drawString(line, MODAL_X + MODAL_W/2, MODAL_Y + 76, 1);
  // Buttons.
  tft.fillRect(ACC_X, ACC_Y, ACC_W, BTN_H, TFT_DARKGREEN);
  tft.drawRect(ACC_X, ACC_Y, ACC_W, BTN_H, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ACCEPT", ACC_X + ACC_W/2, ACC_Y + BTN_H/2, 2);
  tft.fillRect(DEC_X, DEC_Y, ACC_W, BTN_H, 0x3800);
  tft.drawRect(DEC_X, DEC_Y, ACC_W, BTN_H, TFT_RED);
  tft.setTextColor(TFT_WHITE, 0x3800);
  tft.drawString("DECLINE", DEC_X + ACC_W/2, DEC_Y + BTN_H/2, 2);
}

static void drawBar() {
  XferStatus st;
  if (!fileXferStatus(&st)) return;
  tft.fillRect(0, BAR_Y, SCR_W, BAR_H, TFT_BLACK);
  tft.drawFastHLine(0, BAR_Y, SCR_W, TFT_DARKGREY);

  bool terminal = (st.state == XS_DONE || st.state == XS_FAILED || st.state == XS_CANCELED);
  bool active   = (st.state == XS_SENDING || st.state == XS_REPAIR ||
                   st.state == XS_RECEIVING || st.state == XS_WAIT_DONE);
  uint32_t pct = (st.chunksTotal > 0) ? (st.chunksDone * 100UL / st.chunksTotal) : 0;

  // Progress bar area only for non-terminal states.
  if (!terminal) {
    int barW = SCR_W - CANCEL_W - 10;
    tft.drawRect(4, BAR_Y + BAR_PAD + 8, barW, 8, TFT_SILVER);
    int fill = (int)((barW - 2) * pct / 100);
    if (fill < 0) fill = 0;
    uint16_t col = st.incoming ? TFT_CYAN : TFT_GREEN;
    tft.fillRect(5, BAR_Y + BAR_PAD + 9, fill, 6, col);
  }

  // State-specific label.
  char line[80];
  const char* verb = st.incoming ? "RECV" : "SEND";
  switch (st.state) {
    case XS_OFFERING:
      snprintf(line, sizeof(line), "SEND %s  offering, waiting...", st.fileName); break;
    case XS_SENDING:
      snprintf(line, sizeof(line), "SEND %s  %u%%", st.fileName, (unsigned)pct); break;
    case XS_REPAIR:
      snprintf(line, sizeof(line), "SEND %s  repairing...", st.fileName); break;
    case XS_WAIT_DONE:
      snprintf(line, sizeof(line), "SEND %s  verifying...", st.fileName); break;
    case XS_RECEIVING:
      snprintf(line, sizeof(line), "RECV %s  %u%%", st.fileName, (unsigned)pct); break;
    case XS_DONE:
      snprintf(line, sizeof(line), "%s %s  DONE", verb, st.fileName); break;
    case XS_FAILED:
      snprintf(line, sizeof(line), "%s %s  FAILED", verb, st.fileName); break;
    case XS_CANCELED:
      snprintf(line, sizeof(line), "%s %s  CANCELED", verb, st.fileName); break;
    default:
      snprintf(line, sizeof(line), "%s %s", verb, st.fileName); break;
  }
  uint16_t textCol = TFT_SILVER;
  if (st.state == XS_DONE)     textCol = TFT_GREEN;
  if (st.state == XS_FAILED)   textCol = TFT_RED;
  if (st.state == XS_CANCELED) textCol = TFT_ORANGE;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(textCol, TFT_BLACK);
  tft.drawString(line, 4, BAR_Y + BAR_H/2, 1);

  // CANCEL button only while cancel-able.
  if (active || st.state == XS_OFFERING) {
    tft.fillRect(CANCEL_X, BAR_Y + 2, CANCEL_W, BAR_H - 4, 0x3800);
    tft.drawRect(CANCEL_X, BAR_Y + 2, CANCEL_W, BAR_H - 4, TFT_RED);
    tft.setTextColor(TFT_WHITE, 0x3800);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("CANCEL", CANCEL_X + CANCEL_W/2, BAR_Y + BAR_H/2, 1);
  }
}

void xferOverlayDraw() {
  if (fileXferOfferPending()) drawModal();
  else                        drawBar();
}

static bool rectHit(int sx, int sy, int x, int y, int w, int h) {
  return sx >= x && sx < x + w && sy >= y && sy < y + h;
}

// Returns: 1 = consumed and blocking (modal), 0 = consumed non-blocking, -1 = not consumed.
int xferOverlayTouch(int sx, int sy) {
  if (fileXferOfferPending()) {
    if (rectHit(sx, sy, ACC_X, ACC_Y, ACC_W, BTN_H)) {
      fileXferAcceptIncoming(); uiGoto(uiCurrent()); return 1;
    }
    if (rectHit(sx, sy, DEC_X, DEC_Y, ACC_W, BTN_H)) {
      fileXferDeclineIncoming(); uiGoto(uiCurrent()); return 1;
    }
    return 1;  // modal blocks everything else
  }
  XferStatus st;
  if (fileXferStatus(&st)) {
    bool active = (st.state == XS_SENDING || st.state == XS_RECEIVING ||
                   st.state == XS_WAIT_DONE || st.state == XS_OFFERING);
    if (active && rectHit(sx, sy, CANCEL_X, BAR_Y, CANCEL_W, BAR_H)) {
      fileXferCancel(); return 0;
    }
  }
  return -1;
}
