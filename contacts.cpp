// contacts.cpp
#include "contacts.h"
#include "crypto.h"
#include "sd_safe.h"
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Derive the cached AES key for a contact that has a pubkey. Safe to call
// any time after cryptoBegin(); silently no-ops if crypto isn't ready or the
// derivation fails (falls back to per-packet ECDH via cryptoSeal/OpenForPeer).
static void deriveContactAes(Contact& c) {
  c.hasAesKey = false;
  if (!c.hasKey) return;
  if (!cryptoReady()) return;
  if (cryptoDeriveAesForPeer(c.pubkey, c.aesKey)) c.hasAesKey = true;
}

static SemaphoreHandle_t mtx;
static Contact list[MAX_CONTACTS];
static int count = 0;

static void macToStr(const uint8_t* m, char* out /*18*/) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
    m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void macFromStr(const char* s, uint8_t out[6]) {
  for (int i = 0; i < 6; i++) {
    unsigned v = 0;
    sscanf(s + i*3, "%02X", &v);
    out[i] = (uint8_t)v;
  }
}

static void hexToBytes(const char* s, uint8_t* out, int n) {
  for (int i = 0; i < n; i++) {
    unsigned v = 0;
    sscanf(s + i*2, "%02X", &v);
    out[i] = (uint8_t)v;
  }
}

static void bytesToHex(const uint8_t* in, int n, char* out /*2n+1*/) {
  static const char H[] = "0123456789ABCDEF";
  for (int i = 0; i < n; i++) {
    out[i*2]     = H[(in[i] >> 4) & 0xF];
    out[i*2 + 1] = H[in[i] & 0xF];
  }
  out[n*2] = 0;
}

static void loadFromSd() {
  // Line format (v2): MAC,NAME,HEX_PUBKEY  — pubkey column may be empty.
  File f = SD.open("/contacts.csv", FILE_READ);
  if (!f) return;
  while (f.available() && count < MAX_CONTACTS) {
    String ln = f.readStringUntil('\n'); ln.trim();
    if (ln.length() < 18) continue;
    int p1 = ln.indexOf(',');
    if (p1 != 17) continue;
    int p2 = ln.indexOf(',', p1 + 1);
    Contact c{};
    macFromStr(ln.substring(0, 17).c_str(), c.mac);
    String nm = (p2 > 0) ? ln.substring(p1 + 1, p2) : ln.substring(p1 + 1);
    strlcpy(c.name, nm.c_str(), sizeof(c.name));
    if (p2 > 0) {
      String hex = ln.substring(p2 + 1);
      if (hex.length() >= 64) { hexToBytes(hex.c_str(), c.pubkey, 32); c.hasKey = true; }
    }
    c.lastSeenMs = 0;
    deriveContactAes(c);
    list[count++] = c;
  }
  f.close();
}

void contactsBegin() {
  mtx = xSemaphoreCreateMutex();
  count = 0;
  loadFromSd();
  Serial.printf("[CONTACTS] loaded %d\n", count);
}

int contactsCount() { return count; }

const Contact* contactsGet(int idx) {
  if (idx < 0 || idx >= count) return nullptr;
  return &list[idx];
}

int contactsFindByMac(const uint8_t mac[6]) {
  for (int i = 0; i < count; i++)
    if (memcmp(list[i].mac, mac, 6) == 0) return i;
  return -1;
}

bool contactsAdd(const uint8_t mac[6], const char* name) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int existing = contactsFindByMac(mac);
  if (existing >= 0) {
    if (name && name[0]) strlcpy(list[existing].name, name, sizeof(list[existing].name));
    xSemaphoreGive(mtx);
    contactsSave();
    return true;
  }
  if (count >= MAX_CONTACTS) { xSemaphoreGive(mtx); return false; }
  Contact& c = list[count++];
  memcpy(c.mac, mac, 6);
  strlcpy(c.name, (name && name[0]) ? name : "unnamed", sizeof(c.name));
  c.lastSeenMs = millis();
  xSemaphoreGive(mtx);
  contactsSave();
  return true;
}

bool contactsRename(int idx, const char* name) {
  if (idx < 0 || idx >= count || !name || !name[0]) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  strlcpy(list[idx].name, name, sizeof(list[idx].name));
  xSemaphoreGive(mtx);
  contactsSave();
  return true;
}

bool contactsRemove(int idx) {
  if (idx < 0 || idx >= count) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = idx; i < count - 1; i++) list[i] = list[i + 1];
  count--;
  xSemaphoreGive(mtx);
  contactsSave();
  return true;
}

void contactsTouch(const uint8_t mac[6]) {
  int i = contactsFindByMac(mac);
  if (i >= 0) list[i].lastSeenMs = millis();
}

void contactsSave() {
  String body; body.reserve(count * 100);
  char macs[18], keyhex[65];
  for (int i = 0; i < count; i++) {
    macToStr(list[i].mac, macs);
    body += macs; body += ',';
    body += list[i].name; body += ',';
    if (list[i].hasKey) { bytesToHex(list[i].pubkey, 32, keyhex); body += keyhex; }
    body += '\n';
  }
  sdAtomicWrite("/contacts.csv", body);
}

bool contactsSetPubkey(const uint8_t mac[6], const uint8_t pub[32]) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int idx = contactsFindByMac(mac);
  if (idx < 0) {
    if (count >= MAX_CONTACTS) { xSemaphoreGive(mtx); return false; }
    Contact& c = list[count++];
    memcpy(c.mac, mac, 6);
    strlcpy(c.name, "unnamed", sizeof(c.name));
    c.lastSeenMs = millis();
    idx = count - 1;
  }
  memcpy(list[idx].pubkey, pub, 32);
  list[idx].hasKey = true;
  deriveContactAes(list[idx]);
  xSemaphoreGive(mtx);
  contactsSave();
  return true;
}
