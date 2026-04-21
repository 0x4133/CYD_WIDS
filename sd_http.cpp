#include "sd_http.h"

#include "sd_writer.h"
#include <Arduino.h>
#include <SD.h>
#include <mbedtls/base64.h>

static bool gRunning = false;
static char gUrl[64] = "";

static File gUploadFile;
static uint32_t gUploadRemain = 0;
static uint32_t gUploadTotal = 0;
static char gUploadPath[96] = "";

static char gLine[512];
static uint16_t gLineLen = 0;

static void sanitizeName(const char* in, char* out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < cap; i++) {
    char c = in[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') continue;
    out[j++] = c;
  }
  if (j == 0) {
    strlcpy(out, "upload.bin", cap);
    return;
  }
  out[j] = 0;
}

static void printHelp() {
  Serial.println("[SDSER] commands:");
  Serial.println("[SDSER]   HELP");
  Serial.println("[SDSER]   LS");
  Serial.println("[SDSER]   GET <name>");
  Serial.println("[SDSER]   DEL <name>");
  Serial.println("[SDSER]   PUT <name> <size>  (then DATA <base64>... and PUT END)");
}

static bool b64EncodeAndPrint(const uint8_t* data, size_t len) {
  // Encoded length = ceil(len/3)*4.
  uint8_t out[350];
  size_t outLen = 0;
  int rc = mbedtls_base64_encode(out, sizeof(out), &outLen, data, len);
  if (rc != 0) return false;
  Serial.print("[SDSER] DATA ");
  Serial.write(out, outLen);
  Serial.println();
  return true;
}

static bool b64Decode(const char* in, uint8_t* out, size_t outCap, size_t* outLen) {
  if (!in || !out || !outLen) return false;
  int rc = mbedtls_base64_decode(out, outCap, outLen,
                                 (const uint8_t*)in, strlen(in));
  return rc == 0;
}

static void listRoot() {
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("[SDSER] ERR root");
    return;
  }
  Serial.println("[SDSER] LS BEGIN");
  File f;
  while ((f = root.openNextFile())) {
    const char* n = f.name();
    const char* base = strrchr(n, '/');
    base = base ? base + 1 : n;
    if (base && base[0]) {
      Serial.printf("[SDSER] %c %s %lu\n", f.isDirectory() ? 'D' : 'F',
                    base, (unsigned long)f.size());
    }
    f.close();
  }
  root.close();
  Serial.println("[SDSER] LS END");
}

static void doGet(const char* arg) {
  char clean[48];
  sanitizeName(arg, clean, sizeof(clean));
  char path[64];
  snprintf(path, sizeof(path), "/%s", clean);
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    Serial.printf("[SDSER] ERR not-found %s\n", clean);
    return;
  }
  uint32_t n = f.size();
  Serial.printf("[SDSER] GET BEGIN %s %lu\n", clean, (unsigned long)n);
  uint8_t buf[180];
  while (f.available()) {
    int r = f.read(buf, sizeof(buf));
    if (r <= 0) break;
    if (!b64EncodeAndPrint(buf, (size_t)r)) {
      f.close();
      Serial.println("[SDSER] ERR b64-encode");
      return;
    }
  }
  f.close();
  Serial.println("[SDSER] GET END");
}

static void doDelete(const char* arg) {
  char clean[48];
  sanitizeName(arg, clean, sizeof(clean));
  char path[64];
  snprintf(path, sizeof(path), "/%s", clean);
  if (!SD.exists(path)) {
    Serial.printf("[SDSER] ERR not-found %s\n", clean);
    return;
  }
  bool ok = SD.remove(path);
  Serial.printf("[SDSER] %s %s\n", ok ? "OK DEL" : "ERR DEL", clean);
}

static void beginPut(const char* arg) {
  char name[48] = {0};
  unsigned long size = 0;
  if (sscanf(arg, "%47s %lu", name, &size) != 2 || size == 0) {
    Serial.println("[SDSER] ERR usage: PUT <name> <size>");
    return;
  }
  char clean[48];
  sanitizeName(name, clean, sizeof(clean));
  snprintf(gUploadPath, sizeof(gUploadPath), "/%s", clean);
  if (SD.exists(gUploadPath)) SD.remove(gUploadPath);

  if (gUploadFile) gUploadFile.close();
  gUploadFile = SD.open(gUploadPath, FILE_WRITE);
  if (!gUploadFile) {
    gUploadPath[0] = 0;
    Serial.printf("[SDSER] ERR open %s\n", clean);
    return;
  }
  gUploadRemain = (uint32_t)size;
  gUploadTotal  = (uint32_t)size;
  Serial.printf("[SDSER] PUT READY %s %lu\n", clean, size);
}

static void handleCommand(const char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (!*line) return;
  if (strcasecmp(line, "HELP") == 0) { printHelp(); return; }
  if (strcasecmp(line, "LS") == 0) { listRoot(); return; }

  if (strncasecmp(line, "PUT END", 7) == 0) {
    if (gUploadTotal == 0) {
      Serial.println("[SDSER] ERR no-put");
      return;
    }
    if (gUploadRemain != 0) {
      gUploadFile.close();
      SD.remove(gUploadPath);
      Serial.printf("[SDSER] ERR put-size expected=%lu remain=%lu\n",
                    (unsigned long)gUploadTotal, (unsigned long)gUploadRemain);
      gUploadRemain = 0;
      gUploadTotal = 0;
      gUploadPath[0] = 0;
      return;
    }
    gUploadFile.close();
    Serial.printf("[SDSER] PUT OK %s\n", gUploadPath + 1);
    gUploadRemain = 0;
    gUploadTotal = 0;
    gUploadPath[0] = 0;
    return;
  }

  if (strncasecmp(line, "GET ", 4) == 0) { doGet(line + 4); return; }
  if (strncasecmp(line, "DEL ", 4) == 0) { doDelete(line + 4); return; }
  if (strncasecmp(line, "PUT ", 4) == 0) { beginPut(line + 4); return; }

  if (gUploadRemain > 0 && strncasecmp(line, "DATA ", 5) == 0) {
    const char* b64 = line + 5;
    uint8_t raw[192];
    size_t rawLen = 0;
    if (!b64Decode(b64, raw, sizeof(raw), &rawLen) || rawLen == 0) {
      gUploadFile.close();
      SD.remove(gUploadPath);
      Serial.println("[SDSER] ERR put-b64");
      gUploadRemain = 0;
      gUploadTotal = 0;
      gUploadPath[0] = 0;
      return;
    }
    if (rawLen > gUploadRemain) {
      gUploadFile.close();
      SD.remove(gUploadPath);
      Serial.println("[SDSER] ERR put-overflow");
      gUploadRemain = 0;
      gUploadTotal = 0;
      gUploadPath[0] = 0;
      return;
    }
    size_t w = gUploadFile.write(raw, rawLen);
    if (w != rawLen) {
      gUploadFile.close();
      SD.remove(gUploadPath);
      Serial.println("[SDSER] ERR put-write");
      gUploadRemain = 0;
      gUploadTotal = 0;
      gUploadPath[0] = 0;
      return;
    }
    gUploadRemain -= (uint32_t)rawLen;
    return;
  }
  Serial.printf("[SDSER] ERR unknown: %s\n", line);
}

void sdHttpBegin() {
  if (!sdWriterReady()) {
    Serial.println("[SDSER] SD not ready");
    return;
  }
  strlcpy(gUrl, "serial://usb", sizeof(gUrl));
  gRunning = true;
  Serial.println("[SDSER] ready over USB serial");
  printHelp();
}

void sdHttpTick() {
  if (!gRunning) return;

  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      gLine[gLineLen] = 0;
      handleCommand(gLine);
      gLineLen = 0;
      continue;
    }
    if (gLineLen + 1 < sizeof(gLine)) gLine[gLineLen++] = (char)c;
  }
}

bool sdHttpRunning() { return gRunning; }

const char* sdHttpUrl() { return gUrl; }
