#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "ACState.h"

// Who asked for a state change. The override policy in AcController keys off
// this: MANUAL/SINRIC set a hold that blocks AUTOMATION; TIMER/SAFETY
// bypass the hold (user-initiated / protective); BOOT is the state restore;
// SYSTEM is for log entries that aren't commands (NTP sync, boot, etc.).
// SINRIC (the Alexa/Google Home bridge) is kept distinct from MANUAL so the
// bridge can skip echoing its own commands back to the cloud.
enum class CmdSource : uint8_t { MANUAL, SINRIC, AUTOMATION, TIMER, SAFETY, BOOT, SYSTEM };

inline const char* cmdSourceToString(CmdSource s) {
  switch (s) {
    case CmdSource::MANUAL:     return "manual";
    case CmdSource::SINRIC:     return "sinric";
    case CmdSource::AUTOMATION: return "automation";
    case CmdSource::TIMER:      return "timer";
    case CmdSource::SAFETY:     return "safety";
    case CmdSource::BOOT:       return "boot";
    case CmdSource::SYSTEM:     return "system";
  }
  return "?";
}

// Partial state change: only fields with has* set are applied. This lets a
// schedule say "on at 24° cool" while a plain off-timer only touches power.
struct AcCommand {
  bool hasPower = false;
  bool power = false;
  bool hasMode = false;
  AcMode mode = AcMode::COOL;
  bool hasTemp = false;
  uint8_t temp = 24;
  bool hasFan = false;
  FanSpeed fan = FanSpeed::FAN_AUTO;

  static AcCommand powerOff() {
    AcCommand c;
    c.hasPower = true;
    c.power = false;
    return c;
  }
};

// Parses {"power":bool,"temp":16-30,"mode":"cool|...","fan":"auto|..."} —
// every field optional, but at least one must be present.
inline bool acCommandFromJson(JsonObjectConst obj, AcCommand& cmd, String& err) {
  cmd = AcCommand();
  if (obj["power"].is<bool>()) {
    cmd.hasPower = true;
    cmd.power = obj["power"].as<bool>();
  }
  if (!obj["temp"].isNull()) {
    int t = obj["temp"].as<int>();
    if (t < kAcMinTemp || t > kAcMaxTemp) {
      err = "temp must be 16-30";
      return false;
    }
    cmd.hasTemp = true;
    cmd.temp = static_cast<uint8_t>(t);
  }
  if (obj["mode"].is<const char*>()) {
    bool ok = false;
    cmd.mode = acModeFromString(obj["mode"].as<String>(), ok);
    if (!ok) {
      err = "unknown mode";
      return false;
    }
    cmd.hasMode = true;
  }
  if (obj["fan"].is<const char*>()) {
    bool ok = false;
    cmd.fan = fanSpeedFromString(obj["fan"].as<String>(), ok);
    if (!ok) {
      err = "unknown fan speed";
      return false;
    }
    cmd.hasFan = true;
  }
  if (!cmd.hasPower && !cmd.hasTemp && !cmd.hasMode && !cmd.hasFan) {
    err = "action needs at least one of power/temp/mode/fan";
    return false;
  }
  return true;
}

inline void acCommandToJson(const AcCommand& cmd, JsonObject obj) {
  if (cmd.hasPower) obj["power"] = cmd.power;
  if (cmd.hasTemp) obj["temp"] = cmd.temp;
  if (cmd.hasMode) obj["mode"] = acModeToString(cmd.mode);
  if (cmd.hasFan) obj["fan"] = fanSpeedToString(cmd.fan);
}

// Short human form for the event log, e.g. "on 24° cool fan=auto" or "off".
inline String acCommandToString(const AcCommand& cmd) {
  if (cmd.hasPower && !cmd.power) return "off";
  String s = cmd.hasPower ? "on" : "set";
  if (cmd.hasTemp) s += " " + String(cmd.temp) + "\xC2\xB0";
  if (cmd.hasMode) s += String(" ") + acModeToString(cmd.mode);
  if (cmd.hasFan) s += String(" fan=") + fanSpeedToString(cmd.fan);
  return s;
}
