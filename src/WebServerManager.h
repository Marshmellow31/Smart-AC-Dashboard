#pragma once

#include <ESPAsyncWebServer.h>

#include <mutex>
#include <vector>

#include "AcCommand.h"
#include "AppSettings.h"

class AcController;
class AutomationEngine;
class StatsManager;
class TimeManager;
class EventLog;

// REST API + static frontend. All AC changes go through AcController (which
// defers the IR send to the main loop); automation config goes through
// AutomationEngine's thread-safe JSON methods. Handlers run in the async TCP
// task — they never write to LittleFS directly, only raise dirty flags that
// loop() persists.
//
// Also owns the presets ("scenes"): named one-tap states like Night/Turbo/Eco.
class WebServerManager {
 public:
  WebServerManager(AcController& controller, AutomationEngine& engine,
                   StatsManager& stats, AppSettings& settings,
                   TimeManager& time, EventLog& log, uint16_t port = 80);

  void begin();
  void loop();  // deferred persistence of settings/presets

 private:
  struct Preset {
    char name[24];
    AcCommand action;
  };

  void setupControlRoutes();
  void setupAutomationRoutes();
  void setupPresetRoutes();
  void setupMiscRoutes();
  void setupStaticRoutes();

  void loadPresets();
  void seedDefaultPresets();
  void presetsToJson(JsonDocument& doc) const;

  void sendStatus(AsyncWebServerRequest* request);
  void sendJson(AsyncWebServerRequest* request, const JsonDocument& doc, uint16_t code = 200);
  void sendError(AsyncWebServerRequest* request, uint16_t code, const String& message);
  void sendOk(AsyncWebServerRequest* request);

  AcController& controller_;
  AutomationEngine& engine_;
  StatsManager& stats_;
  AppSettings& settings_;
  TimeManager& time_;
  EventLog& log_;
  AsyncWebServer server_;

  mutable std::mutex presetsMutex_;
  std::vector<Preset> presets_;
  volatile bool settingsDirty_ = false;
  volatile bool presetsDirty_ = false;

  static constexpr size_t kMaxPresets = 10;
};
