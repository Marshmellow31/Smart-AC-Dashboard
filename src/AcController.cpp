#include "AcController.h"

#include "ConfigStore.h"
#include "EventLog.h"

namespace {

constexpr const char* kStatePath = "/cfg/state.json";

uint8_t irModeFromAcMode(AcMode mode) {
  switch (mode) {
    case AcMode::COOL: return kSamsungAcCool;
    case AcMode::DRY:  return kSamsungAcDry;
    case AcMode::FAN:  return kSamsungAcFan;
    case AcMode::AUTO: return kSamsungAcAuto;
    case AcMode::HEAT: return kSamsungAcHeat;
  }
  return kSamsungAcCool;
}

uint8_t irFanFromFanSpeed(FanSpeed fan) {
  switch (fan) {
    case FanSpeed::FAN_AUTO: return kSamsungAcFanAuto;
    case FanSpeed::FAN_LOW:  return kSamsungAcFanLow;
    case FanSpeed::FAN_MED:  return kSamsungAcFanMed;
    case FanSpeed::FAN_HIGH: return kSamsungAcFanHigh;
  }
  return kSamsungAcFanAuto;
}

}  // namespace

AcController::AcController(IRSamsungAc& ac, EventLog& log, AppSettings& settings)
    : ac_(ac), log_(log), settings_(settings) {}

void AcController::begin() {
  JsonDocument doc;
  if (ConfigStore::load(kStatePath, doc)) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.power = doc["power"] | false;
    bool ok = false;
    AcMode m = acModeFromString(doc["mode"] | String("cool"), ok);
    if (ok) state_.mode = m;
    FanSpeed f = fanSpeedFromString(doc["fan"] | String("auto"), ok);
    if (ok) state_.fan = f;
    uint8_t t = doc["temp"] | 24;
    state_.temp = constrain(t, kAcMinTemp, kAcMaxTemp);
    bool* fields[AcCommand::kFeatureCount];
    featureFieldsOf(state_, fields);
    for (size_t i = 0; i < AcCommand::kFeatureCount; i++) {
      if (doc[kFeatureKeys[i]].is<bool>()) *fields[i] = doc[kFeatureKeys[i]].as<bool>();
    }
  }

  if (settings_.restoreOnBoot && state_.power) {
    std::lock_guard<std::mutex> lock(mutex_);
    sendPending_ = true;
    lastSource_ = CmdSource::BOOT;
    log_.add(CmdSource::BOOT, "restoring last state after boot: %s %u\xC2\xB0 %s",
             acModeToString(state_.mode), state_.temp, fanSpeedToString(state_.fan));
  }
}

bool AcController::apply(const AcCommand& cmd, CmdSource source, const char* reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Manual hold is asymmetric: automations may always turn the AC OFF
    // (auto-off, program end times, safety never get blocked), but anything
    // else — turning on, changing settings — waits for the hold to expire.
    bool turnsOff = cmd.hasPower && !cmd.power;
    if (source == CmdSource::AUTOMATION && !turnsOff &&
        time(nullptr) < overrideUntil_) {
      // Log outside the lock is nicer, but add() has its own mutex and never
      // calls back into us, so this is deadlock-free.
      log_.add(CmdSource::AUTOMATION, "blocked by manual hold: %s%s%s",
               acCommandToString(cmd).c_str(), reason ? " — " : "",
               reason ? reason : "");
      return false;
    }

    ACState next = state_;
    if (cmd.hasPower) next.power = cmd.power;
    if (cmd.hasMode) next.mode = cmd.mode;
    if (cmd.hasTemp) next.temp = constrain(cmd.temp, kAcMinTemp, kAcMaxTemp);
    if (cmd.hasFan) next.fan = cmd.fan;
    bool* fields[AcCommand::kFeatureCount];
    featureFieldsOf(next, fields);
    for (size_t i = 0; i < AcCommand::kFeatureCount; i++) {
      if (cmd.hasFeature[i]) *fields[i] = cmd.feature[i];
    }
    // The AC treats these as mutually exclusive; mirror that so our state
    // never disagrees with the unit about which one won.
    if (cmd.hasFeature[FEAT_TURBO] && cmd.feature[FEAT_TURBO]) next.quiet = false;
    if (cmd.hasFeature[FEAT_QUIET] && cmd.feature[FEAT_QUIET]) next.turbo = false;

    // No-op commands from the cloud/automation are dropped entirely: Sinric
    // replays retained state on every connect — transmitting those would
    // blast the AC with duplicate IR frames and set spurious holds. A MANUAL
    // no-op still transmits: re-pressing the same setting is how users nudge
    // an AC that missed a frame.
    bool changed = next.power != state_.power || next.mode != state_.mode ||
                   next.temp != state_.temp || next.fan != state_.fan ||
                   next.swing != state_.swing || next.turbo != state_.turbo ||
                   next.quiet != state_.quiet || next.econo != state_.econo ||
                   next.clean != state_.clean || next.ion != state_.ion ||
                   next.display != state_.display || next.beep != state_.beep;
    if (!changed && source != CmdSource::MANUAL) return true;

    state_ = next;

    if (source == CmdSource::MANUAL || source == CmdSource::SINRIC ||
        source == CmdSource::HOMEKIT) {
      overrideUntil_ = time(nullptr) + static_cast<time_t>(settings_.holdMinutes) * 60;
    }

    sendPending_ = true;
    savePending_ = true;
    notifyPending_ = true;
    lastSource_ = source;
  }

  log_.add(source, "%s%s%s", acCommandToString(cmd).c_str(),
           reason ? " — " : "", reason ? reason : "");
  return true;
}

void AcController::loop() {
  ACState snapshot;
  CmdSource source;
  bool doSend, doSave, doNotify;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    doSend = sendPending_;
    doSave = savePending_;
    doNotify = notifyPending_;
    sendPending_ = savePending_ = notifyPending_ = false;
    snapshot = state_;
    source = lastSource_;
  }

  if (doSend) transmit(snapshot);
  if (doSave) persistState();
  if (doNotify) {
    for (auto& cb : callbacks_) cb(snapshot, source);
  }
}

void AcController::transmit(const ACState& s) {
  // Full state on every transmit, matching a real remote: Samsung ACs expect
  // the complete settings frame, not deltas.
  ac_.setPower(s.power);
  ac_.setMode(irModeFromAcMode(s.mode));
  ac_.setTemp(s.temp);
  ac_.setFan(irFanFromFanSpeed(s.fan));
  ac_.setSwing(s.swing);
  ac_.setPowerful(s.turbo);
  ac_.setQuiet(s.quiet);
  ac_.setEcono(s.econo);
  ac_.setClean(s.clean);
  ac_.setIon(s.ion);
  ac_.setDisplay(s.display);
  ac_.setBeep(s.beep);
  ac_.send();

  Serial.printf("IR sent: power=%s mode=%s temp=%u fan=%s\n",
                s.power ? "on" : "off", acModeToString(s.mode), s.temp,
                fanSpeedToString(s.fan));
}

void AcController::persistState() {
  ACState s = state();
  JsonDocument doc;
  doc["power"] = s.power;
  doc["mode"] = acModeToString(s.mode);
  doc["temp"] = s.temp;
  doc["fan"] = fanSpeedToString(s.fan);
  bool* fields[AcCommand::kFeatureCount];
  featureFieldsOf(s, fields);
  for (size_t i = 0; i < AcCommand::kFeatureCount; i++) {
    doc[kFeatureKeys[i]] = *fields[i];
  }
  ConfigStore::save(kStatePath, doc);
}

ACState AcController::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool AcController::overrideActive() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return time(nullptr) < overrideUntil_;
}

time_t AcController::overrideUntil() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return overrideUntil_;
}

void AcController::clearOverride() {
  std::lock_guard<std::mutex> lock(mutex_);
  overrideUntil_ = 0;
}

void AcController::addChangeCallback(ChangeCallback cb) {
  callbacks_.push_back(std::move(cb));
}
