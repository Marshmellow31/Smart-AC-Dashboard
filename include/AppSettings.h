#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// User-tunable settings, persisted to /cfg/settings.json. All fields are
// 32-bit or smaller so cross-task reads without a lock can't tear.
struct AppSettings {
  uint16_t holdMinutes = 120;      // manual-wins hold window for automations
  bool automationEnabled = true;   // master toggle for weekly schedules
  bool restoreOnBoot = false;      // re-send last state after a power cut
  uint16_t acWatts = 1560;         // for the energy estimate
  float tariffPerKwh = 8.0f;       // ₹ per kWh
  uint16_t filterLimitHours = 200; // filter-clean reminder threshold
  uint8_t maxContinuousHours = 0;  // safety auto-off; 0 = disabled

  void toJson(JsonObject o) const {
    o["holdMinutes"] = holdMinutes;
    o["automationEnabled"] = automationEnabled;
    o["restoreOnBoot"] = restoreOnBoot;
    o["acWatts"] = acWatts;
    o["tariffPerKwh"] = tariffPerKwh;
    o["filterLimitHours"] = filterLimitHours;
    o["maxContinuousHours"] = maxContinuousHours;
  }

  void fromJson(JsonObjectConst o) {
    if (!o["holdMinutes"].isNull()) holdMinutes = o["holdMinutes"].as<uint16_t>();
    if (o["automationEnabled"].is<bool>()) automationEnabled = o["automationEnabled"].as<bool>();
    if (o["restoreOnBoot"].is<bool>()) restoreOnBoot = o["restoreOnBoot"].as<bool>();
    if (!o["acWatts"].isNull()) acWatts = o["acWatts"].as<uint16_t>();
    if (!o["tariffPerKwh"].isNull()) tariffPerKwh = o["tariffPerKwh"].as<float>();
    if (!o["filterLimitHours"].isNull()) filterLimitHours = o["filterLimitHours"].as<uint16_t>();
    if (!o["maxContinuousHours"].isNull()) maxContinuousHours = o["maxContinuousHours"].as<uint8_t>();
  }
};
