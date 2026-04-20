#include "light_meter.h"
#include "config.h"
#include <Arduino.h>

static uint32_t lastSampleMs = 0;
static int filtRaw = -1;

void lightMeterBegin() {
  pinMode(LIGHT_SENSOR_PIN, INPUT);
  lastSampleMs = 0;
  filtRaw = -1;
}

void lightMeterSample() {
  if (millis() - lastSampleMs < 250) return;
  lastSampleMs = millis();
  int raw = analogRead(LIGHT_SENSOR_PIN);
  if (filtRaw < 0) filtRaw = raw;
  // IIR low-pass: 75% previous + 25% new
  filtRaw = (filtRaw * 3 + raw) / 4;
}

int lightMeterRaw() {
  return (filtRaw < 0) ? 0 : filtRaw;
}

int lightMeterPercent() {
  int raw = lightMeterRaw();
  int lo = LIGHT_SENSOR_MIN;
  int hi = LIGHT_SENSOR_MAX;
  if (hi <= lo) return 0;
  int p = (raw - lo) * 100 / (hi - lo);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
#if LIGHT_SENSOR_INVERT
  p = 100 - p;
#endif
  return p;
}
