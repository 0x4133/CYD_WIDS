// sd_writer.cpp
#include "sd_writer.h"
#include "config.h"
#include "sd_safe.h"
#include <SD.h>
#include <SPI.h>

// IMPORTANT: FreeRTOS queues byte-copy their items. Putting an Arduino `String`
// in a queued struct causes two owners of the same heap buffer -> heap
// corruption. Keep Msg trivially copyable.
static const size_t MAX_LINE_LEN = 160;
struct Msg {
  char path[28];
  char line[MAX_LINE_LEN];
};

static QueueHandle_t qh = nullptr;
static TaskHandle_t  th = nullptr;
static volatile bool ready = false;
static volatile uint32_t dropped = 0;

static void sdTask(void*) {
  for (;;) {
    Msg m;
    if (xQueueReceive(qh, &m, portMAX_DELAY) == pdTRUE) {
      if (sdSize(m.path) >= LOG_ROTATE_BYTES) sdRotate(m.path, LOG_ROTATE_KEEP);
      sdAppendLine(m.path, String(m.line));
    }
  }
}

bool sdWriterBegin() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  ready = SD.begin(SD_CS_PIN, SPI, SD_FREQ_HZ);
  if (!ready) { Serial.println("[SD] mount failed"); return false; }
  Serial.println("[SD] mounted");
  qh = xQueueCreate(SD_QUEUE_LEN, sizeof(Msg));
  if (!qh) return false;
  xTaskCreatePinnedToCore(sdTask, "sdWriter", 4096, nullptr, 1, &th, 1);
  return true;
}

bool sdWriterEnqueue(const char* path, const String& payload) {
  if (!ready || !qh) return false;
  // Under heap exhaustion, String concat can leave the object with a NULL
  // internal buffer — c_str() then returns nullptr and strlcpy segfaults.
  // Drop the message rather than crash; callers only care about best-effort.
  const char* s = payload.c_str();
  if (!path || !s) { dropped++; return false; }
  Msg m;
  strlcpy(m.path, path, sizeof(m.path));
  strlcpy(m.line, s,    sizeof(m.line));
  if (xQueueSend(qh, &m, 0) != pdTRUE) {
    dropped++;
    return false;
  }
  return true;
}

uint32_t sdWriterDropped() { return dropped; }
bool     sdWriterReady()   { return ready; }
