// baseline.cpp
#include "baseline.h"
#include "alerts.h"
#include "config.h"
#include "sd_safe.h"
#include "sd_writer.h"
#include <SD.h>

// After baseline is locked, we alert once per previously-unseen device.
// Keep a small ring of already-alerted MACs so the same rogue isn't
// re-announced on every scan tick.
#define NEW_ALERT_CACHE 32
static uint8_t newWifiCache[NEW_ALERT_CACHE][6];
static int     newWifiCacheN = 0;
static uint8_t newBleCache[NEW_ALERT_CACHE][6];
static int     newBleCacheN  = 0;

// Aggregate rate limit for NEW_BLE alerts. BLE advertisers commonly rotate
// their MAC (iOS, modern Android, AirTags) so the per-MAC dedupe cache above
// churns and lets the same physical device produce a flood of "new" alerts.
// Under heap pressure that flood is what tips the node into OOM — cap the
// emission rate regardless of MAC.
#define NEW_BLE_MIN_INTERVAL_MS 3000UL
static uint32_t lastNewBleAlertMs = 0;

static void parseMac(const char* s, uint8_t out[6]) {
  for (int i = 0; i < 6; i++) {
    unsigned v = 0;
    sscanf(s + i*3, "%02X", &v);
    out[i] = (uint8_t)v;
  }
}

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool cacheHasOrAdd(uint8_t cache[][6], int& n, const uint8_t mac[6]) {
  for (int i = 0; i < n; i++) if (memcmp(cache[i], mac, 6) == 0) return true;
  if (n < NEW_ALERT_CACHE) { memcpy(cache[n++], mac, 6); }
  else {
    // simple FIFO: shift-drop oldest
    memmove(cache[0], cache[1], (NEW_ALERT_CACHE - 1) * 6);
    memcpy(cache[NEW_ALERT_CACHE - 1], mac, 6);
  }
  return false;
}

struct WifiKey { char bssid[18]; char ssid[33]; uint8_t channel; uint8_t auth; };
struct BleKey  { char mac[18];   char name[33]; };

static WifiKey wifiBase[MAX_BASELINE_WIFI];
static int     wifiBaseN = 0;
static BleKey  bleBase [MAX_BASELINE_BLE];
static int     bleBaseN  = 0;

static BaselineState st = BL_LEARN;
static uint32_t tStart  = 0;
static uint32_t tLock   = 0;

static bool hasWifi(const char* bssid) {
  for (int i = 0; i < wifiBaseN; i++) if (strcmp(wifiBase[i].bssid, bssid)==0) return true;
  return false;
}
static bool hasBle(const char* mac) {
  for (int i = 0; i < bleBaseN; i++) if (strcmp(bleBase[i].mac, mac)==0) return true;
  return false;
}

// Stream each row directly to SD instead of building the whole CSV in a
// single String. The old path allocated up to ~14 KB in a contiguous String
// buffer, which on this node's ~9 KB free heap either failed outright or
// silently truncated, and sdAtomicWrite's error return was discarded so the
// loss was invisible. Streaming writes cost a few hundred bytes for SD
// library buffers, no matter how big the baseline grows.
static bool streamCsv(const char* path,
                      void (*writeAll)(File&)) {
  char tmp[40];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  writeAll(f);
  f.flush();
  f.close();
  if (SD.exists(path)) SD.remove(path);
  return SD.rename(tmp, path);
}

static void writeWifiRows(File& f) {
  for (int i = 0; i < wifiBaseN; i++) {
    f.printf("%s,%s,%u,%u\n",
      wifiBase[i].bssid, wifiBase[i].ssid,
      (unsigned)wifiBase[i].channel, (unsigned)wifiBase[i].auth);
  }
}
static void writeBleRows(File& f) {
  for (int i = 0; i < bleBaseN; i++) {
    f.printf("%s,%s\n", bleBase[i].mac, bleBase[i].name);
  }
}

static bool persist() {
  uint32_t preHeap = ESP.getFreeHeap();
  bool okW = streamCsv("/baseline_wifi.csv", writeWifiRows);
  bool okB = streamCsv("/baseline_ble.csv",  writeBleRows);
  Serial.printf("[BASELINE] persist wifi=%d(%s) ble=%d(%s) heap=%u->%u\n",
    wifiBaseN, okW ? "ok" : "FAIL",
    bleBaseN, okB ? "ok" : "FAIL",
    preHeap, ESP.getFreeHeap());
  return okW && okB;
}

static void loadFromSd() {
  File f = SD.open("/baseline_wifi.csv", FILE_READ);
  if (f) {
    while (f.available() && wifiBaseN < MAX_BASELINE_WIFI) {
      String ln = f.readStringUntil('\n'); ln.trim();
      if (ln.length() == 0) continue;
      int p1 = ln.indexOf(','), p2 = ln.indexOf(',', p1+1), p3 = ln.indexOf(',', p2+1);
      if (p1<0||p2<0||p3<0) continue;
      WifiKey k{};
      strlcpy(k.bssid, ln.substring(0,p1).c_str(), sizeof(k.bssid));
      strlcpy(k.ssid,  ln.substring(p1+1,p2).c_str(), sizeof(k.ssid));
      k.channel = ln.substring(p2+1,p3).toInt();
      k.auth    = ln.substring(p3+1).toInt();
      wifiBase[wifiBaseN++] = k;
    }
    f.close();
  }
  f = SD.open("/baseline_ble.csv", FILE_READ);
  if (f) {
    while (f.available() && bleBaseN < MAX_BASELINE_BLE) {
      String ln = f.readStringUntil('\n'); ln.trim();
      if (ln.length() == 0) continue;
      int p1 = ln.indexOf(',');
      if (p1 < 0) continue;
      BleKey k{};
      strlcpy(k.mac,  ln.substring(0,p1).c_str(), sizeof(k.mac));
      strlcpy(k.name, ln.substring(p1+1).c_str(), sizeof(k.name));
      bleBase[bleBaseN++] = k;
    }
    f.close();
  }
}

void baselineBegin() {
  tStart = millis();
  loadFromSd();
  if (wifiBaseN == 0 && bleBaseN == 0) {
    st = BL_LEARN;
  } else {
    st = BL_MONITOR;
    tLock = millis();
  }
}

void baselineTick(const WifiSeen* wifi, int nWifi,
                  const BleSeen*  ble,  int nBle) {
  if (st == BL_LEARN) {
    for (int i = 0; i < nWifi && wifiBaseN < MAX_BASELINE_WIFI; i++) {
      if (!hasWifi(wifi[i].bssid)) {
        WifiKey k{};
        strlcpy(k.bssid, wifi[i].bssid, sizeof(k.bssid));
        strlcpy(k.ssid,  wifi[i].ssid,  sizeof(k.ssid));
        k.channel = wifi[i].channel;
        k.auth    = wifi[i].auth;
        wifiBase[wifiBaseN++] = k;
      }
    }
    for (int i = 0; i < nBle && bleBaseN < MAX_BASELINE_BLE; i++) {
      if (!hasBle(ble[i].mac)) {
        BleKey k{};
        strlcpy(k.mac,  ble[i].mac,  sizeof(k.mac));
        strlcpy(k.name, ble[i].name, sizeof(k.name));
        bleBase[bleBaseN++] = k;
      }
    }
    if (millis() - tStart >= LEARN_WINDOW_MS) {
      st = BL_MONITOR;
      tLock = millis();
      persist();
      sdWriterEnqueue("/events.log",
        String(millis()) + "|BASELINE_LOCKED|wifi=" + wifiBaseN +
        ",ble=" + bleBaseN);
    }
    return;
  }

  // MONITOR: anything not in the baseline is a new sighting. De-dupe via
  // a small cache so repeat scans don't re-alert the same rogue.
  for (int i = 0; i < nWifi; i++) {
    if (hasWifi(wifi[i].bssid)) continue;
    uint8_t mac[6]; parseMac(wifi[i].bssid, mac);
    if (cacheHasOrAdd(newWifiCache, newWifiCacheN, mac)) continue;
    alertRaise(ALERT_NEW_WIFI, mac, (uint16_t)wifi[i].channel, wifi[i].ssid);
  }
  for (int i = 0; i < nBle; i++) {
    if (hasBle(ble[i].mac)) continue;
    uint8_t mac[6]; parseMac(ble[i].mac, mac);
    if (cacheHasOrAdd(newBleCache, newBleCacheN, mac)) continue;
    // Aggregate rate-limit: drop-and-cache MACs that arrive during the
    // cooldown window. Keeps randomized-MAC advert floods from draining heap
    // via the alert queue + SD writer.
    uint32_t now = millis();
    if (now - lastNewBleAlertMs < NEW_BLE_MIN_INTERVAL_MS) continue;
    lastNewBleAlertMs = now;
    alertRaise(ALERT_NEW_BLE, mac, (uint16_t)(ble[i].rssi & 0xFFFF), ble[i].name);
  }
}

BaselineState baselineStateNow()   { return st; }
uint32_t baselineMsRemaining() {
  if (st != BL_LEARN) return 0;
  uint32_t e = millis() - tStart;
  return (e >= LEARN_WINDOW_MS) ? 0 : (LEARN_WINDOW_MS - e);
}
uint32_t baselineLockMs()          { return tLock; }
bool baselineContainsWifi(const char* b) { return hasWifi(b); }
bool baselineContainsBle (const char* m) { return hasBle(m);  }
int  baselineWifiCount()           { return wifiBaseN; }
int  baselineBleCount()            { return bleBaseN;  }

void baselineRelearn() {
  wifiBaseN = 0; bleBaseN = 0;
  newWifiCacheN = 0; newBleCacheN = 0;
  tStart = millis();
  st = BL_LEARN;
  if (SD.exists("/baseline_wifi.csv")) SD.remove("/baseline_wifi.csv");
  if (SD.exists("/baseline_ble.csv"))  SD.remove("/baseline_ble.csv");
}

bool baselineAddFromAlert(const Alert& a) {
  bool added = false;
  if (a.type == ALERT_NEW_WIFI && wifiBaseN < MAX_BASELINE_WIFI) {
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
      a.mac[0], a.mac[1], a.mac[2], a.mac[3], a.mac[4], a.mac[5]);
    if (!hasWifi(bssid)) {
      WifiKey k{};
      strlcpy(k.bssid, bssid, sizeof(k.bssid));
      strlcpy(k.ssid, a.label, sizeof(k.ssid));
      k.channel = (uint8_t)(a.extra & 0xFF);
      k.auth = 0;
      wifiBase[wifiBaseN++] = k;
      added = true;
    }
  } else if (a.type == ALERT_NEW_BLE && bleBaseN < MAX_BASELINE_BLE) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
      a.mac[0], a.mac[1], a.mac[2], a.mac[3], a.mac[4], a.mac[5]);
    if (!hasBle(mac)) {
      BleKey k{};
      strlcpy(k.mac, mac, sizeof(k.mac));
      strlcpy(k.name, a.label, sizeof(k.name));
      bleBase[bleBaseN++] = k;
      added = true;
    }
  }
  if (!added) return false;

  cacheHasOrAdd(newWifiCache, newWifiCacheN, a.mac);
  cacheHasOrAdd(newBleCache, newBleCacheN, a.mac);
  if (persist()) {
    String row = String(millis()) + "|BASELINE_ADD|" + alertTypeName(a.type);
    sdWriterEnqueue("/events.log", row);
  }
  return true;
  if (a.type != ALERT_NEW_WIFI && a.type != ALERT_NEW_BLE) return false;
  char mac[18];
  macToStr(a.mac, mac);

  if (a.type == ALERT_NEW_WIFI) {
    if (hasWifi(mac)) return true;
    if (wifiBaseN >= MAX_BASELINE_WIFI) return false;
    WifiKey k{};
    strlcpy(k.bssid, mac, sizeof(k.bssid));
    strlcpy(k.ssid,  a.label, sizeof(k.ssid));
    k.channel = (uint8_t)(a.extra & 0xFF);
    k.auth = 0;
    wifiBase[wifiBaseN++] = k;
  } else {
    if (hasBle(mac)) return true;
    if (bleBaseN >= MAX_BASELINE_BLE) return false;
    BleKey k{};
    strlcpy(k.mac, mac, sizeof(k.mac));
    strlcpy(k.name, a.label, sizeof(k.name));
    bleBase[bleBaseN++] = k;
  }
  return persist();
}
