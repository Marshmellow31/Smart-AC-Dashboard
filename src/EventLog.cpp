#include "EventLog.h"

#include <stdarg.h>
#include <time.h>

void EventLog::add(CmdSource src, const char* fmt, ...) {
  Entry e;
  e.t = time(nullptr);
  e.src = src;

  va_list args;
  va_start(args, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, args);
  va_end(args);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[next_] = e;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) count_++;
  }

  Serial.printf("[%s] %s\n", cmdSourceToString(src), e.msg);
}

void EventLog::toJson(JsonArray arr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < count_; i++) {
    // Walk backwards from the most recent entry.
    size_t idx = (next_ + kCapacity - 1 - i) % kCapacity;
    JsonObject o = arr.add<JsonObject>();
    o["time"] = static_cast<uint32_t>(entries_[idx].t);
    o["source"] = cmdSourceToString(entries_[idx].src);
    o["msg"] = entries_[idx].msg;
  }
}
