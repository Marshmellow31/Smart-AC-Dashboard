#include "FirebaseManager.h"

#include "AutomationEngine.h"
#include "EventLog.h"
#include "StatsManager.h"
#include "WebServerManager.h"
#include "secrets.h"

// Provide tokens generation process info.
#include <addons/TokenHelper.h>

namespace {

constexpr const char* kCtlPath = "/ctl";
constexpr const char* kAcStatePath = "/ctl/acState";
constexpr const char* kCmdPath = "/ctl/cmd";
constexpr const char* kHeartbeatPath = "/state/heartbeat";

const char* mirrorNodeName(uint8_t node) {
  switch (node) {
    case 0: return "status";
    case 1: return "settings";
    case 2: return "schedules";
    case 3: return "programs";
    case 4: return "presets";
    case 5: return "timers";
    case 6: return "stats";
    case 7: return "log";
  }
  return "?";
}

}  // namespace

FirebaseManager* FirebaseManager::instance_ = nullptr;

FirebaseManager::FirebaseManager(AcController& acController, AutomationEngine& engine,
                                 StatsManager& stats, WebServerManager& webServer,
                                 AppSettings& settings, EventLog& log)
    : acController_(acController),
      engine_(engine),
      stats_(stats),
      webServer_(webServer),
      settings_(settings),
      log_(log) {
  instance_ = this;
}

void FirebaseManager::begin() {
  // Called on every Wi-Fi (re)connect — must not re-init Firebase or stack a
  // second stream task; the library reconnects on its own.
  if (began_) return;
  began_ = true;

  if (String(FIREBASE_API_KEY).length() == 0 ||
      String(FIREBASE_API_KEY) == "YOUR_FIREBASE_API_KEY") {
    Serial.println("[Firebase] Missing credentials. Firebase sync disabled.");
    return;
  }
  enabled_ = true;

  Serial.println("[Firebase] Initializing...");

  config_.api_key = FIREBASE_API_KEY;
  config_.database_url = FIREBASE_DATABASE_URL;

  auth_.user.email = FIREBASE_USER_EMAIL;
  auth_.user.password = FIREBASE_USER_PASSWORD;

  // Assign the callback function for the long running token generation task
  config_.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config_, &auth_);
  Firebase.reconnectWiFi(true);
}

void FirebaseManager::loop() {
  if (!enabled_) return;

  startStreamIfReady();
  drainStreamEvents();

  if (!Firebase.ready() || !streamStarted_) return;

  mirrorTick();

  unsigned long now = millis();
  if (lastHeartbeatMs_ == 0 || now - lastHeartbeatMs_ >= kHeartbeatIntervalMs) {
    lastHeartbeatMs_ = now;
    Firebase.RTDB.setInt(&fbdo_, kHeartbeatPath, static_cast<int>(time(nullptr)));
  }
}

// ---------------------------------------------------------------------------
// Connect: device state is authoritative — seed the cloud from it and drop
// commands queued while we were offline, THEN start streaming. The initial
// stream snapshot then matches our state, so nothing is re-applied and no IR
// is sent on boot.

void FirebaseManager::startStreamIfReady() {
  if (streamStarted_ || !Firebase.ready()) return;

  unsigned long now = millis();
  if (lastStreamAttemptMs_ != 0 && now - lastStreamAttemptMs_ < kStreamRetryMs) return;
  lastStreamAttemptMs_ = now;

  ACState s = acController_.state();
  FirebaseJson json;
  json.set("power", s.power);
  json.set("temp", s.temp);
  json.set("mode", acModeToString(s.mode));
  json.set("fan", fanSpeedToString(s.fan));
  if (!Firebase.RTDB.setJSON(&fbdo_, kAcStatePath, &json)) {
    Serial.printf("[Firebase] acState seed failed: %s\n", fbdo_.errorReason().c_str());
    return;  // retry next pass
  }

  // Deleting a missing node is a no-op, so the return value doesn't matter.
  Firebase.RTDB.deleteNode(&fbdo_, kCmdPath);

  if (!Firebase.RTDB.beginStream(&stream_, kCtlPath)) {
    Serial.printf("[Firebase] stream begin error: %s\n", stream_.errorReason().c_str());
    return;
  }
  Firebase.RTDB.setStreamCallback(&stream_, streamCallback, streamTimeoutCallback);
  streamStarted_ = true;
  log_.add(CmdSource::SYSTEM, "Firebase connected");
}

// ---------------------------------------------------------------------------
// Stream — callback runs on the Firebase task: queue only, no work here.

void FirebaseManager::streamCallback(FirebaseStream data) {
  if (!instance_) return;
  std::lock_guard<std::mutex> lock(instance_->queueMutex_);
  if (instance_->queue_.size() >= kMaxQueuedEvents) return;
  StreamEvent ev;
  ev.path = data.dataPath();
  ev.type = data.dataType();
  ev.data = data.stringData();
  instance_->queue_.push_back(ev);
}

void FirebaseManager::streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[Firebase] Stream timeout, reconnecting...");
  }
}

void FirebaseManager::drainStreamEvents() {
  std::vector<StreamEvent> events;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    events.swap(queue_);
  }
  for (const auto& ev : events) processEvent(ev);
}

void FirebaseManager::processEvent(const StreamEvent& ev) {
  if (ev.type == "null") return;  // node deletions (incl. our own cmd cleanup)

  if (ev.path == "/") {
    // Full /ctl snapshot (stream (re)connect). acState was just seeded from
    // the device so this is normally a no-op; any cmd children slipped in
    // after the stale-command purge, so they are fresh — run them.
    JsonDocument doc;
    if (deserializeJson(doc, ev.data)) return;
    applyAcState(doc["acState"]);
    for (JsonPairConst kv : doc["cmd"].as<JsonObjectConst>()) {
      executeCommand(String(kv.key().c_str()), kv.value());
    }
    return;
  }

  if (ev.path == "/acState") {
    JsonDocument doc;
    if (deserializeJson(doc, ev.data)) return;
    applyAcState(doc.as<JsonVariantConst>());
    return;
  }

  if (ev.path.startsWith("/acState/")) {
    applyAcStateField(ev.path.substring(strlen("/acState/")), ev.data);
    return;
  }

  if (ev.path.startsWith("/cmd/")) {
    String key = ev.path.substring(strlen("/cmd/"));
    if (key.indexOf('/') >= 0) return;  // partial write into a command — ignore
    JsonDocument doc;
    if (deserializeJson(doc, ev.data)) return;
    executeCommand(key, doc.as<JsonVariantConst>());
    return;
  }
}

// ---------------------------------------------------------------------------
// /ctl/acState → AC command (diff against current state; no-ops are dropped
// again in AcController for safety).

void FirebaseManager::applyAcState(JsonVariantConst obj) {
  if (obj.isNull()) return;
  ACState cur = acController_.state();
  AcCommand cmd;

  if (obj["power"].is<bool>() && obj["power"].as<bool>() != cur.power) {
    cmd.hasPower = true;
    cmd.power = obj["power"].as<bool>();
  }
  if (obj["temp"].is<int>()) {
    int t = constrain(obj["temp"].as<int>(), (int)kAcMinTemp, (int)kAcMaxTemp);
    if (t != cur.temp) {
      cmd.hasTemp = true;
      cmd.temp = static_cast<uint8_t>(t);
    }
  }
  if (obj["mode"].is<const char*>()) {
    bool ok = false;
    AcMode m = acModeFromString(obj["mode"].as<String>(), ok);
    if (ok && m != cur.mode) {
      cmd.hasMode = true;
      cmd.mode = m;
    }
  }
  if (obj["fan"].is<const char*>()) {
    bool ok = false;
    FanSpeed f = fanSpeedFromString(obj["fan"].as<String>(), ok);
    if (ok && f != cur.fan) {
      cmd.hasFan = true;
      cmd.fan = f;
    }
  }

  if (cmd.hasPower || cmd.hasTemp || cmd.hasMode || cmd.hasFan) {
    acController_.apply(cmd, CmdSource::CLOUD, "firebase");
  }
}

void FirebaseManager::applyAcStateField(const String& field, const String& value) {
  ACState cur = acController_.state();
  AcCommand cmd;

  if (field == "power") {
    bool p = (value == "true" || value == "1");
    if (p == cur.power) return;
    cmd.hasPower = true;
    cmd.power = p;
  } else if (field == "temp") {
    int t = constrain((int)value.toInt(), (int)kAcMinTemp, (int)kAcMaxTemp);
    if (t == cur.temp) return;
    cmd.hasTemp = true;
    cmd.temp = static_cast<uint8_t>(t);
  } else if (field == "mode") {
    bool ok = false;
    AcMode m = acModeFromString(value, ok);
    if (!ok || m == cur.mode) return;
    cmd.hasMode = true;
    cmd.mode = m;
  } else if (field == "fan") {
    bool ok = false;
    FanSpeed f = fanSpeedFromString(value, ok);
    if (!ok || f == cur.fan) return;
    cmd.hasFan = true;
    cmd.fan = f;
  } else {
    return;
  }

  acController_.apply(cmd, CmdSource::CLOUD, "firebase");
}

// ---------------------------------------------------------------------------
// /ctl/cmd RPCs — same operations the REST API offers.

void FirebaseManager::executeCommand(const String& key, JsonVariantConst doc) {
  const char* type = doc["type"] | "";
  if (type[0] == '\0') return;

  String err;
  bool ok = true;

  if (strcmp(type, "settings") == 0) {
    settings_.fromJson(doc["settings"].as<JsonObjectConst>());
    webServer_.requestSettingsSave();
    log_.add(CmdSource::CLOUD, "settings updated");
    forceMirror(MIRROR_SETTINGS);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "schedules") == 0) {
    ok = engine_.schedulesFromJson(doc.as<JsonObjectConst>(), err);
    forceMirror(MIRROR_SCHEDULES);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "programs") == 0) {
    ok = engine_.programsFromJson(doc.as<JsonObjectConst>(), err);
    forceMirror(MIRROR_PROGRAMS);
  } else if (strcmp(type, "presets") == 0) {
    ok = webServer_.setPresetsJson(doc.as<JsonObjectConst>(), err);
    forceMirror(MIRROR_PRESETS);
  } else if (strcmp(type, "applyPreset") == 0) {
    ok = webServer_.applyPreset(doc["name"] | String(""), CmdSource::CLOUD, err);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "addTimer") == 0) {
    AcCommand action;
    ok = acCommandFromJson(doc["action"].as<JsonObjectConst>(), action, err);
    if (ok) {
      int minutes = doc["minutes"] | 0;
      ok = engine_.addTimer(static_cast<uint16_t>(constrain(minutes, 0, 65535)),
                            action, err);
    }
    forceMirror(MIRROR_TIMERS);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "cancelTimer") == 0) {
    engine_.cancelTimer(doc["id"] | -1);
    forceMirror(MIRROR_TIMERS);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "startProgram") == 0) {
    ok = engine_.startProgram(doc["id"] | String(""), doc["endTime"] | String(""), err);
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "stopProgram") == 0) {
    engine_.stopProgram("stopped from cloud");
    forceMirror(MIRROR_STATUS);
  } else if (strcmp(type, "filterReset") == 0) {
    stats_.resetFilter();
    forceMirror(MIRROR_STATS);
  } else if (strcmp(type, "clearOverride") == 0) {
    acController_.clearOverride();
    log_.add(CmdSource::CLOUD, "manual hold cleared from cloud");
    forceMirror(MIRROR_STATUS);
  } else {
    log_.add(CmdSource::CLOUD, "unknown cloud cmd '%s'", type);
  }

  if (!ok) {
    log_.add(CmdSource::CLOUD, "cloud cmd '%s' failed: %s", type, err.c_str());
  }
  forceMirror(MIRROR_LOG);

  String path = String(kCmdPath) + "/" + key;
  Firebase.RTDB.deleteNode(&fbdo_, path.c_str());
}

// ---------------------------------------------------------------------------
// Device → cloud

void FirebaseManager::pushState(const ACState& state, CmdSource source) {
  // CLOUD-sourced changes are echoed back on purpose: it confirms the state
  // the device actually applied (e.g. clamped temps, cloud preset applies).
  // No loop risk — applyAcState only acts on diffs, so the echo terminates.
  (void)source;
  if (!enabled_ || !Firebase.ready()) return;

  FirebaseJson json;
  json.set("power", state.power);
  json.set("temp", state.temp);
  json.set("mode", acModeToString(state.mode));
  json.set("fan", fanSpeedToString(state.fan));

  if (!Firebase.RTDB.updateNode(&fbdo_, kAcStatePath, &json)) {
    Serial.printf("[Firebase] Push failed: %s\n", fbdo_.errorReason().c_str());
  }
  forceMirror(MIRROR_STATUS);
  forceMirror(MIRROR_LOG);
}

// Mirrors are only written when their serialized content changed, so steady
// state costs nothing; forced nodes flush on the next loop pass, the rest
// round-robin (one node per interval keeps the blocking write bounded).
void FirebaseManager::mirrorTick() {
  for (uint8_t i = 0; i < MIRROR_COUNT; ++i) {
    if (forced_[i]) {
      forced_[i] = false;
      writeMirror(static_cast<MirrorNode>(i));
    }
  }

  unsigned long now = millis();
  if (now - lastMirrorMs_ < kMirrorIntervalMs) return;
  lastMirrorMs_ = now;

  writeMirror(static_cast<MirrorNode>(mirrorIdx_));
  mirrorIdx_ = (mirrorIdx_ + 1) % MIRROR_COUNT;
}

bool FirebaseManager::writeMirror(MirrorNode node) {
  String payload;
  buildMirrorJson(node, payload);
  if (payload == lastSent_[node]) return true;

  FirebaseJson json;
  json.setJsonData(payload);
  String path = String("/state/") + mirrorNodeName(node);
  if (!Firebase.RTDB.setJSON(&fbdo_, path.c_str(), &json)) {
    Serial.printf("[Firebase] mirror %s failed: %s\n", mirrorNodeName(node),
                  fbdo_.errorReason().c_str());
    return false;
  }
  lastSent_[node] = payload;
  return true;
}

void FirebaseManager::buildMirrorJson(MirrorNode node, String& out) {
  JsonDocument doc;
  switch (node) {
    case MIRROR_STATUS:
      webServer_.statusJson(doc);
      // Changes every second and the web has its own clock — keeping it would
      // turn every mirror pass into a write.
      doc.remove("epoch");
      break;
    case MIRROR_SETTINGS:
      settings_.toJson(doc.to<JsonObject>());
      break;
    case MIRROR_SCHEDULES:
      engine_.schedulesToJson(doc);
      break;
    case MIRROR_PROGRAMS:
      engine_.programsToJson(doc);
      break;
    case MIRROR_PRESETS:
      webServer_.presetsJson(doc);
      break;
    case MIRROR_TIMERS:
      engine_.timersToJson(doc);
      // Same reason as epoch: the web derives remaining time from fireAt.
      for (JsonObject t : doc["timers"].as<JsonArray>()) t.remove("remainingSec");
      break;
    case MIRROR_STATS:
      stats_.toJson(doc.to<JsonObject>());
      break;
    case MIRROR_LOG:
      log_.toJson(doc["events"].to<JsonArray>());
      break;
    default:
      break;
  }
  serializeJson(doc, out);
}
