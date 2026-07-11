#pragma once

#include <Arduino.h>

class EventLog;

// Polls Open-Meteo (free, no API key) for the current outdoor temperature at
// a fixed lat/lon, refreshed on a slow interval. Used by AutomationEngine to
// skip a scheduled cooldown when it's already cool outside. Best-effort: if
// Wi-Fi or the fetch is unavailable, isValid() stays false and callers must
// treat that as "no gate data, don't skip."
class WeatherManager {
 public:
  explicit WeatherManager(EventLog& log) : log_(log) {}

  void setLocation(float lat, float lon);
  void loop(bool wifiConnected);

  bool isValid() const { return valid_; }
  float outdoorTempC() const { return tempC_; }
  time_t lastUpdate() const { return lastFetch_; }

 private:
  void fetch();

  EventLog& log_;
  float lat_ = 21.7051f;   // Bharuch, Gujarat
  float lon_ = 72.9959f;
  bool valid_ = false;
  float tempC_ = 0.0f;
  time_t lastFetch_ = 0;
  unsigned long lastAttemptMs_ = 0;
  bool firstAttemptDone_ = false;
  static constexpr unsigned long kRefreshIntervalMs = 30UL * 60UL * 1000UL;  // 30 min
};
