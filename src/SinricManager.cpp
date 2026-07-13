#include "SinricManager.h"

#include <SinricPro.h>
#include <SinricProFanUS.h>
#include <SinricProWindowAC.h>

#include "AcController.h"
#include "EventLog.h"
#include "secrets.h"

// Keep older secrets.h files (Wi-Fi only) compiling: Sinric stays disabled
// until the user adds real keys. SINRIC_FAN_DEVICE_ID is optional on top —
// a Fan-type device that gives Google Home a fan-speed control (the AC
// unit's range value only surfaces on Alexa).
#ifndef SINRIC_APP_KEY
#define SINRIC_APP_KEY ""
#endif
#ifndef SINRIC_APP_SECRET
#define SINRIC_APP_SECRET ""
#endif
#ifndef SINRIC_AC_DEVICE_ID
#define SINRIC_AC_DEVICE_ID ""
#endif
#ifndef SINRIC_FAN_DEVICE_ID
#define SINRIC_FAN_DEVICE_ID ""
#endif

namespace {

// Cloud commands arriving within this window of a (re)connect are treated as
// stale replays (see the class comment in SinricManager.h) and answered with
// the device's current state instead of being applied.
constexpr unsigned long kConnectGraceMs = 5000;

// Inbound mode strings vary by platform: Alexa sends COOL/HEAT/AUTO/ECO,
// Google Home can also send FAN-ONLY. OFF/ON arrive as modes too and are
// handled separately in the callback (they map to power, not mode).
bool acModeFromSinric(String s, AcMode& out) {
  s.toUpperCase();
  if (s == "COOL") { out = AcMode::COOL; return true; }
  if (s == "HEAT") { out = AcMode::HEAT; return true; }
  if (s == "AUTO" || s == "AUTOMATIC" || s == "ECO") { out = AcMode::AUTO; return true; }
  if (s == "FAN" || s == "FAN-ONLY" || s == "FAN_ONLY" || s == "FANONLY") {
    out = AcMode::FAN;
    return true;
  }
  if (s == "DRY" || s == "DEHUMIDIFY") { out = AcMode::DRY; return true; }
  return false;
}

const char* sinricModeFromAc(AcMode m) {
  switch (m) {
    case AcMode::COOL: return "COOL";
    case AcMode::HEAT: return "HEAT";
    case AcMode::AUTO: return "AUTO";
    case AcMode::FAN:  return "FAN";  // Google's "fan-only"; Alexa ignores it
    default: return nullptr;          // DRY isn't representable on either
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
// manager pointer, shared by the AC unit and the optional fan device.
SinricManager* g_manager = nullptr;

bool onFanRangeValue(const String&, int& value) {
  return g_manager && g_manager->handleFanRange(value);
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
  fanEnabled_ = strlen(SINRIC_FAN_DEVICE_ID) > 0;
  g_manager = this;

  SinricProWindowAC& acUnit = SinricPro[SINRIC_AC_DEVICE_ID];

  acUnit.onPowerState([this](const String&, bool& state) {
    if (ignoreStale("power")) {
      state = controller_.state().power;  // ack with the real state instead
      return true;
    }
    AcCommand cmd;
    cmd.hasPower = true;
    cmd.power = state;
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric power");
    return true;
  });

  acUnit.onTargetTemperature([this](const String&, float& temp) {
    if (ignoreStale("temperature")) {
      temp = static_cast<float>(controller_.state().temp);
      return true;
    }
    int t = constrain((int)lroundf(temp), (int)kAcMinTemp, (int)kAcMaxTemp);
    AcCommand cmd;
    cmd.hasTemp = true;
    cmd.temp = static_cast<uint8_t>(t);
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric temperature");
    temp = static_cast<float>(t);
    return true;
  });

  acUnit.onAdjustTargetTemperature([this](const String&, float& delta) {
    if (ignoreStale("temperature adjust")) {
      delta = static_cast<float>(controller_.state().temp);
      return true;
    }
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
    if (ignoreStale("mode")) {
      const char* cur = sinricModeFromAc(controller_.state().mode);
      if (cur) mode = cur;
      return true;
    }
    String m = mode;
    m.toUpperCase();
    AcCommand cmd;
    if (m == "OFF") {  // Google sends OFF/ON as thermostat modes
      cmd.hasPower = true;
      cmd.power = false;
    } else if (m == "ON") {
      cmd.hasPower = true;
      cmd.power = true;
    } else {
      AcMode am;
      if (!acModeFromSinric(m, am)) return false;
      // Deliberately mode-only: mode commands used to imply power-on, which
      // let stale cloud replays turn the AC on by itself. "Turn on the AC"
      // still works — it arrives as a power command.
      cmd.hasMode = true;
      cmd.mode = am;
    }
    controller_.apply(cmd, CmdSource::SINRIC, "Sinric mode");
    return true;
  });

  acUnit.onRangeValue(onFanRangeValue);

  if (fanEnabled_) {
    SinricProFanUS& fan = SinricPro[SINRIC_FAN_DEVICE_ID];
    // The fan device *is* the AC's fan, so its power is the AC's power.
    fan.onPowerState([this](const String&, bool& state) {
      if (ignoreStale("fan power")) {
        state = controller_.state().power;
        return true;
      }
      AcCommand cmd;
      cmd.hasPower = true;
      cmd.power = state;
      controller_.apply(cmd, CmdSource::SINRIC, "Sinric fan power");
      return true;
    });
    fan.onRangeValue(onFanRangeValue);
  }

  SinricPro.onConnected([this]() { onCloudConnected(); });
  SinricPro.onDisconnected([this]() {
    log_.add(CmdSource::SYSTEM, "Sinric Pro disconnected");
  });

  SinricPro.begin(SINRIC_APP_KEY, SINRIC_APP_SECRET);
  Serial.println("Sinric Pro started.");
}

void SinricManager::loop() {
  if (enabled_) SinricPro.handle();
}

bool SinricManager::handleFanRange(int& value) {
  if (ignoreStale("fan speed")) {
    int cur = rangeFromFan(controller_.state().fan);
    if (cur == 0) return false;  // AUTO isn't representable as 1-3
    value = cur;
    return true;
  }
  AcCommand cmd;
  cmd.hasFan = true;
  switch (value) {
    case 1: cmd.fan = FanSpeed::FAN_LOW; break;
    case 2: cmd.fan = FanSpeed::FAN_MED; break;
    case 3: cmd.fan = FanSpeed::FAN_HIGH; break;
    default: return false;
  }
  controller_.apply(cmd, CmdSource::SINRIC, "Sinric fan");
  return true;
}

void SinricManager::onCloudConnected() {
  connectedAtMs_ = millis();
  everConnected_ = true;
  log_.add(CmdSource::SYSTEM, "Sinric Pro connected");
  // Correct the cloud's idea of the device right away — whatever changed
  // while we were offline is stale up there until this push lands.
  pushState(controller_.state(), CmdSource::SYSTEM);
}

bool SinricManager::ignoreStale(const char* what) {
  if (!everConnected_ || millis() - connectedAtMs_ >= kConnectGraceMs) return false;
  log_.add(CmdSource::SYSTEM, "ignored stale Sinric %s (reconnect replay)", what);
  return true;
}

void SinricManager::pushState(const ACState& s, CmdSource source) {
  // Skip the echo of our own commands; everything else (web UI, automation,
  // timers) is mirrored so Alexa/Google stay in sync.
  if (!enabled_ || source == CmdSource::SINRIC) return;
  if (!SinricPro.isConnected()) return;

  SinricProWindowAC& acUnit = SinricPro[SINRIC_AC_DEVICE_ID];
  acUnit.sendPowerStateEvent(s.power);
  acUnit.sendTargetTemperatureEvent(static_cast<float>(s.temp));
  const char* mode = sinricModeFromAc(s.mode);
  if (mode) acUnit.sendThermostatModeEvent(mode);
  int range = rangeFromFan(s.fan);
  if (range > 0) acUnit.sendRangeValueEvent(range);

  if (fanEnabled_) {
    SinricProFanUS& fan = SinricPro[SINRIC_FAN_DEVICE_ID];
    fan.sendPowerStateEvent(s.power);
    if (range > 0) fan.sendRangeValueEvent(range);
  }
}
