// sd_safe.cpp
#include "sd_safe.h"
#include <SD.h>

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1));
  }
  return c ^ 0xFFFFFFFFu;
}
uint32_t crc32(const String& s) { return crc32((const uint8_t*)s.c_str(), s.length()); }

bool sdAppendLine(const char* path, const String& payload) {
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  String line = payload;
  line += '|';
  char hex[9];
  snprintf(hex, sizeof(hex), "%08X", crc32(payload));
  line += hex;
  line += '\n';
  size_t n = f.print(line);
  f.flush();
  f.close();
  return n == line.length();
}

bool sdAtomicWrite(const char* path, const String& fullContents) {
  String tmp = String(path) + ".new";
  File f = SD.open(tmp.c_str(), FILE_WRITE);
  if (!f) return false;
  size_t n = f.print(fullContents);
  f.flush();
  f.close();
  if (n != fullContents.length()) { SD.remove(tmp.c_str()); return false; }
  if (SD.exists(path)) SD.remove(path);
  return SD.rename(tmp.c_str(), path);
}

size_t sdSize(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

void sdRotate(const char* path, int keep) {
  String oldest = String(path) + "." + String(keep);
  if (SD.exists(oldest.c_str())) SD.remove(oldest.c_str());
  for (int i = keep - 1; i >= 1; i--) {
    String src = String(path) + "." + String(i);
    String dst = String(path) + "." + String(i + 1);
    if (SD.exists(src.c_str())) SD.rename(src.c_str(), dst.c_str());
  }
  if (SD.exists(path)) {
    String dst = String(path) + ".1";
    SD.rename(path, dst.c_str());
  }
}
