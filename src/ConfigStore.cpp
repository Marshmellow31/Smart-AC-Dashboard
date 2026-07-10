#include "ConfigStore.h"

#include <LittleFS.h>

namespace ConfigStore {

void begin() {
  if (!LittleFS.exists("/cfg")) {
    LittleFS.mkdir("/cfg");
  }
}

bool load(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("ConfigStore: failed to parse %s: %s\n", path, err.c_str());
    return false;
  }
  return true;
}

bool save(const char* path, const JsonDocument& doc) {
  // Write to a temp file first so a crash mid-write can't corrupt the config.
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.printf("ConfigStore: failed to open %s for writing\n", tmp.c_str());
    return false;
  }
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (!ok) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

}  // namespace ConfigStore
