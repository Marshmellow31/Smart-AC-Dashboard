#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ir_Samsung.h>

#include "AcController.h"
#include "AppSettings.h"
#include "AutomationEngine.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "SinricManager.h"
#include "FirebaseManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "WebServerManager.h"
#include "WiFiManager.h"

const uint16_t IR_SEND_PIN = 4;
const char* kMdnsHostname = "ac-controller";
const char* kSettingsPath = "/cfg/settings.json";

IRSamsungAc ac(IR_SEND_PIN);
WiFiManager wifiManager;
EventLog eventLog;
AppSettings settings;
TimeManager timeManager(eventLog);
AcController acController(ac, eventLog, settings);
AutomationEngine automation(acController, timeManager, eventLog, settings);
StatsManager stats(acController, timeManager, eventLog, settings);
SinricManager sinric(acController, eventLog);
FirebaseManager firebase(acController, eventLog);
WebServerManager webServer(acController, automation, stats, settings, timeManager, eventLog);

void startMdns() {
  MDNS.end();
  if (MDNS.begin(kMdnsHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local/\n", kMdnsHostname);
  } else {
    Serial.println("mDNS responder failed to start.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 AC Controller - Starting");

  ac.begin();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
  }
  ConfigStore::begin();

  {
    JsonDocument doc;
    if (ConfigStore::load(kSettingsPath, doc)) {
      settings.fromJson(doc.as<JsonObjectConst>());
    }
  }

  acController.begin();
  automation.begin();
  stats.begin();

  // External (manual/cloud/timer/safety) commands cancel a running program;
  // every change is mirrored to Sinric and Firebase so clouds stay in sync.
  acController.addChangeCallback([](const ACState& s, CmdSource source) {
    automation.onExternalCommand(source);
    sinric.pushState(s, source);
    firebase.pushState(s, source);
  });

  wifiManager.begin();
  webServer.begin();

  eventLog.add(CmdSource::SYSTEM, "boot complete");
}

void loop() {
  wifiManager.loop();

  static bool wasConnected = false;
  bool nowConnected = wifiManager.isConnected();
  if (nowConnected && !wasConnected) {
    startMdns();
    timeManager.onWifiConnected();
    sinric.begin();
    firebase.begin();
  }
  wasConnected = nowConnected;

  timeManager.loop();
  acController.loop();
  automation.loop();
  stats.loop();
  sinric.loop();
  firebase.loop();
  webServer.loop();

  delay(10);
}
