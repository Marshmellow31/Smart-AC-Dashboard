#pragma once

#include <ArduinoJson.h>

// Load/save ArduinoJson documents under /cfg/ on LittleFS. Callers keep their
// own dirty flags and call save() from the main loop (not async handlers).
namespace ConfigStore {

void begin();  // ensures /cfg exists
bool load(const char* path, JsonDocument& doc);
bool save(const char* path, const JsonDocument& doc);

}  // namespace ConfigStore
