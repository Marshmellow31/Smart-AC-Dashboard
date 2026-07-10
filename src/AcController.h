#pragma once

#include <ir_Samsung.h>

#include <functional>
#include <mutex>
#include <vector>

#include "AcCommand.h"
#include "AppSettings.h"

class EventLog;

// Single source of truth for AC state and the only module that touches the
// IR transmitter. Everyone else (HTTP, Sinric cloud, automation engine)
// requests changes via apply().
//
// IR sends happen exclusively in loop() (Arduino main loop): the 38kHz
// modulation is software bit-banged with microsecond busy-waits, and doing it
// from the async TCP task corrupts the waveform. apply() only mutates state
// and raises sendPending_.
//
// Override policy ("manual wins"): MANUAL and CLOUD commands start a hold of
// settings.holdMinutes during which AUTOMATION commands are rejected.
// TIMER (user-set countdown) and SAFETY (auto-off) bypass the hold.
class AcController {
 public:
  using ChangeCallback = std::function<void(const ACState&, CmdSource)>;

  AcController(IRSamsungAc& ac, EventLog& log, AppSettings& settings);

  void begin();  // loads persisted state; re-sends it if restoreOnBoot
  void loop();   // IR send + deferred persistence + change callbacks

  // Returns false when an AUTOMATION command is blocked by the manual hold.
  bool apply(const AcCommand& cmd, CmdSource source, const char* reason = nullptr);

  ACState state() const;
  bool overrideActive() const;
  time_t overrideUntil() const;
  void clearOverride();

  // Callbacks fire from loop() (main loop context), never from async tasks.
  void addChangeCallback(ChangeCallback cb);

 private:
  void transmit(const ACState& s);
  void persistState();

  IRSamsungAc& ac_;
  EventLog& log_;
  AppSettings& settings_;

  mutable std::mutex mutex_;
  ACState state_;
  time_t overrideUntil_ = 0;
  bool sendPending_ = false;
  bool savePending_ = false;
  bool notifyPending_ = false;
  CmdSource lastSource_ = CmdSource::BOOT;

  std::vector<ChangeCallback> callbacks_;
};
