// touch.h
#pragma once
#include <Arduino.h>
#include "touch_cal.h"

void touchBegin();
bool touchRead(int& sx, int& sy);
bool touchReadRaw(int& rx, int& ry, int& rz);
void touchSetCal(const TouchCal& c);
TouchCal touchGetCal();
