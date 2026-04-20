// sd_safe.h
#pragma once
#include <Arduino.h>

uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32(const String& s);

// Append "<payload>|<CRC32-hex>\n" to `path`. Flushes and closes every call.
bool sdAppendLine(const char* path, const String& payload);

// Atomic-ish overwrite: writes `path.new`, flushes, closes, then renames over
// `path`. Returns true on success. Used only for baseline files.
bool sdAtomicWrite(const char* path, const String& fullContents);

// Returns file size, or 0 if file missing / SD not ready.
size_t sdSize(const char* path);

// Rotate path -> path.1 -> path.2 ... keeping `keep` files. Call between writes.
void sdRotate(const char* path, int keep);
