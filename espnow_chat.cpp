// espnow_chat.cpp
#include "espnow_chat.h"
#include "contacts.h"
#include "crypto.h"
#include "config.h"
#include "sd_writer.h"
#include "file_xfer.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// Frame tags (first byte of payload):
//   0x02 HELLO    — presence beacon (no UI entry)
//   0x03 KEY_REQ  — unicast, 32-byte X25519 pubkey (initiator → target)
//   0x04 KEY_RESP — unicast, 32-byte X25519 pubkey (target reply)
//   0x05 CHAT_E   — unicast, [seq:1][nonce:12][ciphertext:N][gcm_tag:16]
//                   AES-128-GCM, key = HKDF(X25519 shared). TLS-style AEAD.
#define TAG_HELLO    0x02
#define TAG_KEY_REQ  0x03
#define TAG_KEY_RESP 0x04
#define TAG_CHAT_E   0x05

// Auto-resend policy: ESP-NOW gives a send callback with status. For unicast
// frames we retry up to this many times on FAIL with a delay between attempts.
// The window must exceed a typical WiFi scan on the peer (~2-3s), otherwise
// a single scan can eat every retry and the key exchange / file control
// frame is lost.
#define SEND_MAX_RETRIES 8
#define SEND_RETRY_MS    450

// Chat is unicast+encrypted; retries come from the unicast send-callback
// path (SEND_MAX_RETRIES above). Only HELLO is broadcast now and it's
// advisory — no retry needed.
#define DEDUP_N          16    // remembered (mac,seq) pairs (for CHAT_E)

#define MAX_CHAT_PEERS 8

static SemaphoreHandle_t mtx;
static ChatMsg chatLog[CHAT_LOG_MAX];
static int logHead = 0;
static int logCount = 0;
static volatile uint32_t recvCount = 0;
static uint8_t myMac[6] = {0};

static uint8_t  peers[MAX_CHAT_PEERS][6];
static uint32_t peerLastMs[MAX_CHAT_PEERS];
static int      peerN = 0;

static volatile uint32_t sendOkCount = 0;
static volatile uint32_t sendFailCount = 0;

// Last-activity timestamps for diagnostics. If lastSendCbMs stops advancing
// while lastSendMs keeps moving, the WiFi driver stopped delivering send
// callbacks — a classic coex / internal-queue-stall signature. 0 = never.
static volatile uint32_t lastSendMs   = 0;
static volatile uint32_t lastSendCbMs = 0;

// Pending unicast send — the send callback uses this to decide whether to
// retry from a deferred context (FreeRTOS timer) on FAIL.
#define PENDING_BUF_MAX (1 + 1 + 12 + CHAT_MSG_MAX + 16)
static struct {
  uint8_t dest[6];
  uint8_t buf[PENDING_BUF_MAX];
  uint8_t len;
  uint8_t retriesLeft;
  bool    active;
} pending = {};

static TimerHandle_t retryTimer = nullptr;

static uint8_t mySeq = 0;

// Cooldown on channel resnaps. A WiFi scan leaves us off-channel for ~2-3s,
// and during that window HELLOs we receive (or send) will observe a mismatch
// and try to snap back — which yanks the radio out from under the in-flight
// scan and thrashes coex buffers. Rate-limit to stop that churn.
#define RESNAP_MIN_INTERVAL_MS 5000UL
static uint32_t lastResnapMs = 0;

static void maybeResnapChannel(const char* why) {
  uint8_t cur = 0; wifi_second_chan_t dummy;
  if (esp_wifi_get_channel(&cur, &dummy) != ESP_OK) return;
  if (cur == ESPNOW_CHANNEL) return;
  uint32_t now = millis();
  if (now - lastResnapMs < RESNAP_MIN_INTERVAL_MS) return;
  lastResnapMs = now;
  Serial.printf("[CHAT] %s resnap %u -> %u\n", why, cur, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

// Current unicast chat target (MAC of a contact with hasKey=true). All-zero
// means "no target" — chatSend then refuses. Set via chatSetTarget().
static uint8_t chatTarget[6] = {0};

// RX dedup: remember the last DEDUP_N (mac,seq) pairs; drop repeats.
static struct { uint8_t mac[6]; uint8_t seq; } seen[DEDUP_N] = {};
static int seenHead = 0;

static bool seenLookupOrAdd(const uint8_t mac[6], uint8_t seq) {
  // mtx must be held by caller
  for (int i = 0; i < DEDUP_N; i++) {
    if (seen[i].seq == seq && memcmp(seen[i].mac, mac, 6) == 0) return true;
  }
  memcpy(seen[seenHead].mac, mac, 6);
  seen[seenHead].seq = seq;
  seenHead = (seenHead + 1) % DEDUP_N;
  return false;
}

static bool sendFramedTo(const uint8_t* dest, uint8_t tag,
                         const uint8_t* payload, int plen);
static bool targetIsSet();

static void macToStr(const uint8_t* m, char* out /*18*/) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
    m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void registerPeer(const uint8_t* mac) {
  // mtx must be held by caller
  for (int i = 0; i < peerN; i++) {
    if (memcmp(peers[i], mac, 6) == 0) { peerLastMs[i] = millis(); return; }
  }
  if (peerN < MAX_CHAT_PEERS) {
    memcpy(peers[peerN], mac, 6);
    peerLastMs[peerN] = millis();
    peerN++;
    char m[18]; macToStr(mac, m);
    Serial.printf("[CHAT] new peer %s (total %d)\n", m, peerN);
  }
}

static void appendLog(const uint8_t mac[6], bool mine, const char* text, int len) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  ChatMsg& m = chatLog[logHead];
  memcpy(m.mac, mac, 6);
  m.mine = mine;
  m.tMs  = millis();
  int n = min(len, CHAT_MSG_MAX);
  memcpy(m.text, text, n);
  m.text[n] = 0;
  logHead = (logHead + 1) % CHAT_LOG_MAX;
  if (logCount < CHAT_LOG_MAX) logCount++;
  xSemaphoreGive(mtx);
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!data || len < 1) return;
  recvCount++;

  // Every received frame — chat or hello — marks the sender as a live peer.
  xSemaphoreTake(mtx, portMAX_DELAY);
  registerPeer(info->src_addr);
  xSemaphoreGive(mtx);
  contactsTouch(info->src_addr);

  uint8_t tag = data[0];
  const uint8_t* payload = data + 1;
  int plen = len - 1;

  if (tag == TAG_HELLO) {
    // HELLO carries the sender's home channel as a single byte. If we've
    // drifted off it (e.g. a failed WiFi scan restore), snap back — but
    // rate-limited so a legitimate mid-scan window isn't clobbered.
    if (plen >= 1) maybeResnapChannel("HELLO-rx");
    return;
  }
  if (tag >= TAG_FILE_OFFER && tag <= TAG_FILE_MISSING) {
    fileXferOnFrame(info->src_addr, tag, payload, plen);
    return;
  }
  if (tag == TAG_KEY_REQ || tag == TAG_KEY_RESP) {
    if (plen != CRYPTO_KEY_LEN) return;
    contactsSetPubkey(info->src_addr, payload);
    char mac[18]; macToStr(info->src_addr, mac);
    Serial.printf("[CHAT] got %s pubkey from %s\n",
      tag == TAG_KEY_REQ ? "REQ" : "RESP", mac);
    if (tag == TAG_KEY_REQ && cryptoReady()) {
      // Reply with our pubkey so the initiator can complete the exchange.
      uint8_t ourPub[CRYPTO_KEY_LEN];
      cryptoGetOurPubkey(ourPub);
      chatEnsurePeer(info->src_addr);
      sendFramedTo(info->src_addr, TAG_KEY_RESP, ourPub, CRYPTO_KEY_LEN);
    }
    return;
  }
  if (tag != TAG_CHAT_E) return;   // unknown tag — ignore
  // [seq:1][nonce:12][ct:N][gcm_tag:16]
  if (plen < 1 + CRYPTO_AEAD_OVERHEAD) return;
  uint8_t seq = payload[0];
  const uint8_t* sealed = payload + 1;
  int sealedLen = plen - 1;

  // Dedup on (mac, seq). Separate from file-xfer so file frames don't evict
  // chat entries and vice versa.
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool dup = seenLookupOrAdd(info->src_addr, seq);
  xSemaphoreGive(mtx);
  if (dup) return;

  // Pull the sender's pubkey from contacts. Without it we can't derive the
  // shared secret — silently drop (an alert could be raised here later).
  int ci = contactsFindByMac(info->src_addr);
  if (ci < 0) return;
  const Contact* c = contactsGet(ci);
  if (!c || !c->hasKey) return;

  uint8_t plain[CHAT_MSG_MAX + 1] = {0};
  int plainLen = 0;
  // Prefer the cached AES key (no ECDH in the per-packet path). Fall back to
  // the pubkey variant for contacts loaded before cryptoBegin() succeeded.
  bool opened = c->hasAesKey
    ? cryptoOpenWithKey(c->aesKey, sealed, sealedLen,
                        plain, sizeof(plain) - 1, &plainLen)
    : cryptoOpenFromPeer(c->pubkey, sealed, sealedLen,
                         plain, sizeof(plain) - 1, &plainLen);
  if (!opened) {
    Serial.println("[CHAT] AEAD open failed — tamper or wrong key");
    return;
  }
  plain[plainLen] = 0;

  appendLog(info->src_addr, false, (const char*)plain, plainLen);

  // Build the log row in a single stack buffer. The previous implementation
  // did ~plainLen+5 heap allocations per received chat frame (String "+",
  // "+=" per plaintext char), which is exactly the per-packet malloc path
  // that starved AEAD of heap during bursts.
  char mac[18]; macToStr(info->src_addr, mac);
  char row[48 + CHAT_MSG_MAX + 1];
  int off = snprintf(row, sizeof(row),
    "%lu|CHAT-RX|from=%s,seq=%u,len=%d,text=",
    (unsigned long)millis(), mac, (unsigned)seq, plainLen);
  int cap = (int)sizeof(row) - 1 - off;
  int copyN = plainLen < cap ? plainLen : cap;
  for (int i = 0; i < copyN; i++) {
    char ch = (char)plain[i];
    row[off + i] = (ch < 32 || ch > 126) ? '.' : ch;
  }
  row[off + copyN] = 0;
  sdWriterEnqueue("/espnow_chat.log", String(row));
}

// Retry timer fires on the timer service task — safe to call esp_now_send.
static void retryTimerCb(TimerHandle_t) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!pending.active) { xSemaphoreGive(mtx); return; }
  uint8_t dest[6]; memcpy(dest, pending.dest, 6);
  uint8_t buf[PENDING_BUF_MAX]; memcpy(buf, pending.buf, pending.len);
  uint8_t len = pending.len;
  uint8_t left = pending.retriesLeft;
  xSemaphoreGive(mtx);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  lastSendMs = millis();
  esp_err_t r = esp_now_send(dest, buf, len);
  if (r != ESP_OK) {
    char m[18]; macToStr(dest, m);
    Serial.printf("[CHAT] retry send to %s left=%u err=%d(%s)\n",
                  m, (unsigned)left, (int)r, esp_err_to_name(r));
  }
}

static void onSend(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  if (!info || !info->des_addr) return;
  lastSendCbMs = millis();
  const uint8_t* mac = info->des_addr;
  bool isBcast = (memcmp(mac, BCAST, 6) == 0);
  if (status == ESP_NOW_SEND_SUCCESS) {
    sendOkCount++;
    if (!isBcast) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      if (pending.active && memcmp(pending.dest, mac, 6) == 0) pending.active = false;
      xSemaphoreGive(mtx);
    }
    return;
  }
  // FAIL
  sendFailCount++;
  if (isBcast) return;  // broadcast has no ACK — retries don't help
  bool retry = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (pending.active && memcmp(pending.dest, mac, 6) == 0) {
    if (pending.retriesLeft > 0) { pending.retriesLeft--; retry = true; }
    else                         { pending.active = false; }
  }
  xSemaphoreGive(mtx);
  if (retry && retryTimer) xTimerChangePeriod(retryTimer, pdMS_TO_TICKS(SEND_RETRY_MS), 0);
}

bool chatBegin() {
  mtx = xSemaphoreCreateMutex();
  WiFi.macAddress(myMac);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[CHAT] esp_now_init failed");
    return false;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t ar = esp_now_add_peer(&peer);
  if (ar != ESP_OK && ar != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[CHAT] add_peer err=%d\n", ar);
    return false;
  }
  esp_now_register_recv_cb(&onRecv);
  esp_now_register_send_cb(&onSend);
  retryTimer = xTimerCreate("chatRetry", pdMS_TO_TICKS(SEND_RETRY_MS),
                            pdFALSE, nullptr, retryTimerCb);
  Serial.printf("[CHAT] ready mac=%02X:%02X:%02X:%02X:%02X:%02X ch=%d\n",
    myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5], ESPNOW_CHANNEL);
  return true;
}

bool chatEnsurePeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = ESPNOW_CHANNEL;
  p.ifidx   = WIFI_IF_STA;
  p.encrypt = false;
  esp_err_t r = esp_now_add_peer(&p);
  bool ok = (r == ESP_OK || r == ESP_ERR_ESPNOW_EXIST);
  if (!ok) {
    char m[18]; macToStr(mac, m);
    Serial.printf("[CHAT] add_peer %s err=%d(%s)\n",
                  m, (int)r, esp_err_to_name(r));
  }
  return ok;
}

static bool sendFramedTo(const uint8_t* dest, uint8_t tag,
                         const uint8_t* payload, int plen) {
  uint8_t buf[PENDING_BUF_MAX];
  if (plen > (int)(sizeof(buf) - 1)) plen = sizeof(buf) - 1;
  buf[0] = tag;
  if (plen > 0) memcpy(buf + 1, payload, plen);
  int total = plen + 1;

  bool isBcast = (memcmp(dest, BCAST, 6) == 0);
  if (!isBcast) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    memcpy(pending.dest, dest, 6);
    memcpy(pending.buf, buf, total);
    pending.len = total;
    pending.retriesLeft = SEND_MAX_RETRIES;
    pending.active = true;
    xSemaphoreGive(mtx);
  }
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  lastSendMs = millis();
  esp_err_t r = esp_now_send(dest, buf, total);
  if (r != ESP_OK) {
    // Surface the real error name. ESP-NOW returns things like
    // ESP_ERR_ESPNOW_NO_MEM (heap too tight), _NOT_FOUND (peer gone),
    // _IF (wrong interface) — all of which otherwise look like silent
    // drops from the caller's perspective.
    char m[18]; macToStr(dest, m);
    Serial.printf("[CHAT] esp_now_send to %s tag=0x%02X err=%d(%s)\n",
                  m, tag, (int)r, esp_err_to_name(r));
  }
  return r == ESP_OK;
}

static bool sendFramed(uint8_t tag, const uint8_t* payload, int plen) {
  return sendFramedTo(BCAST, tag, payload, plen);
}

bool chatRequestKey(const uint8_t mac[6]) {
  if (!cryptoReady()) return false;
  if (!chatEnsurePeer(mac)) return false;
  uint8_t ourPub[CRYPTO_KEY_LEN];
  cryptoGetOurPubkey(ourPub);
  return sendFramedTo(mac, TAG_KEY_REQ, ourPub, CRYPTO_KEY_LEN);
}

uint32_t chatSendOk()   { return sendOkCount; }
uint32_t chatSendFail() { return sendFailCount; }

void chatDiagPrint() {
  uint8_t ch = 0; wifi_second_chan_t sec;
  if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) ch = 0;

  uint32_t now = millis();
  // "never" (value 0) shows as -1 so we don't mistake 0-ms-ago for never.
  int32_t sinceSend  = lastSendMs   ? (int32_t)(now - lastSendMs)   : -1;
  int32_t sinceCb    = lastSendCbMs ? (int32_t)(now - lastSendCbMs) : -1;

  bool pActive;
  uint8_t pDest[6];
  uint8_t pRetries;
  xSemaphoreTake(mtx, portMAX_DELAY);
  pActive  = pending.active;
  memcpy(pDest, pending.dest, 6);
  pRetries = pending.retriesLeft;
  xSemaphoreGive(mtx);

  char dm[18] = "--";
  if (pActive) macToStr(pDest, dm);

  Serial.printf(
    "[CHAT-DIAG] ch=%u ok=%lu fail=%lu lastTx=%ldms lastCb=%ldms "
    "pending=%s dest=%s retries=%u peers=%d tgt=%s\n",
    ch,
    (unsigned long)sendOkCount, (unsigned long)sendFailCount,
    (long)sinceSend, (long)sinceCb,
    pActive ? "yes" : "no", dm, (unsigned)pRetries,
    peerN, targetIsSet() ? "set" : "none");
}

static bool targetIsSet() {
  for (int i = 0; i < 6; i++) if (chatTarget[i]) return true;
  return false;
}

void chatSetTarget(const uint8_t mac[6]) {
  if (!mac) { memset(chatTarget, 0, 6); return; }
  memcpy(chatTarget, mac, 6);
}
bool chatHasTarget() { return targetIsSet(); }
void chatGetTarget(uint8_t out[6]) { memcpy(out, chatTarget, 6); }

bool chatSend(const char* text) {
  if (!text || !text[0]) return false;
  if (!targetIsSet()) {
    Serial.println("[CHAT] send blocked: no target selected");
    return false;
  }
  int ci = contactsFindByMac(chatTarget);
  if (ci < 0) {
    Serial.println("[CHAT] send blocked: target not in contacts");
    return false;
  }
  const Contact* c = contactsGet(ci);
  if (!c || !c->hasKey) {
    Serial.println("[CHAT] send blocked: target has no pubkey");
    return false;
  }
  if (!cryptoReady()) {
    Serial.println("[CHAT] send blocked: crypto not ready");
    return false;
  }

  int len = strlen(text);
  if (len > CHAT_MSG_MAX) len = CHAT_MSG_MAX;

  // Build the sealed payload: [seq:1][nonce:12][ct:len][gcm_tag:16]
  uint8_t payload[1 + 12 + CHAT_MSG_MAX + 16];
  uint8_t seq = ++mySeq;
  payload[0] = seq;
  int sealedLen = 0;
  bool sealed = c->hasAesKey
    ? cryptoSealWithKey(c->aesKey, (const uint8_t*)text, len,
                        payload + 1, sizeof(payload) - 1, &sealedLen)
    : cryptoSealForPeer(c->pubkey, (const uint8_t*)text, len,
                        payload + 1, sizeof(payload) - 1, &sealedLen);
  if (!sealed) {
    Serial.println("[CHAT] AEAD seal failed");
    return false;
  }
  // Seed the dedup cache so an echoed-back frame doesn't log twice.
  xSemaphoreTake(mtx, portMAX_DELAY);
  seenLookupOrAdd(myMac, seq);
  xSemaphoreGive(mtx);

  if (!chatEnsurePeer(chatTarget)) return false;
  bool ok = sendFramedTo(chatTarget, TAG_CHAT_E, payload, 1 + sealedLen);
  Serial.printf("[CHAT] send seq=%u plain=%d sealed=%d %s\n",
                seq, len, sealedLen, ok ? "ok" : "fail");
  if (ok) {
    appendLog(chatTarget, true, text, len);
    char mac[18]; macToStr(chatTarget, mac);
    String row = String(millis()) + "|CHAT-TX|to=" + mac +
                 ",seq=" + String(seq) + ",len=" + String(len) +
                 ",text=" + String(text);
    sdWriterEnqueue("/espnow_chat.log", row);
  }
  return ok;
}

bool chatSendHello() {
  // Defensive: if anything left us off-channel, snap back before announcing —
  // but honor the resnap cooldown so we don't interrupt a legitimate scan.
  maybeResnapChannel("HELLO-tx");
  uint8_t ch = ESPNOW_CHANNEL;
  return sendFramed(TAG_HELLO, &ch, 1);
}

int chatSnapshot(ChatMsg* out, int maxOut) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(logCount, maxOut);
  for (int i = 0; i < n; i++) {
    int idx = (logHead - 1 - i + CHAT_LOG_MAX) % CHAT_LOG_MAX;
    out[i] = chatLog[idx];
  }
  xSemaphoreGive(mtx);
  return n;
}

uint32_t chatRecvCount() { return recvCount; }
int      chatPeerCount() { return peerN; }

int chatPeerList(uint8_t outMac[][6], uint32_t* outLastMs, int maxOut) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = min(peerN, maxOut);
  for (int i = 0; i < n; i++) {
    memcpy(outMac[i], peers[i], 6);
    outLastMs[i] = peerLastMs[i];
  }
  xSemaphoreGive(mtx);
  return n;
}
