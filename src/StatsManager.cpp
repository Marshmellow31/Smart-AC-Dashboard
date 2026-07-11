#include "StatsManager.h"

#include "AcController.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "TimeManager.h"

namespace {
constexpr const char* kStatsPath = "/cfg/stats.json";
}

StatsManager::StatsManager(AcController& controller, TimeManager& time,
                           EventLog& log, AppSettings& settings)
    : controller_(controller), time_(time), log_(log), settings_(settings) {}

void StatsManager::begin() {
  JsonDocument doc;
  if (!ConfigStore::load(kStatsPath, doc)) return;

  std::lock_guard<std::mutex> lock(mutex_);
  filterSeconds_ = doc["filterSeconds"] | 0;
  for (JsonObjectConst o : doc["days"].as<JsonArrayConst>()) {
    if (days_.size() >= kMaxDays) break;
    DayStat d;
    strlcpy(d.date, o["date"] | "", sizeof(d.date));
    d.onSeconds = o["onSeconds"] | 0;
    if (d.date[0] != '\0') days_.push_back(d);
  }
}

void StatsManager::loop() {
  unsigned long nowMs = millis();
  if (nowMs - lastTickMs_ >= 1000) {
    lastTickMs_ = nowMs;
    tick();
  }

  bool doSave = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (saveRequested_ && nowMs - lastSaveMs_ >= 5000) {
      doSave = true;
    } else if (wasOn_ && nowMs - lastSaveMs_ >= kSaveIntervalMs) {
      doSave = true;  // periodic checkpoint while running
    }
    if (doSave) {
      saveRequested_ = false;
      lastSaveMs_ = nowMs;
    }
  }
  if (doSave) persist();
}

void StatsManager::tick() {
  bool on = controller_.state().power;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (on && !wasOn_) {
      onSinceMs_ = millis();
    }
    if (!on && wasOn_) {
      saveRequested_ = true;  // checkpoint on power-off
    }
    wasOn_ = on;

    if (on) {
      filterSeconds_++;

      struct tm lt;
      if (time_.localTm(lt)) {
        char today[11];
        strftime(today, sizeof(today), "%Y-%m-%d", &lt);
        if (days_.empty() || strcmp(days_.back().date, today) != 0) {
          DayStat d;
          strlcpy(d.date, today, sizeof(d.date));
          d.onSeconds = 0;
          days_.push_back(d);
          while (days_.size() > kMaxDays) days_.erase(days_.begin());
          saveRequested_ = true;  // persist the day rollover promptly
        }
        days_.back().onSeconds++;
      }
    }
  }

  // Safety auto-off: pure millis-based so it works even without NTP.
  if (on && settings_.maxContinuousHours > 0) {
    unsigned long limitMs = settings_.maxContinuousHours * 3600UL * 1000UL;
    if (millis() - onSinceMs_ >= limitMs) {
      char reason[64];
      snprintf(reason, sizeof(reason), "safety auto-off after %u h continuous",
               settings_.maxContinuousHours);
      controller_.apply(AcCommand::powerOff(), CmdSource::SAFETY, reason);
    }
  }
}

void StatsManager::persist() {
  JsonDocument doc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    doc["filterSeconds"] = filterSeconds_;
    JsonArray arr = doc["days"].to<JsonArray>();
    for (const auto& d : days_) {
      JsonObject o = arr.add<JsonObject>();
      o["date"] = d.date;
      o["onSeconds"] = d.onSeconds;
    }
  }
  ConfigStore::save(kStatsPath, doc);
}

void StatsManager::toJson(JsonObject obj) const {
  float kwhPerSecond = settings_.acWatts / 1000.0f / 3600.0f;
  float tariff = settings_.tariffPerKwh;

  std::lock_guard<std::mutex> lock(mutex_);

  char today[11] = "";
  char weekStart[11] = "";  // 6 days ago → rolling 7-day window incl. today
  char month[8] = "";
  struct tm lt;
  if (time_.localTm(lt)) {
    strftime(today, sizeof(today), "%Y-%m-%d", &lt);
    strftime(month, sizeof(month), "%Y-%m", &lt);
    time_t wk = time_.now() - 6 * 24 * 3600;
    struct tm wt;
    localtime_r(&wk, &wt);
    strftime(weekStart, sizeof(weekStart), "%Y-%m-%d", &wt);
  }

  uint32_t todaySec = 0, weekSec = 0, monthSec = 0;
  JsonArray daysArr = obj["days"].to<JsonArray>();
  for (const auto& d : days_) {
    // ISO dates compare correctly as strings.
    if (strcmp(d.date, today) == 0) todaySec = d.onSeconds;
    if (weekStart[0] != '\0' && strcmp(d.date, weekStart) >= 0) {
      weekSec += d.onSeconds;
    }
    if (month[0] != '\0' && strncmp(d.date, month, strlen(month)) == 0) {
      monthSec += d.onSeconds;
    }
    JsonObject o = daysArr.add<JsonObject>();
    o["date"] = d.date;
    o["onMinutes"] = d.onSeconds / 60;
    o["kwh"] = d.onSeconds * kwhPerSecond;
    o["cost"] = d.onSeconds * kwhPerSecond * tariff;
  }

  auto period = [&](const char* key, uint32_t seconds) {
    JsonObject p = obj[key].to<JsonObject>();
    p["onMinutes"] = seconds / 60;
    p["kwh"] = seconds * kwhPerSecond;
    p["cost"] = seconds * kwhPerSecond * tariff;
  };
  period("today", todaySec);
  period("week", weekSec);
  period("month", monthSec);

  JsonObject f = obj["filter"].to<JsonObject>();
  f["hours"] = filterSeconds_ / 3600;
  f["limitHours"] = settings_.filterLimitHours;
  f["needsCleaning"] = (filterSeconds_ / 3600) >= settings_.filterLimitHours;

  obj["continuousOnMinutes"] =
      wasOn_ ? (millis() - onSinceMs_) / 60000UL : 0;
}

void StatsManager::resetFilter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    filterSeconds_ = 0;
    saveRequested_ = true;
  }
  log_.add(CmdSource::SYSTEM, "filter runtime counter reset");
}
