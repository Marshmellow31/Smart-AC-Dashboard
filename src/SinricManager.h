#pragma once

#include <Arduino.h>

#include "AcCommand.h"

class AcController;
class EventLog;

// Sinric Pro cloud bridge → Alexa / Google Home / Sinric app.
//
// Devices:
//  - Window AC (AC_UNIT, required): power, target temperature, thermostat
//    mode, and fan speed as a range value 1-3. Google Home only surfaces
//    power / temperature / mode (off, cool, fan-only, on) for AC units —
//    the fan range is Alexa-only.
//  - Fan (FAN, optional via SINRIC_FAN_DEVICE_ID in secrets.h): mirrors the
//    AC's fan so Google Home gets a speed control too. Its power state is
//    the AC's power state.
//
// Anti-ghost policy: right after a (re)connect the cloud tends to deliver
// stale commands queued or retained while the device was offline — the event
// log showed bursts of [sinric] lines immediately after every "Sinric Pro
// connected", overriding fresh local changes and turning the AC on by
// itself. On connect we push the device's real state to the cloud, and any
// command arriving within the grace window is answered with the current
// state instead of being applied.
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
  // changes to the cloud so the Alexa/Google apps stay in sync.
  void pushState(const ACState& s, CmdSource source);

  // Shared by the AC unit's and the fan device's range callbacks (which are
  // plain function pointers, hence routed through a file-scope trampoline).
  bool handleFanRange(int& value);

 private:
  void onCloudConnected();
  bool ignoreStale(const char* what);  // true = within the reconnect grace

  AcController& controller_;
  EventLog& log_;
  bool enabled_ = false;
  bool began_ = false;
  bool fanEnabled_ = false;
  bool everConnected_ = false;
  unsigned long connectedAtMs_ = 0;
};
