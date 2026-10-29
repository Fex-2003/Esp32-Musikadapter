#pragma once
#include <Arduino.h>
#include <vector>
namespace issues {
struct Entry { String path, reason; uint32_t boot=0, seconds=0, attempts=0; };
void begin();
void record(const String& path, const String& reason);
bool blocked(const String& path);
const std::vector<Entry>& list();
void clear(const String& path); // leer: alle
void flush(bool force=false);
bool retry(const String& path, String& message);
}
