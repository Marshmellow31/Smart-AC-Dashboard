#include "AutomationEngine.h"

#include "AcController.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "TimeManager.h"

namespace {

constexpr const char* kSchedulesPath = "/cfg/schedules.json";
constexpr const char* kProgramsPath = "/cfg/programs.json";
constexpr const char* kRuntimePath = "/cfg/runtime.json";

bool parseHHMM(const char* s, uint8_t& hour, uint8_t& minute) {
  unsigned h = 0, m = 0;
  if (sscanf(s, "%u:%u", &h, &m) != 2) return false;
  if (h > 23 || m > 59) return false;
  hour = static_cast<uint8_t>(h);
  minute = static_cast<uint8_t>(m);
  return true;
}

String formatHHMM(uint8_t hour, uint8_t minute) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", hour, minute);
  return String(buf);
}

AcCommand commandFromStep(const ProgramStep& step) {
  AcCommand cmd;
  cmd.hasPower = true;
  cmd.power = step.on;
  if (step.on) {
    cmd.hasTemp = true;
    cmd.temp = step.temp;
    cmd.hasMode = true;
    cmd.mode = step.mode;
    cmd.hasFan = true;
    cmd.fan = step.fan;
  }
  return cmd;
}

void stepToJson(const ProgramStep& step, JsonObject o) {
  o["on"] = step.on;
  o["minutes"] = step.minutes;
  if (step.on) {
    o["temp"] = step.temp;
    o["mode"] = acModeToString(step.mode);
    o["fan"] = fanSpeedToString(step.fan);
  }
}

bool stepFromJson(JsonObjectConst o, ProgramStep& step, String& err) {
  step = ProgramStep();
  step.on = o["on"] | true;
  int minutes = o["minutes"] | 0;
  if (minutes < 1 || minutes > 1440) {
    err = "step minutes must be 1-1440";
    return false;
  }
  step.minutes = static_cast<uint16_t>(minutes);
  if (step.on) {
    int t = o["temp"] | 24;
    if (t < kAcMinTemp || t > kAcMaxTemp) {
      err = "step temp must be 16-30";
      return false;
    }
    step.temp = static_cast<uint8_t>(t);
    bool ok = true;
    if (o["mode"].is<const char*>()) {
      step.mode = acModeFromString(o["mode"].as<String>(), ok);
      if (!ok) {
        err = "unknown step mode";
        return false;
      }
    }
    if (o["fan"].is<const char*>()) {
      step.fan = fanSpeedFromString(o["fan"].as<String>(), ok);
      if (!ok) {
        err = "unknown step fan";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

AutomationEngine::AutomationEngine(AcController& controller, TimeManager& time,
                                   EventLog& log, AppSettings& settings)
    : controller_(controller), time_(time), log_(log), settings_(settings) {}

// ---------------------------------------------------------------------------
// Setup / persistence

void AutomationEngine::begin() {
  loadAll();
}

void AutomationEngine::loadAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  {
    JsonDocument doc;
    if (ConfigStore::load(kSchedulesPath, doc)) {
      for (JsonObjectConst o : doc["slots"].as<JsonArrayConst>()) {
        if (slots_.size() >= kMaxSlots) break;
        ScheduleSlot slot;
        strlcpy(slot.name, o["name"] | "", sizeof(slot.name));
        slot.enabled = o["enabled"] | true;
        for (int d : o["days"].as<JsonArrayConst>()) {
          if (d >= 0 && d <= 6) slot.daysMask |= (1 << d);
        }
        if (!parseHHMM(o["time"] | "", slot.hour, slot.minute)) continue;
        String err;
        if (!acCommandFromJson(o["action"].as<JsonObjectConst>(), slot.action, err)) continue;
        slots_.push_back(slot);
      }
    }
  }

  {
    JsonDocument doc;
    if (ConfigStore::load(kProgramsPath, doc)) {
      for (JsonObjectConst o : doc["programs"].as<JsonArrayConst>()) {
        if (programs_.size() >= kMaxPrograms) break;
        Program p;
        strlcpy(p.id, o["id"] | "", sizeof(p.id));
        if (p.id[0] == '\0') continue;
        strlcpy(p.name, o["name"] | "", sizeof(p.name));
        p.repeat = o["repeat"] | false;
        const char* endTime = o["endTime"] | "";
        uint8_t eh, em;
        if (endTime[0] != '\0' && parseHHMM(endTime, eh, em)) {
          p.endHour = static_cast<int8_t>(eh);
          p.endMinute = static_cast<int8_t>(em);
        }
        for (JsonObjectConst so : o["steps"].as<JsonArrayConst>()) {
          if (p.steps.size() >= kMaxSteps) break;
          ProgramStep step;
          String err;
          if (stepFromJson(so, step, err)) p.steps.push_back(step);
        }
        if (!p.steps.empty()) programs_.push_back(p);
      }
    } else {
      seedDefaultPrograms();
      programsDirty_ = true;
    }
  }

  {
    JsonDocument doc;
    if (ConfigStore::load(kRuntimePath, doc)) {
      time_t now = time(nullptr);
      for (JsonObjectConst o : doc["timers"].as<JsonArrayConst>()) {
        if (timers_.size() >= kMaxTimers) break;
        CountdownTimer t;
        t.id = o["id"] | 0;
        t.fireAt = o["fireAt"] | 0;
        String err;
        if (!acCommandFromJson(o["action"].as<JsonObjectConst>(), t.action, err)) continue;
        // Drop timers that expired while we were powered off — firing a stale
        // ON hours later would be surprising.
        if (t.fireAt <= now) {
          log_.add(CmdSource::SYSTEM, "dropped expired timer (%s)",
                   acCommandToString(t.action).c_str());
          runtimeDirty_ = true;
          continue;
        }
        timers_.push_back(t);
        if (t.id >= nextTimerId_) nextTimerId_ = t.id + 1;
      }

      JsonObjectConst prog = doc["program"].as<JsonObjectConst>();
      if (prog["active"] | false) {
        strlcpy(activeProgramId_, prog["id"] | "", sizeof(activeProgramId_));
        programStart_ = prog["start"] | 0;
        programEnd_ = prog["end"] | 0;
        Program* p = findProgram(activeProgramId_);
        time_t elapsed = now - programStart_;
        bool valid = p && programStart_ > 0 && elapsed >= 0;
        if (valid && !p->repeat && elapsed >= (time_t)programTotalSeconds(*p)) valid = false;
        if (valid && programEnd_ > 0 && now >= programEnd_) valid = false;
        if (valid) {
          programActive_ = true;
          lastStep_ = -1;  // re-apply the current step after the reboot
          log_.add(CmdSource::SYSTEM, "resuming program '%s' after reboot", p->name);
        } else {
          runtimeDirty_ = true;
        }
      }
    }
  }
}

void AutomationEngine::seedDefaultPrograms() {
  auto makeStep = [](bool on, uint16_t minutes, uint8_t temp, FanSpeed fan) {
    ProgramStep s;
    s.on = on;
    s.minutes = minutes;
    s.temp = temp;
    s.mode = AcMode::COOL;
    s.fan = fan;
    return s;
  };

  // Shipped sleep presets and an interval-cycling example; the user can edit
  // or delete these from the Programs UI.
  Program sleep8;
  strlcpy(sleep8.id, "sleep8h", sizeof(sleep8.id));
  strlcpy(sleep8.name, "Sleep 8h (24\xC2\xB0\xE2\x86\x92""26\xC2\xB0)", sizeof(sleep8.name));
  sleep8.steps.push_back(makeStep(true, 60, 24, FanSpeed::FAN_LOW));
  sleep8.steps.push_back(makeStep(true, 120, 25, FanSpeed::FAN_LOW));
  sleep8.steps.push_back(makeStep(true, 299, 26, FanSpeed::FAN_LOW));
  sleep8.steps.push_back(makeStep(false, 1, 24, FanSpeed::FAN_AUTO));
  programs_.push_back(sleep8);

  Program sleep4;
  strlcpy(sleep4.id, "sleep4h", sizeof(sleep4.id));
  strlcpy(sleep4.name, "Sleep 4h (24\xC2\xB0\xE2\x86\x92""26\xC2\xB0)", sizeof(sleep4.name));
  sleep4.steps.push_back(makeStep(true, 60, 24, FanSpeed::FAN_LOW));
  sleep4.steps.push_back(makeStep(true, 60, 25, FanSpeed::FAN_LOW));
  sleep4.steps.push_back(makeStep(true, 119, 26, FanSpeed::FAN_LOW));
  sleep4.steps.push_back(makeStep(false, 1, 24, FanSpeed::FAN_AUTO));
  programs_.push_back(sleep4);

  Program cycle;
  strlcpy(cycle.id, "cycle45-30", sizeof(cycle.id));
  strlcpy(cycle.name, "Cycle 45m on / 30m off @24\xC2\xB0", sizeof(cycle.name));
  cycle.repeat = true;
  cycle.endHour = 6;
  cycle.endMinute = 0;
  cycle.steps.push_back(makeStep(true, 45, 24, FanSpeed::FAN_AUTO));
  cycle.steps.push_back(makeStep(false, 30, 24, FanSpeed::FAN_AUTO));
  programs_.push_back(cycle);
}

void AutomationEngine::saveDirty() {
  bool schedules, programs, runtime;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    schedules = schedulesDirty_;
    programs = programsDirty_;
    runtime = runtimeDirty_;
    schedulesDirty_ = programsDirty_ = runtimeDirty_ = false;
  }

  if (schedules) {
    JsonDocument doc;
    schedulesToJson(doc);
    ConfigStore::save(kSchedulesPath, doc);
  }
  if (programs) {
    JsonDocument doc;
    programsToJson(doc);
    ConfigStore::save(kProgramsPath, doc);
  }
  if (runtime) {
    JsonDocument doc;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      JsonArray arr = doc["timers"].to<JsonArray>();
      for (const auto& t : timers_) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = t.id;
        o["fireAt"] = static_cast<uint32_t>(t.fireAt);
        acCommandToJson(t.action, o["action"].to<JsonObject>());
      }
      JsonObject prog = doc["program"].to<JsonObject>();
      prog["active"] = programActive_;
      prog["id"] = activeProgramId_;
      prog["start"] = static_cast<uint32_t>(programStart_);
      prog["end"] = static_cast<uint32_t>(programEnd_);
    }
    ConfigStore::save(kRuntimePath, doc);
  }
}

// ---------------------------------------------------------------------------
// Evaluation

void AutomationEngine::loop() {
  unsigned long nowMs = millis();
  if (nowMs - lastTickMs_ >= 1000) {
    lastTickMs_ = nowMs;
    if (time_.isTimeValid()) tick(time_.now());
  }
  saveDirty();
}

void AutomationEngine::tick(time_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  tickTimers(now);
  tickProgram(now);

  struct tm lt;
  localtime_r(&now, &lt);
  tickSchedules(now, lt);
}

void AutomationEngine::tickTimers(time_t now) {
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (now >= it->fireAt) {
      AcCommand action = it->action;
      it = timers_.erase(it);
      runtimeDirty_ = true;
      controller_.apply(action, CmdSource::TIMER, "countdown timer");
    } else {
      ++it;
    }
  }
}

void AutomationEngine::tickProgram(time_t now) {
  if (!programActive_) return;

  Program* p = findProgram(activeProgramId_);
  if (!p || p->steps.empty()) {
    stopProgramLocked("program deleted");
    return;
  }

  if (programEnd_ > 0 && now >= programEnd_) {
    stopProgramLocked("end time reached");
    controller_.apply(AcCommand::powerOff(), CmdSource::AUTOMATION, "program end time");
    return;
  }

  uint32_t total = programTotalSeconds(*p);
  time_t elapsed = now - programStart_;
  if (elapsed < 0) {  // clock jumped backwards (NTP correction)
    programStart_ = now;
    elapsed = 0;
  }

  if (!p->repeat && elapsed >= (time_t)total) {
    stopProgramLocked("completed");
    return;
  }

  uint32_t pos = p->repeat ? (uint32_t)(elapsed % total) : (uint32_t)elapsed;
  uint32_t acc = 0;
  int idx = 0;
  for (size_t i = 0; i < p->steps.size(); i++) {
    acc += (uint32_t)p->steps[i].minutes * 60;
    if (pos < acc) {
      idx = static_cast<int>(i);
      break;
    }
  }

  if (idx != lastStep_) {
    lastStep_ = idx;
    runtimeDirty_ = true;
    char reason[64];
    snprintf(reason, sizeof(reason), "program '%s' step %d/%u", p->name, idx + 1,
             (unsigned)p->steps.size());
    controller_.apply(commandFromStep(p->steps[idx]), CmdSource::AUTOMATION, reason);
  }
}

void AutomationEngine::tickSchedules(time_t now, const struct tm& lt) {
  if (!settings_.automationEnabled) return;

  int32_t epochMin = static_cast<int32_t>(now / 60);
  for (auto& slot : slots_) {
    if (!slot.enabled) continue;
    if (!(slot.daysMask & (1 << lt.tm_wday))) continue;
    if (lt.tm_hour != slot.hour || lt.tm_min != slot.minute) continue;
    if (slot.lastHandledMinute == epochMin) continue;
    slot.lastHandledMinute = epochMin;

    if (programActive_) {
      log_.add(CmdSource::AUTOMATION, "skipped (program running): schedule '%s'",
               slot.name);
      continue;
    }
    char reason[48];
    snprintf(reason, sizeof(reason), "schedule '%s'", slot.name);
    controller_.apply(slot.action, CmdSource::AUTOMATION, reason);
  }
}

void AutomationEngine::onExternalCommand(CmdSource source) {
  if (source != CmdSource::MANUAL && source != CmdSource::CLOUD &&
      source != CmdSource::TIMER && source != CmdSource::SAFETY) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (programActive_) {
    char reason[48];
    snprintf(reason, sizeof(reason), "cancelled by %s command", cmdSourceToString(source));
    stopProgramLocked(reason);
  }
}

// ---------------------------------------------------------------------------
// Programs

Program* AutomationEngine::findProgram(const char* id) {
  for (auto& p : programs_) {
    if (strcmp(p.id, id) == 0) return &p;
  }
  return nullptr;
}

uint32_t AutomationEngine::programTotalSeconds(const Program& p) {
  uint32_t total = 0;
  for (const auto& s : p.steps) total += (uint32_t)s.minutes * 60;
  return total;
}

time_t AutomationEngine::nextOccurrence(time_t after, uint8_t hour, uint8_t minute) {
  struct tm lt;
  localtime_r(&after, &lt);
  lt.tm_hour = hour;
  lt.tm_min = minute;
  lt.tm_sec = 0;
  time_t t = mktime(&lt);
  if (t <= after) t += 24 * 3600;
  return t;
}

void AutomationEngine::stopProgramLocked(const char* reason) {
  if (!programActive_) return;
  Program* p = findProgram(activeProgramId_);
  log_.add(CmdSource::AUTOMATION, "program '%s' stopped: %s",
           p ? p->name : activeProgramId_, reason);
  programActive_ = false;
  activeProgramId_[0] = '\0';
  programStart_ = programEnd_ = 0;
  lastStep_ = -1;
  runtimeDirty_ = true;
}

bool AutomationEngine::startProgram(const String& id, const String& endTime, String& err) {
  if (!time_.isTimeValid()) {
    err = "clock not synced yet, try again shortly";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    Program* p = findProgram(id.c_str());
    if (!p) {
      err = "unknown program id";
      return false;
    }
    if (programActive_) stopProgramLocked("replaced by new program");

    time_t now = time_.now();
    programActive_ = true;
    strlcpy(activeProgramId_, p->id, sizeof(activeProgramId_));
    programStart_ = now;
    lastStep_ = -1;
    programEnd_ = 0;

    uint8_t eh = 0, em = 0;
    if (endTime.length() > 0) {
      if (!parseHHMM(endTime.c_str(), eh, em)) {
        programActive_ = false;
        err = "endTime must be HH:MM";
        return false;
      }
      programEnd_ = nextOccurrence(now, eh, em);
    } else if (p->endHour >= 0) {
      programEnd_ = nextOccurrence(now, (uint8_t)p->endHour, (uint8_t)p->endMinute);
    }
    runtimeDirty_ = true;

    if (programEnd_ > 0) {
      log_.add(CmdSource::AUTOMATION, "program '%s' started, ends %s", p->name,
               time_.format(programEnd_).c_str());
    } else {
      log_.add(CmdSource::AUTOMATION, "program '%s' started", p->name);
    }
  }

  // A program start is explicit user intent — release any manual hold so the
  // first step isn't blocked.
  controller_.clearOverride();
  return true;
}

bool AutomationEngine::stopProgram(const char* reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!programActive_) return false;
  stopProgramLocked(reason);
  return true;
}

void AutomationEngine::programsToJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonArray arr = doc["programs"].to<JsonArray>();
  for (const auto& p : programs_) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["name"] = p.name;
    o["repeat"] = p.repeat;
    o["endTime"] = p.endHour >= 0 ? formatHHMM(p.endHour, p.endMinute) : String("");
    JsonArray steps = o["steps"].to<JsonArray>();
    for (const auto& s : p.steps) stepToJson(s, steps.add<JsonObject>());
  }
}

bool AutomationEngine::programsFromJson(JsonObjectConst root, String& err) {
  JsonArrayConst arr = root["programs"].as<JsonArrayConst>();
  if (arr.isNull()) {
    err = "expected {\"programs\": [...]}";
    return false;
  }
  if (arr.size() > kMaxPrograms) {
    err = "too many programs (max 10)";
    return false;
  }

  std::vector<Program> parsed;
  for (JsonObjectConst o : arr) {
    Program p;
    strlcpy(p.id, o["id"] | "", sizeof(p.id));
    if (p.id[0] == '\0') {
      err = "every program needs a non-empty id";
      return false;
    }
    for (const auto& existing : parsed) {
      if (strcmp(existing.id, p.id) == 0) {
        err = "duplicate program id: " + String(p.id);
        return false;
      }
    }
    strlcpy(p.name, o["name"] | p.id, sizeof(p.name));
    p.repeat = o["repeat"] | false;
    const char* endTime = o["endTime"] | "";
    if (endTime[0] != '\0') {
      uint8_t eh, em;
      if (!parseHHMM(endTime, eh, em)) {
        err = "endTime must be HH:MM";
        return false;
      }
      p.endHour = static_cast<int8_t>(eh);
      p.endMinute = static_cast<int8_t>(em);
    }
    JsonArrayConst steps = o["steps"].as<JsonArrayConst>();
    if (steps.isNull() || steps.size() == 0 || steps.size() > kMaxSteps) {
      err = "each program needs 1-20 steps";
      return false;
    }
    for (JsonObjectConst so : steps) {
      ProgramStep step;
      if (!stepFromJson(so, step, err)) return false;
      p.steps.push_back(step);
    }
    parsed.push_back(std::move(p));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  programs_ = std::move(parsed);
  programsDirty_ = true;
  // If the active program's definition vanished, the next tick stops it.
  return true;
}

// ---------------------------------------------------------------------------
// Schedules

void AutomationEngine::schedulesToJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonArray arr = doc["slots"].to<JsonArray>();
  for (const auto& slot : slots_) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = slot.name;
    o["enabled"] = slot.enabled;
    JsonArray days = o["days"].to<JsonArray>();
    for (int d = 0; d < 7; d++) {
      if (slot.daysMask & (1 << d)) days.add(d);
    }
    o["time"] = formatHHMM(slot.hour, slot.minute);
    acCommandToJson(slot.action, o["action"].to<JsonObject>());
  }
}

bool AutomationEngine::schedulesFromJson(JsonObjectConst root, String& err) {
  JsonArrayConst arr = root["slots"].as<JsonArrayConst>();
  if (arr.isNull()) {
    err = "expected {\"slots\": [...]}";
    return false;
  }
  if (arr.size() > kMaxSlots) {
    err = "too many schedule slots (max 16)";
    return false;
  }

  std::vector<ScheduleSlot> parsed;
  for (JsonObjectConst o : arr) {
    ScheduleSlot slot;
    strlcpy(slot.name, o["name"] | "schedule", sizeof(slot.name));
    slot.enabled = o["enabled"] | true;
    JsonArrayConst days = o["days"].as<JsonArrayConst>();
    if (days.isNull() || days.size() == 0) {
      err = "each slot needs days [0-6] (0=Sunday)";
      return false;
    }
    for (int d : days) {
      if (d < 0 || d > 6) {
        err = "days must be 0-6 (0=Sunday)";
        return false;
      }
      slot.daysMask |= (1 << d);
    }
    if (!parseHHMM(o["time"] | "", slot.hour, slot.minute)) {
      err = "each slot needs time \"HH:MM\"";
      return false;
    }
    if (!acCommandFromJson(o["action"].as<JsonObjectConst>(), slot.action, err)) {
      return false;
    }
    parsed.push_back(slot);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  slots_ = std::move(parsed);
  schedulesDirty_ = true;
  return true;
}

// ---------------------------------------------------------------------------
// Countdown timers

void AutomationEngine::timersToJson(JsonDocument& doc) const {
  time_t now = time(nullptr);
  std::lock_guard<std::mutex> lock(mutex_);
  JsonArray arr = doc["timers"].to<JsonArray>();
  for (const auto& t : timers_) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = t.id;
    o["fireAt"] = static_cast<uint32_t>(t.fireAt);
    o["remainingSec"] = t.fireAt > now ? static_cast<uint32_t>(t.fireAt - now) : 0;
    acCommandToJson(t.action, o["action"].to<JsonObject>());
  }
}

bool AutomationEngine::addTimer(uint16_t minutes, const AcCommand& action, String& err) {
  if (!time_.isTimeValid()) {
    err = "clock not synced yet, try again shortly";
    return false;
  }
  if (minutes < 1 || minutes > 1440) {
    err = "minutes must be 1-1440";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (timers_.size() >= kMaxTimers) {
    err = "too many timers (max 4)";
    return false;
  }
  CountdownTimer t;
  t.id = nextTimerId_++;
  if (nextTimerId_ == 0) nextTimerId_ = 1;
  t.fireAt = time_.now() + static_cast<time_t>(minutes) * 60;
  t.action = action;
  timers_.push_back(t);
  runtimeDirty_ = true;
  log_.add(CmdSource::TIMER, "timer set: %s in %u min",
           acCommandToString(action).c_str(), minutes);
  return true;
}

int AutomationEngine::cancelTimer(int id) {
  std::lock_guard<std::mutex> lock(mutex_);
  int removed = 0;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (id < 0 || it->id == id) {
      it = timers_.erase(it);
      removed++;
    } else {
      ++it;
    }
  }
  if (removed > 0) {
    runtimeDirty_ = true;
    log_.add(CmdSource::TIMER, "%d timer(s) cancelled", removed);
  }
  return removed;
}

// ---------------------------------------------------------------------------
// Status

bool AutomationEngine::computeNextSchedule(time_t now, time_t& fireAt,
                                           const char*& name) const {
  bool found = false;
  for (const auto& slot : slots_) {
    if (!slot.enabled) continue;
    for (int offset = 0; offset <= 7; offset++) {
      struct tm c;
      localtime_r(&now, &c);
      c.tm_mday += offset;
      c.tm_hour = slot.hour;
      c.tm_min = slot.minute;
      c.tm_sec = 0;
      time_t t = mktime(&c);  // normalises date and recomputes tm_wday
      if (t <= now) continue;
      if (!(slot.daysMask & (1 << c.tm_wday))) continue;
      if (!found || t < fireAt) {
        fireAt = t;
        name = slot.name;
        found = true;
      }
      break;  // earliest occurrence for this slot found
    }
  }
  return found;
}

void AutomationEngine::statusToJson(JsonObject obj) const {
  time_t now = time(nullptr);
  std::lock_guard<std::mutex> lock(mutex_);

  JsonObject prog = obj["program"].to<JsonObject>();
  prog["active"] = programActive_;
  if (programActive_) {
    prog["id"] = activeProgramId_;
    for (const auto& p : programs_) {
      if (strcmp(p.id, activeProgramId_) == 0) {
        prog["name"] = p.name;
        break;
      }
    }
    prog["step"] = lastStep_ + 1;
    prog["startedAt"] = static_cast<uint32_t>(programStart_);
    if (programEnd_ > 0) prog["endsAt"] = static_cast<uint32_t>(programEnd_);
  }

  obj["timerCount"] = timers_.size();

  time_t fireAt = 0;
  const char* name = nullptr;
  if (settings_.automationEnabled && computeNextSchedule(now, fireAt, name)) {
    JsonObject next = obj["nextSchedule"].to<JsonObject>();
    next["name"] = name;
    next["at"] = static_cast<uint32_t>(fireAt);
  }
}
