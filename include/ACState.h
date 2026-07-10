#pragma once

#include <Arduino.h>

// Protocol-agnostic AC state. Deliberately has no dependency on
// IRremoteESP8266 so it can be reused by future MQTT/scheduler/HA code
// without pulling in IR-specific headers.

// Arduino core #defines LOW/HIGH as macros, which text-substitute into any
// enumerator named LOW/HIGH regardless of enum class scoping — hence the
// FAN_ prefix here instead of bare AUTO/LOW/MED/HIGH.
enum class AcMode : uint8_t { COOL, DRY, FAN, AUTO, HEAT };
enum class FanSpeed : uint8_t { FAN_AUTO, FAN_LOW, FAN_MED, FAN_HIGH };

struct ACState {
  bool power = false;
  AcMode mode = AcMode::COOL;
  uint8_t temp = 24;
  FanSpeed fan = FanSpeed::FAN_AUTO;
};

constexpr uint8_t kAcMinTemp = 16;
constexpr uint8_t kAcMaxTemp = 30;

inline const char* acModeToString(AcMode mode) {
  switch (mode) {
    case AcMode::COOL: return "cool";
    case AcMode::DRY:  return "dry";
    case AcMode::FAN:  return "fan";
    case AcMode::AUTO: return "auto";
    case AcMode::HEAT: return "heat";
  }
  return "cool";
}

inline AcMode acModeFromString(const String& s, bool& ok) {
  ok = true;
  if (s == "cool") return AcMode::COOL;
  if (s == "dry")  return AcMode::DRY;
  if (s == "fan")  return AcMode::FAN;
  if (s == "auto") return AcMode::AUTO;
  if (s == "heat") return AcMode::HEAT;
  ok = false;
  return AcMode::COOL;
}

inline const char* fanSpeedToString(FanSpeed fan) {
  switch (fan) {
    case FanSpeed::FAN_AUTO: return "auto";
    case FanSpeed::FAN_LOW:  return "low";
    case FanSpeed::FAN_MED:  return "medium";
    case FanSpeed::FAN_HIGH: return "high";
  }
  return "auto";
}

inline FanSpeed fanSpeedFromString(const String& s, bool& ok) {
  ok = true;
  if (s == "auto")   return FanSpeed::FAN_AUTO;
  if (s == "low")    return FanSpeed::FAN_LOW;
  if (s == "medium") return FanSpeed::FAN_MED;
  if (s == "high")   return FanSpeed::FAN_HIGH;
  ok = false;
  return FanSpeed::FAN_AUTO;
}
