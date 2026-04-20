// touch_cal.h
#pragma once
#include <Arduino.h>

// General 2D affine mapping: screen = A * raw + C.
//   sx = ax*rxRaw + bx*ryRaw + cx
//   sy = ay*rxRaw + by*ryRaw + cy
// This is axis- and rotation-agnostic: the wizard discovers the right
// direction and coupling from the calibration taps.
struct TouchCal {
  float ax, bx, cx;
  float ay, by, cy;
};

bool touchCalLoad(TouchCal& out);
bool touchCalSave(const TouchCal& cal);
bool touchCalRunWizard(TouchCal& out);  // returns true on verified success
