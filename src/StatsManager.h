#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <mutex>
#include <vector>

#include "AcCommand.h"
#include "AppSettings.h"

class AcController;
class TimeManager;
class EventLog;

// Runtime bookkeeping built purely from our own commanded state (IR is
// send-only, so this tracks what we told the AC to do):
//  - per-day ON time, kept for 30 days → energy/cost estimate in the API
//  - filter-clean reminder after N total ON hours
//  - safety auto-off after N continuous ON hours (optional)
class StatsManager {
 public:
  StatsManager(AcController& controller, TimeManager& time, EventLog& log,
               AppSettings& settings);

  void begin();
  void loop();

  void toJson(JsonObject obj) const;
  void resetFilter();

 private:
  struct DayStat {
    char date[11];  // "YYYY-MM-DD"
    uint32_t onSeconds;
  };

  void tick();
  void persist();

  AcController& controller_;
  TimeManager& time_;
  EventLog& log_;
  AppSettings& settings_;

  mutable std::mutex mutex_;
  std::vector<DayStat> days_;   // newest last, max 30
  uint32_t filterSeconds_ = 0;  // since last filter reset

  bool wasOn_ = false;
  unsigned long onSinceMs_ = 0;  // millis when power went on (continuous-on)
  unsigned long lastTickMs_ = 0;
  unsigned long lastSaveMs_ = 0;
  bool saveRequested_ = false;

  static constexpr size_t kMaxDays = 30;
  static constexpr unsigned long kSaveIntervalMs = 10UL * 60UL * 1000UL;
};
