// sd_writer.h
#pragma once
#include <Arduino.h>

bool sdWriterBegin();              // mounts SD, starts task, returns ok
bool sdWriterEnqueue(const char* path, const String& payload);  // false if dropped
uint32_t sdWriterDropped();        // drop counter for status strip
bool sdWriterReady();              // true if SD mounted
