#pragma once

#include <Arduino.h>

#include "AcCommand.h"

class EventLog;

// NTP time in IST. All time-based automation must check isTimeValid() and
// hold off until the first sync completes.
class TimeManager {
 public:
  explicit TimeManager(EventLog& log) : log_(log) {}

  void onWifiConnected();  // starts SNTP (idempotent)
  void loop();             // logs once when the first sync lands

  bool isTimeValid() const { return time(nullptr) > kMinValidEpoch; }
  time_t now() const { return time(nullptr); }

  bool localTm(struct tm& out) const {
    if (!isTimeValid()) return false;
    time_t t = time(nullptr);
    localtime_r(&t, &out);
    return true;
  }

  String format(time_t t) const;  // "2026-07-11 14:30" local time

 private:
  static constexpr time_t kMinValidEpoch = 1700000000;  // Nov 2023

  EventLog& log_;
  bool configured_ = false;
  bool syncLogged_ = false;
};
