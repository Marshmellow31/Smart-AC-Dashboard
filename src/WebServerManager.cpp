#include "WebServerManager.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>

#include "AcController.h"
#include "AutomationEngine.h"
#include "ConfigStore.h"
#include "EventLog.h"
#include "StatsManager.h"
#include "TimeManager.h"

namespace {
constexpr const char* kPresetsPath = "/cfg/presets.json";
constexpr const char* kSettingsPath = "/cfg/settings.json";
}  // namespace

WebServerManager::WebServerManager(AcController& controller, AutomationEngine& engine,
                                   StatsManager& stats, AppSettings& settings,
                                   TimeManager& time, EventLog& log, uint16_t port)
    : controller_(controller),
      engine_(engine),
      stats_(stats),
      settings_(settings),
      time_(time),
      log_(log),
      server_(port) {}

void WebServerManager::begin() {
  loadPresets();
  setupControlRoutes();
  setupAutomationRoutes();
  setupPresetRoutes();
  setupMiscRoutes();
  setupStaticRoutes();
  server_.begin();
  Serial.println("Web server started.");
}

void WebServerManager::loop() {
  if (settingsDirty_) {
    settingsDirty_ = false;
    JsonDocument doc;
    settings_.toJson(doc.to<JsonObject>());
    ConfigStore::save(kSettingsPath, doc);
  }
  if (presetsDirty_) {
    presetsDirty_ = false;
    JsonDocument doc;
    presetsToJson(doc);
    ConfigStore::save(kPresetsPath, doc);
  }
}

// ---------------------------------------------------------------------------
// Response helpers

void WebServerManager::sendJson(AsyncWebServerRequest* request,
                                const JsonDocument& doc, uint16_t code) {
  String out;
  serializeJson(doc, out);
  request->send(code, "application/json", out);
}

void WebServerManager::sendError(AsyncWebServerRequest* request, uint16_t code,
                                 const String& message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(request, doc, code);
}

void WebServerManager::sendOk(AsyncWebServerRequest* request) {
  request->send(200, "application/json", "{\"ok\":true}");
}

void WebServerManager::sendStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  statusJson(doc);
  sendJson(request, doc);
}

void WebServerManager::statusJson(JsonDocument& doc) const {
  ACState s = controller_.state();
  doc["power"] = s.power;
  doc["mode"] = acModeToString(s.mode);
  doc["temp"] = s.temp;
  doc["fan"] = fanSpeedToString(s.fan);

  bool timeValid = time_.isTimeValid();
  doc["timeValid"] = timeValid;
  if (timeValid) {
    doc["time"] = time_.format(time_.now());
    doc["epoch"] = static_cast<uint32_t>(time_.now());
  }

  JsonObject ov = doc["override"].to<JsonObject>();
  bool ovActive = controller_.overrideActive();
  ov["active"] = ovActive;
  if (ovActive) ov["until"] = static_cast<uint32_t>(controller_.overrideUntil());

  doc["automationEnabled"] = settings_.automationEnabled;
  engine_.statusToJson(doc.as<JsonObject>());
}

// ---------------------------------------------------------------------------
// Manual control (existing endpoints, now routed through AcController)

void WebServerManager::setupControlRoutes() {
  server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendStatus(request);
  });

  auto* powerHandler = new AsyncCallbackJsonWebHandler(
      "/api/power", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        if (!body["on"].is<bool>()) {
          sendError(request, 400, "Expected JSON body: {\"on\": true|false}");
          return;
        }
        AcCommand cmd;
        cmd.hasPower = true;
        cmd.power = body["on"].as<bool>();
        controller_.apply(cmd, CmdSource::MANUAL, "web UI");
        sendStatus(request);
      });
  server_.addHandler(powerHandler);

  auto* tempHandler = new AsyncCallbackJsonWebHandler(
      "/api/temp", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        if (!body["value"].is<int>()) {
          sendError(request, 400, "Expected JSON body: {\"value\": 16-30}");
          return;
        }
        int value = body["value"].as<int>();
        if (value < kAcMinTemp || value > kAcMaxTemp) {
          sendError(request, 400, "Temperature must be between 16 and 30");
          return;
        }
        AcCommand cmd;
        cmd.hasTemp = true;
        cmd.temp = static_cast<uint8_t>(value);
        controller_.apply(cmd, CmdSource::MANUAL, "web UI");
        sendStatus(request);
      });
  server_.addHandler(tempHandler);

  auto* modeHandler = new AsyncCallbackJsonWebHandler(
      "/api/mode", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        if (!body["mode"].is<const char*>()) {
          sendError(request, 400, "Expected JSON body: {\"mode\": \"cool|dry|fan|auto|heat\"}");
          return;
        }
        bool ok = false;
        AcMode mode = acModeFromString(body["mode"].as<String>(), ok);
        if (!ok) {
          sendError(request, 400, "Unknown mode. Use cool, dry, fan, auto, or heat");
          return;
        }
        AcCommand cmd;
        cmd.hasMode = true;
        cmd.mode = mode;
        controller_.apply(cmd, CmdSource::MANUAL, "web UI");
        sendStatus(request);
      });
  server_.addHandler(modeHandler);

  auto* fanHandler = new AsyncCallbackJsonWebHandler(
      "/api/fan", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        if (!body["speed"].is<const char*>()) {
          sendError(request, 400, "Expected JSON body: {\"speed\": \"auto|low|medium|high\"}");
          return;
        }
        bool ok = false;
        FanSpeed fan = fanSpeedFromString(body["speed"].as<String>(), ok);
        if (!ok) {
          sendError(request, 400, "Unknown fan speed. Use auto, low, medium, or high");
          return;
        }
        AcCommand cmd;
        cmd.hasFan = true;
        cmd.fan = fan;
        controller_.apply(cmd, CmdSource::MANUAL, "web UI");
        sendStatus(request);
      });
  server_.addHandler(fanHandler);
}

// ---------------------------------------------------------------------------
// Automation: schedules, timers, programs

void WebServerManager::setupAutomationRoutes() {
  server_.on("/api/schedules", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    engine_.schedulesToJson(doc);
    sendJson(request, doc);
  });

  auto* schedulesHandler = new AsyncCallbackJsonWebHandler(
      "/api/schedules", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        String err;
        if (!engine_.schedulesFromJson(json.as<JsonObjectConst>(), err)) {
          sendError(request, 400, err);
          return;
        }
        JsonDocument doc;
        engine_.schedulesToJson(doc);
        sendJson(request, doc);
      });
  server_.addHandler(schedulesHandler);

  // Register /api/timers/cancel before /api/timers: AsyncCallbackJsonWebHandler
  // also matches sub-paths of its URI, and the first registered handler wins.
  auto* timerCancelHandler = new AsyncCallbackJsonWebHandler(
      "/api/timers/cancel", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        int id = body["id"] | -1;
        engine_.cancelTimer(id);
        JsonDocument doc;
        engine_.timersToJson(doc);
        sendJson(request, doc);
      });
  server_.addHandler(timerCancelHandler);

  server_.on("/api/timers", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    engine_.timersToJson(doc);
    sendJson(request, doc);
  });

  auto* timerHandler = new AsyncCallbackJsonWebHandler(
      "/api/timers", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        int minutes = body["minutes"] | 0;
        AcCommand action;
        String err;
        if (!acCommandFromJson(body["action"].as<JsonObjectConst>(), action, err)) {
          sendError(request, 400, "action: " + err);
          return;
        }
        if (!engine_.addTimer(static_cast<uint16_t>(constrain(minutes, 0, 65535)),
                              action, err)) {
          sendError(request, 400, err);
          return;
        }
        JsonDocument doc;
        engine_.timersToJson(doc);
        sendJson(request, doc);
      });
  server_.addHandler(timerHandler);

  server_.on("/api/programs", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    engine_.programsToJson(doc);
    sendJson(request, doc);
  });

  auto* programsHandler = new AsyncCallbackJsonWebHandler(
      "/api/programs", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        String err;
        if (!engine_.programsFromJson(json.as<JsonObjectConst>(), err)) {
          sendError(request, 400, err);
          return;
        }
        JsonDocument doc;
        engine_.programsToJson(doc);
        sendJson(request, doc);
      });
  server_.addHandler(programsHandler);

  auto* programStartHandler = new AsyncCallbackJsonWebHandler(
      "/api/program/start", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        String id = body["id"] | "";
        String endTime = body["endTime"] | "";
        String err;
        if (!engine_.startProgram(id, endTime, err)) {
          sendError(request, 400, err);
          return;
        }
        sendStatus(request);
      });
  server_.addHandler(programStartHandler);

  server_.on("/api/program/stop", HTTP_POST, [this](AsyncWebServerRequest* request) {
    engine_.stopProgram("stopped from web UI");
    sendStatus(request);
  });
}

// ---------------------------------------------------------------------------
// Presets (scenes)

void WebServerManager::loadPresets() {
  JsonDocument doc;
  if (ConfigStore::load(kPresetsPath, doc)) {
    std::lock_guard<std::mutex> lock(presetsMutex_);
    for (JsonObjectConst o : doc["presets"].as<JsonArrayConst>()) {
      if (presets_.size() >= kMaxPresets) break;
      Preset p;
      strlcpy(p.name, o["name"] | "", sizeof(p.name));
      String err;
      if (p.name[0] == '\0') continue;
      if (!acCommandFromJson(o["action"].as<JsonObjectConst>(), p.action, err)) continue;
      presets_.push_back(p);
    }
  } else {
    seedDefaultPresets();
    presetsDirty_ = true;
  }
}

void WebServerManager::seedDefaultPresets() {
  auto make = [](const char* name, uint8_t temp, FanSpeed fan) {
    Preset p;
    strlcpy(p.name, name, sizeof(p.name));
    p.action.hasPower = true;
    p.action.power = true;
    p.action.hasMode = true;
    p.action.mode = AcMode::COOL;
    p.action.hasTemp = true;
    p.action.temp = temp;
    p.action.hasFan = true;
    p.action.fan = fan;
    return p;
  };
  std::lock_guard<std::mutex> lock(presetsMutex_);
  presets_.push_back(make("Night", 26, FanSpeed::FAN_LOW));
  presets_.push_back(make("Turbo", 18, FanSpeed::FAN_HIGH));
  presets_.push_back(make("Eco", 26, FanSpeed::FAN_AUTO));
}

void WebServerManager::presetsToJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(presetsMutex_);
  JsonArray arr = doc["presets"].to<JsonArray>();
  for (const auto& p : presets_) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = p.name;
    acCommandToJson(p.action, o["action"].to<JsonObject>());
  }
}

bool WebServerManager::applyPreset(const String& name, CmdSource source, String& err) {
  AcCommand action;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(presetsMutex_);
    for (const auto& p : presets_) {
      if (name == p.name) {
        action = p.action;
        found = true;
        break;
      }
    }
  }
  if (!found) {
    err = "Unknown preset";
    return false;
  }
  char reason[40];
  snprintf(reason, sizeof(reason), "preset '%s'", name.c_str());
  controller_.apply(action, source, reason);
  return true;
}

bool WebServerManager::setPresetsJson(JsonObjectConst root, String& err) {
  JsonArrayConst arr = root["presets"].as<JsonArrayConst>();
  if (arr.isNull()) {
    err = "expected {\"presets\": [...]}";
    return false;
  }
  if (arr.size() > kMaxPresets) {
    err = "too many presets (max 10)";
    return false;
  }
  std::vector<Preset> parsed;
  for (JsonObjectConst o : arr) {
    Preset p;
    strlcpy(p.name, o["name"] | "", sizeof(p.name));
    if (p.name[0] == '\0') {
      err = "every preset needs a name";
      return false;
    }
    String cmdErr;
    if (!acCommandFromJson(o["action"].as<JsonObjectConst>(), p.action, cmdErr)) {
      err = "action: " + cmdErr;
      return false;
    }
    parsed.push_back(p);
  }
  {
    std::lock_guard<std::mutex> lock(presetsMutex_);
    presets_ = std::move(parsed);
  }
  presetsDirty_ = true;
  return true;
}

void WebServerManager::setupPresetRoutes() {
  // /apply registered before the bare URI (sub-path matching, see timers).
  auto* applyHandler = new AsyncCallbackJsonWebHandler(
      "/api/presets/apply", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject body = json.as<JsonObject>();
        String name = body["name"] | "";
        String err;
        if (!applyPreset(name, CmdSource::MANUAL, err)) {
          sendError(request, 404, err);
          return;
        }
        sendStatus(request);
      });
  server_.addHandler(applyHandler);

  server_.on("/api/presets", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    presetsToJson(doc);
    sendJson(request, doc);
  });

  auto* presetsHandler = new AsyncCallbackJsonWebHandler(
      "/api/presets", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        String err;
        if (!setPresetsJson(json.as<JsonObjectConst>(), err)) {
          sendError(request, 400, err);
          return;
        }
        JsonDocument doc;
        presetsToJson(doc);
        sendJson(request, doc);
      });
  server_.addHandler(presetsHandler);
}

// ---------------------------------------------------------------------------
// Settings, stats, log

void WebServerManager::setupMiscRoutes() {
  server_.on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    settings_.toJson(doc.to<JsonObject>());
    sendJson(request, doc);
  });

  auto* settingsHandler = new AsyncCallbackJsonWebHandler(
      "/api/settings", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        settings_.fromJson(json.as<JsonObjectConst>());
        settingsDirty_ = true;
        log_.add(CmdSource::SYSTEM, "settings updated");
        JsonDocument doc;
        settings_.toJson(doc.to<JsonObject>());
        sendJson(request, doc);
      });
  server_.addHandler(settingsHandler);

  server_.on("/api/stats", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    stats_.toJson(doc.to<JsonObject>());
    sendJson(request, doc);
  });

  server_.on("/api/filter/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
    stats_.resetFilter();
    sendOk(request);
  });

  server_.on("/api/log", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    log_.toJson(doc["events"].to<JsonArray>());
    sendJson(request, doc);
  });

  server_.on("/api/override/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
    controller_.clearOverride();
    log_.add(CmdSource::SYSTEM, "manual hold cleared from web UI");
    sendStatus(request);
  });
}

void WebServerManager::setupStaticRoutes() {
  // Registered before the "/" catch-all so these specific paths win; each
  // prefers its precompressed data/*.gz sibling (built by tools/gzip_data.py)
  // and gets a long cache lifetime since filenames don't change on update.
  server_.serveStatic("/style.css", LittleFS, "/style.css")
      .setTryGzipFirst(true)
      .setCacheControl("max-age=604800");
  server_.serveStatic("/script.js", LittleFS, "/script.js")
      .setTryGzipFirst(true)
      .setCacheControl("max-age=604800");
  server_.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setTryGzipFirst(true)
      .setCacheControl("max-age=600");

  server_.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "application/json", "{\"error\":\"Not found\"}");
  });
}
