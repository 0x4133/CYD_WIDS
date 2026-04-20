// keyboard.h
#pragma once
#include <Arduino.h>

// Full-screen modal. Prefills `out` with current content, lets the user
// edit, returns true on OK and false on CANCEL. outSize includes null.
bool keyboardPrompt(const char* title, char* out, int outSize);
