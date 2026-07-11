#include "WeatherManager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "EventLog.h"

void WeatherManager::setLocation(float lat, float lon) {
  lat_ = lat;
  lon_ = lon;
}

void WeatherManager::loop(bool wifiConnected) {
  if (!wifiConnected) return;
  unsigned long nowMs = millis();
  if (firstAttemptDone_ && nowMs - lastAttemptMs_ < kRefreshIntervalMs) return;
  firstAttemptDone_ = true;
  lastAttemptMs_ = nowMs;
  fetch();
}

void WeatherManager::fetch() {
  WiFiClientSecure client;
  // Open-Meteo is a free, keyless API; skipping cert validation avoids
  // needing a CA store on the device for a read-only, non-sensitive request.
  client.setInsecure();

  char url[160];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m&timezone=Asia%%2FKolkata",
           lat_, lon_);

  HTTPClient http;
  if (!http.begin(client, url)) {
    log_.add(CmdSource::SYSTEM, "weather: connection setup failed");
    return;
  }
  http.setTimeout(8000);

  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    float t = doc["current"]["temperature_2m"] | NAN;
    if (!err && !isnan(t)) {
      tempC_ = t;
      valid_ = true;
      lastFetch_ = time(nullptr);
    } else {
      log_.add(CmdSource::SYSTEM, "weather: bad response payload");
    }
  } else {
    log_.add(CmdSource::SYSTEM, "weather: fetch failed (HTTP %d)", code);
  }
  http.end();
}
