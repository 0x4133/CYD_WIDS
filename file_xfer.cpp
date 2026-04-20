// file_xfer.cpp — ESP-NOW unicast file transfer, async/out-of-order (v2).
//
// Protocol:
//   sender   → receiver : FILE_OFFER (size, crc, chunks, name)
//   receiver → sender   : FILE_ACCEPT / FILE_DECLINE
//   sender   → receiver : FILE_CHUNK [seq:2][data:N]  (in-order initial pass)
//   receiver → sender   : FILE_MISSING [count:2][(start:2,end:2)...]
//                          — emitted periodically while incomplete; [start,end)
//                            are runs of not-yet-received seqs
//   sender   → receiver : FILE_CHUNK (retransmits only the listed seqs)
//   receiver → sender   : FILE_DONE [status]  (once CRC verifies)
//
// Receiver preallocates the .partial file up-front so chunks can be written
// at arbitrary seq offsets. There is no ordering requirement. Any chunk may
// be dropped on the wire; the sender keeps retransmitting whatever the
// receiver reports missing until the bitmap is complete.
#include "file_xfer.h"
#include "espnow_chat.h"
#include "contacts.h"
#include "config.h"
#include "radio_scheduler.h"
#include "alerts.h"
#include <SD.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Pacing: sender fires at most one chunk every CHUNK_INTERVAL_MS. Needs to
// exceed typical SD seek+write latency on the receiver or the RX queue fills.
#define CHUNK_INTERVAL_MS     20
// Receiver emits a MISSING frame this often while incomplete.
#define MISSING_INTERVAL_MS   1500
// Receiver gives up if no CHUNK arrives for this long.
#define RECV_STALL_MS         20000
// Sender timeout in XS_WAIT_DONE (receiver said it's complete).
#define DONE_TIMEOUT_MS       30000
// Sender gives up in XS_REPAIR if no MISSING received for this long.
#define REPAIR_IDLE_MS        25000
// How long the offer modal lingers without a user decision.
#define OFFER_TIMEOUT_MS      30000
// Keep terminal status visible this long before returning to IDLE.
#define TERMINAL_LINGER_MS    3000
// Max gap ranges we parse from one MISSING frame (fits in 200-byte payload).
#define MAX_REPAIR_RANGES     48

static SemaphoreHandle_t mtx;
static XferStatus        st = {};
static File              fh;
static uint32_t          crcExpected  = 0;
static uint32_t          tStateEnter  = 0;
static char              inPath[96];       // receiver: .partial path; sender: source path

// ---- sender state --------------------------------------------------------
static uint16_t          initialSeq   = 0;     // next seq to send in initial pass
static uint32_t          tLastChunk   = 0;
static uint32_t          tLastMissingRx = 0;   // last time MISSING arrived
struct RepairRange { uint16_t start, end, cur; };
static RepairRange       repairList[MAX_REPAIR_RANGES];
static int               repairCount  = 0;
static int               repairIdx    = 0;

// ---- receiver state ------------------------------------------------------
static uint8_t*          recvBitmap   = nullptr;
static uint16_t          recvCount    = 0;
static uint32_t          tLastChunkRx = 0;
static uint32_t          tLastMissingTx = 0;

// RX chunk queue keeps heavy SD ops off the ESP-NOW RX task's stack.
#define CHUNK_Q_DEPTH 24
struct QChunk { uint16_t seq; uint8_t len; uint8_t data[XFER_CHUNK_BYTES]; };
static QChunk  qBuf[CHUNK_Q_DEPTH];
static uint8_t qHead = 0, qTail = 0;
static inline bool qEmpty() { return qHead == qTail; }
static inline bool qFull()  { return (uint8_t)((qHead + 1) % CHUNK_Q_DEPTH) == qTail; }

// Renamed off `bitGet`/`bitSet` to avoid collision with Arduino.h macros.
static inline bool bmGet(uint16_t i) { return (recvBitmap[i >> 3] >> (i & 7)) & 1; }
static inline void bmSet(uint16_t i) { recvBitmap[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static void setState(XferState s) {
  st.state = s;
  tStateEnter = millis();
}

static void logStatus(const char* tag) {
  Serial.printf("[XFER] %s state=%d inc=%d name=%s %lu/%lu chunks=%u/%u\n",
    tag, st.state, st.incoming, st.fileName,
    (unsigned long)st.bytesDone, (unsigned long)st.fileSize,
    st.chunksDone, st.chunksTotal);
}

static void sanitizeName(const char* in, char* out) {
  int j = 0;
  for (int i = 0; in[i] && j < XFER_NAME_MAX; i++) {
    char c = in[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') continue;
    if (c == '.' && j > 0 && out[j-1] == '.') continue;
    out[j++] = c;
  }
  if (j == 0) { strcpy(out, "file"); return; }
  out[j] = 0;
}

static void resolveInbox(const char* sender, const char* fname, char* outPath, int cap) {
  String dir = String("/inbox/") + sender;
  if (!SD.exists("/inbox"))    SD.mkdir("/inbox");
  if (!SD.exists(dir.c_str())) SD.mkdir(dir.c_str());
  String base = dir + "/" + fname;
  if (!SD.exists(base.c_str())) { strlcpy(outPath, base.c_str(), cap); return; }
  int dot = -1;
  for (int i = strlen(fname) - 1; i >= 0; i--) if (fname[i] == '.') { dot = i; break; }
  String stem = (dot > 0) ? String(fname).substring(0, dot) : String(fname);
  String ext  = (dot > 0) ? String(fname).substring(dot)    : String("");
  for (int n = 1; n < 100; n++) {
    String cand = dir + "/" + stem + "_" + String(n) + ext;
    if (!SD.exists(cand.c_str())) { strlcpy(outPath, cand.c_str(), cap); return; }
  }
  strlcpy(outPath, base.c_str(), cap);
}

static bool sendRaw(const uint8_t dest[6], uint8_t tag,
                    const uint8_t* data, int len) {
  if (!chatEnsurePeer(dest)) return false;
  uint8_t buf[250];
  if (len + 1 > (int)sizeof(buf)) return false;
  buf[0] = tag;
  if (len > 0) memcpy(buf + 1, data, len);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  return esp_now_send(dest, buf, len + 1) == ESP_OK;
}

// Control frames (OFFER/ACCEPT/DECLINE/DONE/CANCEL) — burst 3x spaced ~400ms
// so one lands even if the peer is mid WiFi scan.
static bool sendCtl(const uint8_t dest[6], uint8_t tag,
                    const uint8_t* data, int len) {
  bool anyOk = false;
  for (int i = 0; i < 3; i++) {
    if (sendRaw(dest, tag, data, len)) anyOk = true;
    if (i < 2) vTaskDelay(pdMS_TO_TICKS(140));
  }
  return anyOk;
}

static void cleanupFile() {
  if (fh) fh.close();
  if (inPath[0] && st.incoming && st.state != XS_DONE) SD.remove(inPath);
  inPath[0] = 0;
  if (recvBitmap) { free(recvBitmap); recvBitmap = nullptr; }
  recvCount = 0;
  qHead = qTail = 0;
  repairCount = 0;
  repairIdx = 0;
}

static void endTransfer(XferState terminal) {
  cleanupFile();
  setState(terminal);
  logStatus("end");
  if (radioPausedForXfer()) radioResumeAfterXfer();
}

void fileXferBegin() {
  mtx = xSemaphoreCreateMutex();
  memset(&st, 0, sizeof(st));
  if (!SD.exists("/inbox")) SD.mkdir("/inbox");
  Serial.println("[XFER] ready");
}

bool fileXferStartSend(const uint8_t destMac[6], const char* path) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_IDLE && st.state != XS_DONE &&
      st.state != XS_FAILED && st.state != XS_CANCELED) {
    xSemaphoreGive(mtx);
    Serial.println("[XFER] start: busy");
    return false;
  }
  int ci = contactsFindByMac(destMac);
  if (ci < 0) { xSemaphoreGive(mtx); Serial.println("[XFER] start: no contact"); return false; }
  const Contact* c = contactsGet(ci);
  if (!c || !c->hasKey) { xSemaphoreGive(mtx); Serial.println("[XFER] start: no key"); return false; }

  File f = SD.open(path, FILE_READ);
  if (!f) { xSemaphoreGive(mtx); Serial.printf("[XFER] start: open fail %s\n", path); return false; }
  uint32_t size = f.size();
  if (size == 0 || size > 13000000UL) {
    f.close(); xSemaphoreGive(mtx);
    Serial.printf("[XFER] start: bad size %lu\n", (unsigned long)size);
    return false;
  }

  // Streaming CRC32.
  uint8_t tmp[512];
  uint32_t crc = 0;
  while (f.available()) {
    int n = f.read(tmp, sizeof(tmp));
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, tmp, n);
  }
  f.close();

  const char* fname = path;
  for (int i = strlen(path) - 1; i >= 0; i--) if (path[i] == '/') { fname = path + i + 1; break; }
  char cleanName[XFER_NAME_MAX + 1];
  sanitizeName(fname, cleanName);

  memset(&st, 0, sizeof(st));
  st.incoming = false;
  memcpy(st.peerMac, destMac, 6);
  strlcpy(st.peerName, c->name, sizeof(st.peerName));
  strlcpy(st.fileName, cleanName, sizeof(st.fileName));
  st.fileSize    = size;
  st.chunksTotal = (uint16_t)((size + XFER_CHUNK_BYTES - 1) / XFER_CHUNK_BYTES);
  st.startMs     = millis();
  crcExpected    = crc;
  strlcpy(inPath, path, sizeof(inPath));
  initialSeq   = 0;
  repairCount  = 0;
  repairIdx    = 0;
  tLastChunk   = 0;
  tLastMissingRx = millis();

  uint8_t buf[16 + XFER_NAME_MAX];
  int nameLen = strlen(cleanName);
  memcpy(buf + 0, &size,  4);
  memcpy(buf + 4, &crc,   4);
  memcpy(buf + 8, &st.chunksTotal, 2);
  buf[10] = (uint8_t)nameLen;
  memcpy(buf + 11, cleanName, nameLen);

  radioPauseForXfer();
  setState(XS_OFFERING);
  xSemaphoreGive(mtx);

  bool ok = sendCtl(destMac, TAG_FILE_OFFER, buf, 11 + nameLen);
  if (!ok) {
    Serial.println("[XFER] start: esp_now_send failed");
    endTransfer(XS_FAILED);
    return false;
  }
  logStatus("offer-sent");
  return true;
}

bool fileXferOfferPending() {
  return st.state == XS_OFFERED;
}

void fileXferAcceptIncoming() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_OFFERED) { xSemaphoreGive(mtx); return; }
  resolveInbox(st.peerName, st.fileName, inPath, sizeof(inPath));
  String partial = String(inPath) + ".partial";
  // Force a clean file — stale .partial from a prior aborted attempt would
  // leave FILE_WRITE positioned past our zeros and corrupt preallocation.
  if (SD.exists(partial.c_str())) SD.remove(partial.c_str());
  fh = SD.open(partial.c_str(), FILE_WRITE);
  if (!fh) {
    xSemaphoreGive(mtx);
    Serial.printf("[XFER] open-partial fail %s\n", partial.c_str());
    sendRaw(st.peerMac, TAG_FILE_DECLINE, (const uint8_t*)"\x02", 1);
    endTransfer(XS_FAILED);
    return;
  }
  strlcpy(inPath, partial.c_str(), sizeof(inPath));
  Serial.printf("[XFER] accept: prealloc %lu bytes, bitmap=%u bits\n",
                (unsigned long)st.fileSize, (unsigned)st.chunksTotal);

  // Preallocate the full size with zeros so seek+write works at arbitrary seq.
  // FAT won't let us seek past EOF, so we have to grow the file here.
  uint8_t zeros[512] = {0};
  uint32_t remaining = st.fileSize;
  uint32_t tPre = millis();
  while (remaining > 0) {
    int n = (remaining > sizeof(zeros)) ? sizeof(zeros) : remaining;
    int w = fh.write(zeros, n);
    if (w != n) {
      xSemaphoreGive(mtx);
      Serial.println("[XFER] preallocate write fail");
      endTransfer(XS_FAILED);
      return;
    }
    remaining -= n;
    // Yield every ~8KB so RX task / UI task can run during a long prealloc.
    if ((remaining & 0x1FFF) == 0) {
      xSemaphoreGive(mtx);
      vTaskDelay(pdMS_TO_TICKS(1));
      xSemaphoreTake(mtx, portMAX_DELAY);
      // State could have changed (cancel, etc.)
      if (st.state != XS_OFFERED) {
        xSemaphoreGive(mtx);
        Serial.println("[XFER] prealloc aborted by state change");
        return;
      }
    }
  }
  fh.flush();
  Serial.printf("[XFER] prealloc %lu bytes in %lums\n",
                (unsigned long)st.fileSize, (unsigned long)(millis() - tPre));

  size_t bmBytes = (st.chunksTotal + 7) / 8;
  recvBitmap = (uint8_t*)calloc(bmBytes, 1);
  if (!recvBitmap) {
    xSemaphoreGive(mtx);
    Serial.println("[XFER] bitmap alloc fail");
    endTransfer(XS_FAILED);
    return;
  }
  recvCount = 0;
  st.chunksDone = 0;
  st.bytesDone  = 0;
  tLastChunkRx   = millis();
  tLastMissingTx = millis();
  qHead = qTail = 0;
  radioPauseForXfer();
  setState(XS_RECEIVING);
  uint8_t peer[6]; memcpy(peer, st.peerMac, 6);
  xSemaphoreGive(mtx);
  sendCtl(peer, TAG_FILE_ACCEPT, nullptr, 0);
  logStatus("accepted");
}

void fileXferDeclineIncoming() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_OFFERED) { xSemaphoreGive(mtx); return; }
  uint8_t mac[6]; memcpy(mac, st.peerMac, 6);
  xSemaphoreGive(mtx);
  sendCtl(mac, TAG_FILE_DECLINE, (const uint8_t*)"\x00", 1);
  endTransfer(XS_CANCELED);
}

void fileXferCancel() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state == XS_IDLE || st.state == XS_DONE ||
      st.state == XS_FAILED || st.state == XS_CANCELED) {
    xSemaphoreGive(mtx);
    return;
  }
  uint8_t mac[6]; memcpy(mac, st.peerMac, 6);
  xSemaphoreGive(mtx);
  sendCtl(mac, TAG_FILE_CANCEL, nullptr, 0);
  endTransfer(XS_CANCELED);
}

bool fileXferStatus(XferStatus* out) {
  if (!out) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  *out = st;
  xSemaphoreGive(mtx);
  return st.state != XS_IDLE;
}

// ---- frame dispatch ------------------------------------------------------

static void onOffer(const uint8_t src[6], const uint8_t* p, int plen) {
  int ci = contactsFindByMac(src);
  if (ci < 0) {
    Serial.println("[XFER] OFFER dropped: not in contacts");
    alertRaise(ALERT_NEW_ESPNOW, src, 0, "file-offer from stranger");
    return;
  }
  const Contact* c = contactsGet(ci);
  if (!c || !c->hasKey) { Serial.println("[XFER] OFFER dropped: no key"); return; }
  if (plen < 11) { Serial.println("[XFER] OFFER dropped: short"); return; }

  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state == XS_OFFERED && memcmp(st.peerMac, src, 6) == 0) {
    xSemaphoreGive(mtx);
    return;
  }
  if (st.state != XS_IDLE && st.state != XS_DONE &&
      st.state != XS_FAILED && st.state != XS_CANCELED) {
    xSemaphoreGive(mtx);
    sendRaw(src, TAG_FILE_DECLINE, (const uint8_t*)"\x01", 1);
    return;
  }

  uint32_t size; uint32_t crc; uint16_t chunks;
  memcpy(&size,   p + 0, 4);
  memcpy(&crc,    p + 4, 4);
  memcpy(&chunks, p + 8, 2);
  uint8_t nameLen = p[10];
  if (nameLen > XFER_NAME_MAX || 11 + nameLen > plen) { xSemaphoreGive(mtx); return; }

  char raw[XFER_NAME_MAX + 1] = {0};
  memcpy(raw, p + 11, nameLen);
  char clean[XFER_NAME_MAX + 1];
  sanitizeName(raw, clean);

  memset(&st, 0, sizeof(st));
  st.incoming    = true;
  memcpy(st.peerMac, src, 6);
  strlcpy(st.peerName, c->name, sizeof(st.peerName));
  strlcpy(st.fileName, clean, sizeof(st.fileName));
  st.fileSize    = size;
  st.chunksTotal = chunks;
  st.startMs     = millis();
  crcExpected    = crc;
  setState(XS_OFFERED);
  xSemaphoreGive(mtx);
  logStatus("offer-recv");
}

static void onAccept(const uint8_t src[6]) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_OFFERING || memcmp(st.peerMac, src, 6) != 0) {
    xSemaphoreGive(mtx); return;
  }
  initialSeq = 0;
  tLastChunk = 0;
  tLastMissingRx = millis();
  repairCount = 0;
  repairIdx   = 0;
  setState(XS_SENDING);
  xSemaphoreGive(mtx);
  logStatus("send-start");
}

static void onDecline(const uint8_t src[6], uint8_t reason) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_OFFERING || memcmp(st.peerMac, src, 6) != 0) {
    xSemaphoreGive(mtx); return;
  }
  xSemaphoreGive(mtx);
  Serial.printf("[XFER] declined reason=%u\n", reason);
  endTransfer(XS_CANCELED);
}

static void onChunk(const uint8_t src[6], const uint8_t* p, int plen) {
  if (plen < 2 || plen - 2 > XFER_CHUNK_BYTES) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_RECEIVING || memcmp(st.peerMac, src, 6) != 0) {
    xSemaphoreGive(mtx); return;
  }
  if (qFull()) {
    xSemaphoreGive(mtx);
    static uint32_t tLastWarn = 0;
    if (millis() - tLastWarn > 500) {
      Serial.println("[XFER] chunk queue full — drop (will retransmit)");
      tLastWarn = millis();
    }
    return;
  }
  uint16_t seq; memcpy(&seq, p, 2);
  QChunk& q = qBuf[qHead];
  q.seq = seq;
  q.len = (uint8_t)(plen - 2);
  memcpy(q.data, p + 2, q.len);
  qHead = (qHead + 1) % CHUNK_Q_DEPTH;
  tLastChunkRx = millis();
  xSemaphoreGive(mtx);
}

static void onMissing(const uint8_t src[6], const uint8_t* p, int plen) {
  if (plen < 2) return;
  uint16_t count; memcpy(&count, p, 2);
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ours = (st.state == XS_SENDING || st.state == XS_REPAIR ||
               st.state == XS_WAIT_DONE) &&
              memcmp(st.peerMac, src, 6) == 0;
  if (!ours) { xSemaphoreGive(mtx); return; }
  tLastMissingRx = millis();

  if (count == 0) {
    // Receiver is complete — wait for FILE_DONE confirmation.
    if (st.state != XS_WAIT_DONE) {
      setState(XS_WAIT_DONE);
      Serial.println("[XFER] receiver complete — awaiting FILE_DONE");
    }
    xSemaphoreGive(mtx);
    return;
  }

  int avail = (plen - 2) / 4;
  if (avail > MAX_REPAIR_RANGES) avail = MAX_REPAIR_RANGES;
  if ((int)count < avail) avail = count;
  repairCount = 0;
  for (int i = 0; i < avail; i++) {
    uint16_t s, e;
    memcpy(&s, p + 2 + i*4,     2);
    memcpy(&e, p + 2 + i*4 + 2, 2);
    if (e <= s || s >= st.chunksTotal) continue;
    if (e > st.chunksTotal) e = st.chunksTotal;
    repairList[repairCount].start = s;
    repairList[repairCount].end   = e;
    repairList[repairCount].cur   = s;
    repairCount++;
  }
  repairIdx = 0;
  if (repairCount > 0) {
    if (st.state == XS_SENDING && initialSeq >= st.chunksTotal) setState(XS_REPAIR);
    else if (st.state == XS_WAIT_DONE) setState(XS_REPAIR);  // receiver re-opened work
    tLastChunk = 0;
  }
  xSemaphoreGive(mtx);
  Serial.printf("[XFER] MISSING count=%u ranges=%d\n", (unsigned)count, repairCount);
}

static void onCancel(const uint8_t src[6]) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool relevant = (st.state != XS_IDLE && st.state != XS_DONE &&
                   st.state != XS_FAILED && st.state != XS_CANCELED) &&
                  memcmp(st.peerMac, src, 6) == 0;
  xSemaphoreGive(mtx);
  if (relevant) endTransfer(XS_CANCELED);
}

static void onDone(const uint8_t src[6], uint8_t status) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool match = (st.state == XS_SENDING || st.state == XS_REPAIR ||
                st.state == XS_WAIT_DONE) &&
               memcmp(st.peerMac, src, 6) == 0;
  xSemaphoreGive(mtx);
  if (!match) return;
  if (status == 0) endTransfer(XS_DONE);
  else {
    alertRaise(ALERT_NEW_ESPNOW, src, 0, "peer crc fail");
    endTransfer(XS_FAILED);
  }
}

void fileXferOnFrame(const uint8_t src[6], uint8_t tag,
                     const uint8_t* payload, int plen) {
  switch (tag) {
    case TAG_FILE_OFFER:   onOffer(src, payload, plen); break;
    case TAG_FILE_ACCEPT:  onAccept(src); break;
    case TAG_FILE_DECLINE: onDecline(src, plen > 0 ? payload[0] : 0); break;
    case TAG_FILE_CHUNK:   onChunk(src, payload, plen); break;
    case TAG_FILE_MISSING: onMissing(src, payload, plen); break;
    case TAG_FILE_CANCEL:  onCancel(src); break;
    case TAG_FILE_DONE:    onDone(src, plen > 0 ? payload[0] : 0); break;
    case TAG_FILE_ACK:     break;  // legacy, ignored
  }
}

// ---- receiver: drain queue + periodic MISSING ----------------------------

static void drainRxQueue() {
  while (true) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (st.state != XS_RECEIVING || qEmpty() || !fh || !recvBitmap) {
      xSemaphoreGive(mtx); return;
    }
    QChunk q = qBuf[qTail];
    qTail = (qTail + 1) % CHUNK_Q_DEPTH;

    if (q.seq >= st.chunksTotal) { xSemaphoreGive(mtx); continue; }
    if (bmGet(q.seq))            { xSemaphoreGive(mtx); continue; }

    uint32_t pos = (uint32_t)q.seq * XFER_CHUNK_BYTES;
    if (!fh.seek(pos)) {
      xSemaphoreGive(mtx);
      Serial.println("[XFER] recv seek fail");
      endTransfer(XS_FAILED);
      return;
    }
    int w = fh.write(q.data, q.len);
    if (w != q.len) {
      xSemaphoreGive(mtx);
      Serial.println("[XFER] recv write fail");
      endTransfer(XS_FAILED);
      return;
    }
    bmSet(q.seq);
    recvCount++;
    st.chunksDone = recvCount;
    uint32_t b = (uint32_t)recvCount * XFER_CHUNK_BYTES;
    st.bytesDone = (b > st.fileSize) ? st.fileSize : b;
    xSemaphoreGive(mtx);
  }
}

// Build + send a MISSING frame describing current gaps. Returns true if a
// frame was sent (or completion was announced), false if nothing to do yet.
// Must be called WITHOUT holding mtx — acquires/releases internally.
static void receiverTickMissing() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_RECEIVING || !recvBitmap) { xSemaphoreGive(mtx); return; }
  uint8_t peer[6]; memcpy(peer, st.peerMac, 6);

  // Build range list from bitmap.
  uint8_t buf[240];
  int pos = 2;
  uint16_t count = 0;
  uint16_t total = st.chunksTotal;
  uint16_t s = 0;
  bool inGap = false;
  for (uint16_t i = 0; i < total && count < MAX_REPAIR_RANGES; i++) {
    bool have = bmGet(i);
    if (!have && !inGap) { s = i; inGap = true; }
    if (have && inGap) {
      uint16_t e = i;
      memcpy(buf + pos, &s, 2); pos += 2;
      memcpy(buf + pos, &e, 2); pos += 2;
      count++;
      inGap = false;
    }
  }
  if (inGap && count < MAX_REPAIR_RANGES) {
    uint16_t e = total;
    memcpy(buf + pos, &s, 2); pos += 2;
    memcpy(buf + pos, &e, 2); pos += 2;
    count++;
  }
  memcpy(buf, &count, 2);

  bool complete = (count == 0 && recvCount >= total);
  char rawInPath[96]; strlcpy(rawInPath, inPath, sizeof(rawInPath));
  xSemaphoreGive(mtx);

  if (!complete) {
    sendRaw(peer, TAG_FILE_MISSING, buf, pos);
    Serial.printf("[XFER] sent MISSING count=%u pos=%d\n", (unsigned)count, pos);
    return;
  }

  // Completion path: CRC-verify the preallocated file, rename, send FILE_DONE.
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (fh) { fh.flush(); fh.close(); }
  xSemaphoreGive(mtx);

  File verify = SD.open(rawInPath, FILE_READ);
  if (!verify) {
    Serial.println("[XFER] recv: reopen for crc fail");
    endTransfer(XS_FAILED);
    return;
  }
  uint32_t crc = 0;
  uint8_t tmp[512];
  while (verify.available()) {
    int n = verify.read(tmp, sizeof(tmp));
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, tmp, n);
  }
  verify.close();
  bool ok = (crc == crcExpected);

  // Rename .partial → final name (or remove on CRC failure).
  char finalPath[96];
  strlcpy(finalPath, rawInPath, sizeof(finalPath));
  int n = strlen(finalPath);
  if (n > 8 && strcmp(finalPath + n - 8, ".partial") == 0) finalPath[n - 8] = 0;
  if (ok) {
    if (SD.exists(finalPath)) SD.remove(finalPath);
    SD.rename(rawInPath, finalPath);
  } else {
    SD.remove(rawInPath);
    alertRaise(ALERT_NEW_ESPNOW, peer, 0, "file crc fail");
  }
  // Clear inPath so cleanupFile doesn't try to remove the already-renamed one.
  xSemaphoreTake(mtx, portMAX_DELAY);
  inPath[0] = 0;
  xSemaphoreGive(mtx);

  uint8_t status = ok ? 0 : 1;
  sendCtl(peer, TAG_FILE_DONE, &status, 1);
  // Inform sender we're done, even if CRC failed (they stop retransmitting).
  uint16_t zero = 0;
  sendRaw(peer, TAG_FILE_MISSING, (const uint8_t*)&zero, 2);
  endTransfer(ok ? XS_DONE : XS_FAILED);
}

// ---- sender: initial pass + repair pump ----------------------------------

static void senderTick() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (st.state != XS_SENDING && st.state != XS_REPAIR) {
    xSemaphoreGive(mtx); return;
  }
  if (!fh) {
    fh = SD.open(inPath, FILE_READ);
    if (!fh) {
      xSemaphoreGive(mtx);
      Serial.printf("[XFER] send-open fail %s\n", inPath);
      endTransfer(XS_FAILED);
      return;
    }
  }

  uint32_t now = millis();
  if ((now - tLastChunk) < CHUNK_INTERVAL_MS) { xSemaphoreGive(mtx); return; }

  // Determine the seq to send this tick.
  uint16_t seq = 0xFFFF;
  if (st.state == XS_SENDING && initialSeq < st.chunksTotal) {
    seq = initialSeq++;
    // Advance display progress during initial pass.
    st.chunksDone = initialSeq;
    uint32_t b = (uint32_t)initialSeq * XFER_CHUNK_BYTES;
    st.bytesDone = (b > st.fileSize) ? st.fileSize : b;
    if (initialSeq >= st.chunksTotal) {
      // Initial pass done. MISSING frames received during the pass were
      // lagging real state — discard them and wait for a fresh one before
      // spending airtime on repair.
      setState(XS_REPAIR);
      repairCount = 0;
      repairIdx   = 0;
      tLastMissingRx = millis();
    }
  } else if (st.state == XS_REPAIR) {
    while (repairIdx < repairCount && repairList[repairIdx].cur >= repairList[repairIdx].end) {
      repairIdx++;
    }
    if (repairIdx < repairCount) seq = repairList[repairIdx].cur++;
  }

  if (seq == 0xFFFF) { xSemaphoreGive(mtx); return; }

  uint32_t pos = (uint32_t)seq * XFER_CHUNK_BYTES;
  if (!fh.seek(pos)) {
    xSemaphoreGive(mtx);
    Serial.printf("[XFER] send seek fail pos=%lu\n", (unsigned long)pos);
    endTransfer(XS_FAILED);
    return;
  }
  uint8_t buf[XFER_CHUNK_BYTES + 2];
  memcpy(buf, &seq, 2);
  int n = fh.read(buf + 2, XFER_CHUNK_BYTES);
  if (n <= 0) {
    xSemaphoreGive(mtx);
    Serial.printf("[XFER] send read fail seq=%u\n", (unsigned)seq);
    endTransfer(XS_FAILED);
    return;
  }
  uint8_t mac[6]; memcpy(mac, st.peerMac, 6);
  xSemaphoreGive(mtx);
  sendRaw(mac, TAG_FILE_CHUNK, buf, 2 + n);
  xSemaphoreTake(mtx, portMAX_DELAY);
  tLastChunk = millis();
  xSemaphoreGive(mtx);
}

void fileXferTick() {
  // Receiver side.
  drainRxQueue();
  {
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool recvDue = (st.state == XS_RECEIVING &&
                    (millis() - tLastMissingTx) >= MISSING_INTERVAL_MS);
    if (recvDue) tLastMissingTx = millis();
    xSemaphoreGive(mtx);
    if (recvDue) receiverTickMissing();
  }

  // Sender side.
  senderTick();

  // Timeouts.
  uint32_t now = millis();
  xSemaphoreTake(mtx, portMAX_DELAY);
  XferState s = st.state;
  uint32_t since = now - tStateEnter;
  uint32_t sinceChunkRx = now - tLastChunkRx;
  uint32_t sinceMissRx  = now - tLastMissingRx;
  xSemaphoreGive(mtx);

  if (s == XS_OFFERED  && since > OFFER_TIMEOUT_MS) { fileXferDeclineIncoming(); return; }
  if (s == XS_OFFERING && since > OFFER_TIMEOUT_MS) { endTransfer(XS_FAILED);    return; }
  if (s == XS_WAIT_DONE && since > DONE_TIMEOUT_MS) { endTransfer(XS_FAILED);    return; }
  if (s == XS_RECEIVING && sinceChunkRx > RECV_STALL_MS) { endTransfer(XS_FAILED); return; }
  if (s == XS_REPAIR   && sinceMissRx > REPAIR_IDLE_MS)  { endTransfer(XS_FAILED); return; }

  if ((s == XS_DONE || s == XS_FAILED || s == XS_CANCELED) &&
      since > TERMINAL_LINGER_MS) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    memset(&st, 0, sizeof(st));
    setState(XS_IDLE);
    xSemaphoreGive(mtx);
  }
}
