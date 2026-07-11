#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <NetBIOS.h>
#include <ir_Samsung.h>

#include "AcController.h"
#include "AppSettings.h"
#include "AutomationEngine.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "SinricManager.h"
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
WebServerManager webServer(acController, automation, stats, settings, timeManager, eventLog);

// Two name services so the UI is reachable by name from any client:
//  - mDNS  → http://ac-controller.local/  (Android, iOS, macOS, Linux)
//  - NetBIOS → http://ac-controller/      (Windows, where mDNS is unreliable)
// The raw IP printed on the serial console always works as a fallback.
void startNameServices() {
  MDNS.end();
  if (MDNS.begin(kMdnsHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local/\n", kMdnsHostname);
  } else {
    Serial.println("mDNS responder failed to start.");
  }
  NBNS.begin(kMdnsHostname);
  Serial.printf("NetBIOS responder started: http://%s/\n", kMdnsHostname);
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
  stats.begin();

  // External (manual/cloud/timer/safety) commands cancel a running program;
  // every change is mirrored to Sinric so clouds stay in sync.
  acController.addChangeCallback([](const ACState& s, CmdSource source) {
    automation.onExternalCommand(source);
    sinric.pushState(s, source);
  });

  wifiManager.begin();
  webServer.begin();

  eventLog.add(CmdSource::SYSTEM, "boot complete");
}

void loop() {
  wifiManager.loop();

  static bool wasConnected = false;
  static bool nameServicesStarted = false;
  bool nowConnected = wifiManager.isConnected();
  if (nowConnected && !wasConnected) {
    // mDNS/NetBIOS track the current IP automatically once registered, so
    // this only needs to run on the very first connect. Re-running it on
    // every reconnect (e.g. a router that flaps for a while after a power
    // outage) repeatedly hits ESP32 mDNS's known end()/begin() leak and can
    // starve heap for the web server while Sinric's lighter-weight socket
    // keeps working — exactly the "cloud control works, local UI doesn't"
    // failure mode.
    if (!nameServicesStarted) {
      startNameServices();
      nameServicesStarted = true;
    }
    timeManager.onWifiConnected();
    sinric.begin();
  }
  wasConnected = nowConnected;

  timeManager.loop();
  acController.loop();
  automation.loop();
  stats.loop();
  sinric.loop();
  webServer.loop();

  delay(10);
}
