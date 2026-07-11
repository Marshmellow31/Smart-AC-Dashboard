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
class WeatherManager;

// One weekly schedule entry: fire an action at HH:MM on selected weekdays.
struct ScheduleSlot {
  char name[24] = "";
  bool enabled = true;
  uint8_t daysMask = 0;  // bit 0 = Sunday ... bit 6 = Saturday (tm_wday)
  uint8_t hour = 0;
  uint8_t minute = 0;
  AcCommand action;

  // Optional outdoor-temperature gate (needs WeatherManager data). When
  // enabled, the slot only fires if the last known outdoor temperature is at
  // or above skipIfOutdoorBelowC — e.g. skip a 6pm cooldown if it's already
  // cool outside. If no weather reading is available yet, the gate is
  // ignored and the slot fires normally (fail open, not fail silent).
  bool weatherGateEnabled = false;
  int8_t skipIfOutdoorBelowC = 28;

  int32_t lastHandledMinute = -1;  // epoch/60 fire-once guard, not persisted
};

// One step of a program. ON steps always carry full settings so the result
// is predictable; OFF steps only cut power.
struct ProgramStep {
  bool on = true;
  uint16_t minutes = 30;
  uint8_t temp = 24;
  AcMode mode = AcMode::COOL;
  FanSpeed fan = FanSpeed::FAN_AUTO;
};

// A program is an ordered step list — this covers both interval cycling
// ("on 45m @24°, off 30m, repeat until 6:00") and sleep curves
// ("on 60m @24°, on 60m @25°, on 120m @26°, off").
struct Program {
  char id[20] = "";
  char name[40] = "";
  bool repeat = false;
  int8_t endHour = -1;  // default end time for repeat programs; -1 = none
  int8_t endMinute = 0;
  std::vector<ProgramStep> steps;

  // Optional: id of another program to auto-start when this one finishes on
  // its own (all steps completed, or its end time was reached). Chaining is
  // skipped if the program was cancelled by a manual/Sinric/timer/safety
  // command or deleted out from under itself — only a natural finish chains.
  // Ignored for repeat=true programs (they only stop via end time or cancel,
  // both already covered above). Empty = no chaining.
  char chainToId[20] = "";
};

struct CountdownTimer {
  uint8_t id = 0;
  time_t fireAt = 0;
  AcCommand action;
};

// Clock-driven automation: countdown timers, weekly schedules, and programs.
// Evaluated once a second from the main loop. All *ToJson/*FromJson methods
// are thread-safe so the async HTTP task can call them directly.
//
// Interaction rules (deterministic on purpose):
//  - Weekly schedules are skipped while a program runs or a manual hold is
//    active (the skip is logged).
//  - Any MANUAL/SINRIC/TIMER/SAFETY command cancels a running program.
//  - Countdown timers and program steps bypass the manual hold (a program
//    can only be running if no hold is active — starting one clears it).
class AutomationEngine {
 public:
  static constexpr size_t kMaxSlots = 16;
  static constexpr size_t kMaxPrograms = 10;
  static constexpr size_t kMaxSteps = 20;
  static constexpr size_t kMaxTimers = 10;

  AutomationEngine(AcController& controller, TimeManager& time, EventLog& log,
                   AppSettings& settings);

  // Optional: wired in from main.cpp after construction. Weather-gated
  // schedules no-op (fail open) until this is set.
  void setWeatherManager(WeatherManager* weather) { weather_ = weather; }

  void begin();  // load persisted config, seed sleep presets, resume program
  void loop();

  // Wired to AcController's change callback (main loop context): external
  // commands cancel a running program.
  void onExternalCommand(CmdSource source);

  void schedulesToJson(JsonDocument& doc) const;
  bool schedulesFromJson(JsonObjectConst root, String& err);

  void programsToJson(JsonDocument& doc) const;
  bool programsFromJson(JsonObjectConst root, String& err);
  bool startProgram(const String& id, const String& endTime, String& err);
  bool stopProgram(const char* reason);  // false if none active

  void timersToJson(JsonDocument& doc) const;
  bool addTimer(uint16_t minutes, const AcCommand& action, String& err);
  int cancelTimer(int id);  // id -1 = all; returns count cancelled

  // Active program / timers / next schedule summary for /api/status.
  void statusToJson(JsonObject obj) const;

 private:
  void tick(time_t now);
  void tickTimers(time_t now);
  void tickProgram(time_t now);
  void tickSchedules(time_t now, const struct tm& lt);
  void saveDirty();

  // All helpers below assume mutex_ is held.
  Program* findProgram(const char* id);
  void stopProgramLocked(const char* reason);
  void startChainedProgramLocked(const char* id, time_t now);
  bool computeNextSchedule(time_t now, time_t& fireAt, const char*& name) const;
  static time_t nextOccurrence(time_t after, uint8_t hour, uint8_t minute);
  static uint32_t programTotalSeconds(const Program& p);

  void seedDefaultPrograms();
  void loadAll();

  AcController& controller_;
  TimeManager& time_;
  EventLog& log_;
  AppSettings& settings_;
  WeatherManager* weather_ = nullptr;

  mutable std::mutex mutex_;
  std::vector<ScheduleSlot> slots_;
  std::vector<Program> programs_;
  std::vector<CountdownTimer> timers_;
  uint8_t nextTimerId_ = 1;

  bool programActive_ = false;
  char activeProgramId_[20] = "";
  time_t programStart_ = 0;
  time_t programEnd_ = 0;  // 0 = no end time
  int lastStep_ = -1;

  bool schedulesDirty_ = false;
  bool programsDirty_ = false;
  bool runtimeDirty_ = false;
  unsigned long lastTickMs_ = 0;
};
