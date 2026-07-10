#pragma once

#include <Arduino.h>

#include "AcCommand.h"

class AcController;
class EventLog;

// Sinric Pro cloud bridge → Alexa / Google Home / Sinric app.
// Uses the WindowAC device type: power, target temperature, thermostat mode
// (COOL/HEAT/AUTO), and fan speed as a range value 1-3.
//
// Callbacks arrive from SinricPro.handle(), which we run in the main loop,
// so calling AcController::apply() from them is safe (the IR send itself
// still happens in AcController::loop()).
//
// Disabled automatically when the Sinric credentials in secrets.h are left
// empty, so the firmware builds and runs without an account.
class SinricManager {
 public:
  SinricManager(AcController& controller, EventLog& log);

  void begin();  // call once after Wi-Fi is first connected
  void loop();

  // Wired to AcController's change callback: mirrors local/automation state
  // changes to the cloud so the Alexa app stays in sync.
  void pushState(const ACState& s, CmdSource source);

 private:
  AcController& controller_;
  EventLog& log_;
  bool enabled_ = false;
  bool began_ = false;
};
