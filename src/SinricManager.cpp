#include "SinricManager.h"

#include <SinricPro.h>
#include <SinricProWindowAC.h>

#include "AcController.h"
#include "EventLog.h"
#include "secrets.h"

// Keep older secrets.h files (Wi-Fi only) compiling: Sinric stays disabled
// until the user adds real keys.
#ifndef SINRIC_APP_KEY
#define SINRIC_APP_KEY ""
#endif
#ifndef SINRIC_APP_SECRET
#define SINRIC_APP_SECRET ""
#endif
#ifndef SINRIC_AC_DEVICE_ID
#define SINRIC_AC_DEVICE_ID ""
#endif

namespace {

// Sinric thermostat modes are uppercase strings; we support the overlap with
// the AC's modes. DRY/FAN aren't standard thermostat modes — commands with
// them are rejected and local DRY/FAN states just aren't mirrored as a mode.
bool acModeFromSinric(const String& s, AcMode& out) {
  if (s == "COOL") { out = AcMode::COOL; return true; }
  if (s == "HEAT") { out = AcMode::HEAT; return true; }
  if (s == "AUTO") { out = AcMode::AUTO; return true; }
  return false;
}

const char* sinricModeFromAc(AcMode m) {
  switch (m) {
    case AcMode::COOL: return "COOL";
    case AcMode::HEAT: return "HEAT";
    case AcMode::AUTO: return "AUTO";
    default: return nullptr;
  }
}

int rangeFromFan(FanSpeed f) {
  switch (f) {
    case FanSpeed::FAN_LOW:  return 1;
    case FanSpeed::FAN_MED:  return 2;
    case FanSpeed::FAN_HIGH: return 3;
    default: return 0;  // auto — not representable as a range value
  }
}

// onRangeValue takes a plain function pointer (unlike the other callbacks,
// which take std::function), so no capturing lambda — hence this file-scope
// controller pointer.
AcController* g_rangeController = nullptr;

bool onFanRangeValue(const String&, int& value) {
  if (!g_rangeController) return false;
  AcCommand cmd;
  cmd.hasFan = true;
  switch (value) {
    case 1: cmd.fan = FanSpeed::FAN_LOW; break;
    case 2: cmd.fan = FanSpeed::FAN_MED; break;
    case 3: cmd.fan = FanSpeed::FAN_HIGH; break;
    default: return false;
  }
  g_rangeController->apply(cmd, CmdSource::SINRIC, "Sinric fan");
  return true;
}

}  // namespace

SinricManager::SinricManager(AcController& controller, EventLog& log)
    : controller_(controller), log_(log) {}

void SinricManager::begin() {
  if (began_) return;
  began_ = true;

  if (strlen(SINRIC_APP_KEY) == 0 || strlen(SINRIC_AC_DEVICE_ID) == 0) {
    log_.add(CmdSource::SYSTEM, "Sinric Pro disabled (no credentials in secrets.h)");
    return;
  }
  enabled_ = true;

  SinricProWindowAC& acUnit = SinricPro[SINRIC_AC_DEVICE_ID];

  acUnit.onPowerState([this](const String&, bool& state) {
    AcCommand cmd;
    cmd.hasPower = true;
    cmd.power = state;
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric power");
    return true;
  });

  acUnit.onTargetTemperature([this](const String&, float& temp) {
    int t = constrain((int)lroundf(temp), (int)kAcMinTemp, (int)kAcMaxTemp);
    AcCommand cmd;
    cmd.hasTemp = true;
    cmd.temp = static_cast<uint8_t>(t);
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric temperature");
    temp = static_cast<float>(t);
    return true;
  });

  acUnit.onAdjustTargetTemperature([this](const String&, float& delta) {
    int t = constrain((int)controller_.state().temp + (int)lroundf(delta),
                      (int)kAcMinTemp, (int)kAcMaxTemp);
    AcCommand cmd;
    cmd.hasTemp = true;
    cmd.temp = static_cast<uint8_t>(t);
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric temperature adjust");
    delta = static_cast<float>(t);  // callback returns the absolute value
    return true;
  });

  acUnit.onThermostatMode([this](const String&, String& mode) {
    AcMode m;
    if (!acModeFromSinric(mode, m)) return false;
    AcCommand cmd;
    cmd.hasMode = true;
    cmd.mode = m;
    cmd.hasPower = true;  // Alexa mode changes imply the unit should run
    cmd.power = true;
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric mode");
    return true;
  });

  g_rangeController = &controller_;
  acUnit.onRangeValue(onFanRangeValue);

  SinricPro.onConnected([this]() {
    log_.add(CmdSource::SYSTEM, "Sinric Pro connected");
  });
  SinricPro.onDisconnected([this]() {
    log_.add(CmdSource::SYSTEM, "Sinric Pro disconnected");
  });

  SinricPro.begin(SINRIC_APP_KEY, SINRIC_APP_SECRET);
  Serial.println("Sinric Pro started.");
}

void SinricManager::loop() {
  if (enabled_) SinricPro.handle();
}

void SinricManager::pushState(const ACState& s, CmdSource source) {
  // Skip only the echo of our own commands — changes from Firebase (CLOUD)
  // must still be mirrored here so Alexa/Google stay in sync.
  if (!enabled_ || source == CmdSource::SINRIC) return;
  if (!SinricPro.isConnected()) return;

  SinricProWindowAC& acUnit = SinricPro[SINRIC_AC_DEVICE_ID];
  acUnit.sendPowerStateEvent(s.power);
  acUnit.sendTargetTemperatureEvent(static_cast<float>(s.temp));
  const char* mode = sinricModeFromAc(s.mode);
  if (mode) acUnit.sendThermostatModeEvent(mode);
  int range = rangeFromFan(s.fan);
  if (range > 0) acUnit.sendRangeValueEvent(range);
}
