#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <mutex>

#include "AcCommand.h"

// In-RAM ring buffer of automation/command events, exposed at /api/log so the
// user can see exactly what fired when (and what got skipped and why).
class EventLog {
 public:
  void add(CmdSource src, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
  void toJson(JsonArray arr) const;  // newest first

 private:
  struct Entry {
    time_t t;
    CmdSource src;
    char msg[96];
  };

  static constexpr size_t kCapacity = 50;
  mutable std::mutex mutex_;
  Entry entries_[kCapacity];
  size_t next_ = 0;
  size_t count_ = 0;
};
