#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <NetBIOS.h>
#include <WiFi.h>
#include <ir_Samsung.h>

#include "AcController.h"
#include "AppSettings.h"
#include "AutomationEngine.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "HomeKitManager.h"
#include "SinricManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "WeatherManager.h"
#include "WebServerManager.h"

const uint16_t IR_SEND_PIN = 4;
const char* kSettingsPath = "/cfg/settings.json";

IRSamsungAc ac(IR_SEND_PIN);
EventLog eventLog;
AppSettings settings;
TimeManager timeManager(eventLog);
AcController acController(ac, eventLog, settings);
AutomationEngine automation(acController, timeManager, eventLog, settings);
StatsManager stats(acController, timeManager, eventLog, settings);
SinricManager sinric(acController, eventLog);
HomeKitManager homekit(acController, eventLog);
WeatherManager weather(eventLog);
WebServerManager webServer(acController, automation, stats, settings, timeManager, eventLog);

// Wi-Fi and mDNS are owned by HomeSpan (HomeKitManager) — it connects with
// the credentials from secrets.h, retries with backoff forever, and starts
// the mDNS responder as "ac-controller". This hook runs once, right after
// that first connect, to layer on everything else that needs the network:
//  - the HTTP service record (http://ac-controller.local/) on HomeSpan's
//    mDNS instance, plus NetBIOS for Windows (http://ac-controller/)
//  - NTP sync
//  - the Sinric Pro cloud bridge
void onNetworkUp() {
  MDNS.addService("http", "tcp", 80);
  NBNS.begin("ac-controller");
  Serial.println("Name services ready: http://ac-controller.local/ and http://ac-controller/");

  timeManager.onWifiConnected();
  sinric.begin();
}

// Mount LittleFS, self-healing a bad volume instead of crashing on it.
//
// LittleFS.begin(true) formats on a mount *failure*, but on a freshly erased
// flash it can report success while leaving a zero-capacity volume (block
// count 0). The first write then divides by zero deep inside littlefs and
// panics the CPU before setup() finishes — an unrecoverable boot loop. So we
// treat a zero-byte mount as failure and force one clean reformat; if even
// that fails we run without persistence rather than brick the device.
bool mountFileSystem() {
  bool mounted = LittleFS.begin(true);
  if (mounted && LittleFS.totalBytes() == 0) {
    Serial.println("LittleFS mounted empty — reformatting...");
    LittleFS.end();
    mounted = LittleFS.format() && LittleFS.begin(false);
  }
  if (mounted) {
    Serial.printf("LittleFS ready: %u KB total, %u KB used\n",
                  (unsigned)(LittleFS.totalBytes() / 1024),
                  (unsigned)(LittleFS.usedBytes() / 1024));
  } else {
    Serial.println("LittleFS unavailable — configs will not persist this boot.");
  }
  return mounted;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 AC Controller - Starting");

  ac.begin();

  // Only touch the filesystem once we know it's genuinely usable; a failed
  // mount leaves every ConfigStore load/save to no-op safely.
  if (mountFileSystem()) {
    ConfigStore::begin();
    JsonDocument doc;
    if (ConfigStore::load(kSettingsPath, doc)) {
      settings.fromJson(doc.as<JsonObjectConst>());
    }
  }

  acController.begin();
  automation.begin();
  automation.setWeatherManager(&weather);
  weather.setLocation(settings.weatherLat, settings.weatherLon);
  stats.begin();

  // External (manual/cloud/HomeKit/timer/safety) commands cancel a running
  // program; every change is mirrored to both cloud bridges so the Alexa/
  // Google and Apple Home apps stay in sync (each skips its own echo).
  acController.addChangeCallback([](const ACState& s, CmdSource source) {
    automation.onExternalCommand(source);
    sinric.pushState(s, source);
    homekit.pushState(s, source);
  });

  homekit.setWeatherManager(&weather);
  homekit.onWifiReady(onNetworkUp);
  homekit.begin();  // brings up Wi-Fi, mDNS and the HomeKit accessory
  webServer.begin();

  eventLog.add(CmdSource::SYSTEM, "boot complete");
}

void loop() {
  homekit.loop();  // HomeSpan poll: Wi-Fi management + HAP requests

  timeManager.loop();
  acController.loop();
  automation.loop();
  stats.loop();
  sinric.loop();
  webServer.loop();

  // Cheap enough to re-set every tick, so a settings-page location edit
  // takes effect without extra plumbing.
  weather.setLocation(settings.weatherLat, settings.weatherLon);
  weather.loop(WiFi.status() == WL_CONNECTED);

  delay(10);
}
