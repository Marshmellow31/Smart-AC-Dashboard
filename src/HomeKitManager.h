#pragma once

#include <Arduino.h>

#include <functional>

#include "AcCommand.h"

class AcController;
class EventLog;
class WeatherManager;

// Native Apple HomeKit bridge (HomeSpan). Follows the same pattern as
// SinricManager: HomeKit characteristic writes are translated into
// AcCommands and routed through AcController::apply() with
// CmdSource::HOMEKIT (manual-class: sets the hold, cancels programs);
// local state changes are mirrored back via pushState() from
// AcController's change callback. No AC logic lives here.
//
// Home app layout (bridge-style accessory):
//  - "AC" — HeaterCooler service: power, cool/heat/auto, target temperature,
//    fan speed (0=auto, 25/50/75=low/med/high), vertical swing.
//    DRY and FAN-only modes aren't representable in HomeKit and surface as
//    AUTO; changing them from the Home app sends the real mode picked there.
//  - "Turbo" / "Quiet" / "Purify" — Switch services for the feature toggles.
//
// CurrentTemperature (required by HomeKit) reports the outdoor temperature
// from WeatherManager (user's choice — there is no room sensor); it falls
// back to the setpoint until a weather reading exists.
//
// HomeSpan also owns Wi-Fi (credentials injected from secrets.h) and mDNS
// (hostname kept as "ac-controller"), replacing the old WiFiManager — two
// competing reconnect loops would fight over the radio. onWifiReady() lets
// main.cpp hook first-connect work (HTTP mDNS record, NetBIOS, NTP, Sinric).
//
// Pairing is optional: the accessory advertises but does nothing until
// paired. Default setup code 466-37-726, overridable via
// HOMEKIT_PAIRING_CODE in secrets.h.
class HomeKitManager {
 public:
  HomeKitManager(AcController& controller, EventLog& log);

  // Must be called from setup() (HomeSpan initializes its HAP database
  // there); network comes up later from its poll loop.
  void begin();
  void loop();  // homeSpan.poll() + periodic CurrentTemperature refresh

  void setWeatherManager(WeatherManager* weather) { weather_ = weather; }

  // Invoked once, right after HomeSpan brings Wi-Fi + mDNS + HAP up.
  void onWifiReady(std::function<void()> cb) { wifiReady_ = std::move(cb); }

  // Wired to AcController's change callback: mirrors state to Home app.
  void pushState(const ACState& s, CmdSource source);

  bool isConnected() const;

  // Called from HomeSpan's C-style callbacks via file-scope trampolines.
  void handleWifiReady();
  void handlePairing(bool paired);

 private:
  AcController& controller_;
  EventLog& log_;
  WeatherManager* weather_ = nullptr;
  std::function<void()> wifiReady_;
  unsigned long lastTempPushMs_ = 0;
};
