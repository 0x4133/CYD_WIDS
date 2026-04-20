#pragma once
#include <stdint.h>

void lightMeterBegin();
void lightMeterSample();
int  lightMeterRaw();
int  lightMeterPercent();  // 0..100
