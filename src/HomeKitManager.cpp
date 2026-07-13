#include "HomeKitManager.h"

#include <HomeSpan.h>

#include "AcController.h"
#include "EventLog.h"
#include "WeatherManager.h"
#include "secrets.h"

#ifndef HOMEKIT_PAIRING_CODE
#define HOMEKIT_PAIRING_CODE ""  // empty = HomeSpan default 466-37-726
#endif

namespace {

HomeKitManager* g_manager = nullptr;

// --- value mapping helpers -------------------------------------------------

// HomeKit TargetHeaterCoolerState: 0=AUTO 1=HEAT 2=COOL. DRY/FAN-only have
// no HomeKit representation and surface as AUTO.
uint8_t targetStateFromMode(AcMode m) {
  switch (m) {
    case AcMode::COOL: return 2;
    case AcMode::HEAT: return 1;
    default: return 0;
  }
}

AcMode modeFromTargetState(int v) {
  switch (v) {
    case 2: return AcMode::COOL;
    case 1: return AcMode::HEAT;
    default: return AcMode::AUTO;
  }
}

// CurrentHeaterCoolerState: 0=INACTIVE 1=IDLE 2=HEATING 3=COOLING. Without a
// room sensor there is no "idle" to detect — powered means running.
uint8_t currentStateFrom(const ACState& s) {
  if (!s.power) return 0;
  return s.mode == AcMode::HEAT ? 2 : 3;
}

// RotationSpeed (0-100, step 25): 0=auto, 25/50/75=low/med/high. 100 also
// reads as high so dragging the slider to the top does something sensible.
int rotationFromFan(FanSpeed f) {
  switch (f) {
    case FanSpeed::FAN_LOW:  return 25;
    case FanSpeed::FAN_MED:  return 50;
    case FanSpeed::FAN_HIGH: return 75;
    default: return 0;
  }
}

FanSpeed fanFromRotation(int v) {
  if (v <= 0) return FanSpeed::FAN_AUTO;
  if (v <= 37) return FanSpeed::FAN_LOW;
  if (v <= 62) return FanSpeed::FAN_MED;
  return FanSpeed::FAN_HIGH;
}

}  // namespace

// --- HomeSpan services -------------------------------------------------------

struct AcHeaterCoolerService : Service::HeaterCooler {
  AcController& ctl;
  SpanCharacteristic* active;
  SpanCharacteristic* curTemp;
  SpanCharacteristic* curState;
  SpanCharacteristic* tgtState;
  SpanCharacteristic* coolTemp;
  SpanCharacteristic* heatTemp;
  SpanCharacteristic* rotation;
  SpanCharacteristic* swing;

  explicit AcHeaterCoolerService(AcController& controller) : ctl(controller) {
    ACState s = ctl.state();
    active = new Characteristic::Active(s.power ? 1 : 0);
    curTemp = new Characteristic::CurrentTemperature(s.temp);
    curTemp->setRange(-20, 60);  // outdoor reading; Bharuch summers run hot
    curState = new Characteristic::CurrentHeaterCoolerState(currentStateFrom(s));
    tgtState = new Characteristic::TargetHeaterCoolerState(targetStateFromMode(s.mode));
    coolTemp = new Characteristic::CoolingThresholdTemperature(s.temp);
    coolTemp->setRange(kAcMinTemp, kAcMaxTemp, 1);
    heatTemp = new Characteristic::HeatingThresholdTemperature(s.temp);
    heatTemp->setRange(kAcMinTemp, kAcMaxTemp, 1);
    rotation = new Characteristic::RotationSpeed(rotationFromFan(s.fan));
    rotation->setRange(0, 100, 25);
    swing = new Characteristic::SwingMode(s.swing ? 1 : 0);
  }

  boolean update() override {
    AcCommand cmd;
    if (active->updated()) {
      cmd.hasPower = true;
      cmd.power = active->getNewVal() == 1;
    }
    if (tgtState->updated()) {
      cmd.hasMode = true;
      cmd.mode = modeFromTargetState(tgtState->getNewVal());
    }
    // The AC has a single setpoint; either threshold write maps onto it.
    if (coolTemp->updated()) {
      cmd.hasTemp = true;
      cmd.temp = static_cast<uint8_t>(coolTemp->getNewVal());
    } else if (heatTemp->updated()) {
      cmd.hasTemp = true;
      cmd.temp = static_cast<uint8_t>(heatTemp->getNewVal());
    }
    if (rotation->updated()) {
      cmd.hasFan = true;
      cmd.fan = fanFromRotation(rotation->getNewVal());
    }
    if (swing->updated()) {
      cmd.hasFeature[FEAT_SWING] = true;
      cmd.feature[FEAT_SWING] = swing->getNewVal() == 1;
    }
    if (cmd.hasPower || cmd.hasMode || cmd.hasTemp || cmd.hasFan ||
        cmd.hasFeature[FEAT_SWING]) {
      ctl.apply(cmd, CmdSource::HOMEKIT, "HomeKit");
    }
    return true;
  }
};

struct AcFeatureSwitch : Service::Switch {
  AcController& ctl;
  FeatureId feat;
  const char* reason;
  SpanCharacteristic* on;

  AcFeatureSwitch(AcController& controller, FeatureId f, const char* why, bool initial)
      : ctl(controller), feat(f), reason(why) {
    on = new Characteristic::On(initial);
  }

  boolean update() override {
    AcCommand cmd;
    cmd.hasFeature[feat] = true;
    cmd.feature[feat] = on->getNewVal() == 1;
    ctl.apply(cmd, CmdSource::HOMEKIT, reason);
    return true;
  }
};

namespace {
AcHeaterCoolerService* g_ac = nullptr;
AcFeatureSwitch* g_turbo = nullptr;
AcFeatureSwitch* g_quiet = nullptr;
AcFeatureSwitch* g_purify = nullptr;

void wifiReadyTrampoline() {
  if (g_manager) g_manager->handleWifiReady();
}

void wifiConnectAllTrampoline(int count) {
  // Modem power-save naps the radio and drops long-lived connections
  // (Sinric's websocket, HAP event streams) — keep it off on every connect.
  WiFi.setSleep(false);
}
}  // namespace

// --- manager -----------------------------------------------------------------

HomeKitManager::HomeKitManager(AcController& controller, EventLog& log)
    : controller_(controller), log_(log) {}

void HomeKitManager::begin() {
  g_manager = this;

  // Start the Wi-Fi driver (and with it the LwIP TCP/IP task) now: HomeSpan
  // itself only touches Wi-Fi from its first poll(), but AsyncWebServer
  // binds its listening socket during setup() and asserts ("Invalid mbox")
  // if the stack isn't up yet.
  WiFi.mode(WIFI_STA);

  homeSpan.setHostNameSuffix("");  // keep the historic "ac-controller" name
  homeSpan.setPortNum(1201);       // default is 80 — that's the dashboard's
  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASSWORD);
  homeSpan.setWifiCallback(wifiReadyTrampoline);
  homeSpan.setWifiCallbackAll(wifiConnectAllTrampoline);
  homeSpan.setPairCallback([](boolean paired) {
    if (g_manager) g_manager->handlePairing(paired);
  });
  if (strlen(HOMEKIT_PAIRING_CODE) > 0) {
    homeSpan.setPairingCode(HOMEKIT_PAIRING_CODE);
  }

  homeSpan.begin(Category::Bridges, "AC Controller", "ac-controller", "ESP32-AC");

  ACState s = controller_.state();

  new SpanAccessory();  // bridge root
  new Service::AccessoryInformation();
  new Characteristic::Identify();

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("AC");
  g_ac = new AcHeaterCoolerService(controller_);

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Turbo");
  g_turbo = new AcFeatureSwitch(controller_, FEAT_TURBO, "HomeKit turbo", s.turbo);

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Quiet");
  g_quiet = new AcFeatureSwitch(controller_, FEAT_QUIET, "HomeKit quiet", s.quiet);

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Purify");
  g_purify = new AcFeatureSwitch(controller_, FEAT_ION, "HomeKit purify", s.ion);
}

void HomeKitManager::loop() {
  homeSpan.poll();

  // Refresh the reported "current temperature" (outdoor, per user choice;
  // setpoint until the first weather reading arrives) once a minute.
  unsigned long nowMs = millis();
  if (g_ac && nowMs - lastTempPushMs_ >= 60000) {
    lastTempPushMs_ = nowMs;
    float t = (weather_ && weather_->isValid()) ? weather_->outdoorTempC()
                                                : controller_.state().temp;
    if (fabsf(g_ac->curTemp->getVal<float>() - t) >= 0.1f) g_ac->curTemp->setVal(t);
  }
}

void HomeKitManager::handleWifiReady() {
  log_.add(CmdSource::SYSTEM, "HomeKit up at %s", WiFi.localIP().toString().c_str());
  if (wifiReady_) wifiReady_();
}

void HomeKitManager::handlePairing(bool paired) {
  log_.add(CmdSource::SYSTEM, "HomeKit %s", paired ? "paired" : "unpaired");
}

bool HomeKitManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

void HomeKitManager::pushState(const ACState& s, CmdSource source) {
  if (!g_ac) return;
  // Unlike Sinric there is no echo loop to break: setVal() after our own
  // update() is a cheap no-op thanks to the diff checks, and pushing
  // unconditionally keeps side effects (turbo↔quiet exclusivity) in sync.
  (void)source;

  if (g_ac->active->getVal() != (s.power ? 1 : 0)) g_ac->active->setVal(s.power ? 1 : 0);
  uint8_t cur = currentStateFrom(s);
  if (g_ac->curState->getVal() != cur) g_ac->curState->setVal(cur);
  uint8_t tgt = targetStateFromMode(s.mode);
  if (g_ac->tgtState->getVal() != tgt) g_ac->tgtState->setVal(tgt);
  if (g_ac->coolTemp->getVal() != s.temp) g_ac->coolTemp->setVal(s.temp);
  if (g_ac->heatTemp->getVal() != s.temp) g_ac->heatTemp->setVal(s.temp);
  int rot = rotationFromFan(s.fan);
  if (g_ac->rotation->getVal() != rot) g_ac->rotation->setVal(rot);
  if (g_ac->swing->getVal() != (s.swing ? 1 : 0)) g_ac->swing->setVal(s.swing ? 1 : 0);

  if (g_turbo->on->getVal() != (s.turbo ? 1 : 0)) g_turbo->on->setVal(s.turbo ? 1 : 0);
  if (g_quiet->on->getVal() != (s.quiet ? 1 : 0)) g_quiet->on->setVal(s.quiet ? 1 : 0);
  if (g_purify->on->getVal() != (s.ion ? 1 : 0)) g_purify->on->setVal(s.ion ? 1 : 0);
}
