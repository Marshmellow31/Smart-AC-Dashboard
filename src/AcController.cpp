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

    if (source == CmdSource::AUTOMATION && time(nullptr) < overrideUntil_) {
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

    // No-op commands from the cloud/automation are dropped entirely: Sinric
    // replays retained state on every connect — transmitting those would
    // blast the AC with duplicate IR frames and set spurious holds. A MANUAL
    // no-op still transmits: re-pressing the same setting is how users nudge
    // an AC that missed a frame.
    bool changed = next.power != state_.power || next.mode != state_.mode ||
                   next.temp != state_.temp || next.fan != state_.fan;
    if (!changed && source != CmdSource::MANUAL) return true;

    state_ = next;

    if (source == CmdSource::MANUAL || source == CmdSource::SINRIC) {
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
