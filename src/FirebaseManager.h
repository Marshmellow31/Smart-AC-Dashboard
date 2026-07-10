#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>

#include <mutex>
#include <vector>

#include "AcController.h"

class AutomationEngine;
class StatsManager;
class WebServerManager;
class EventLog;

// Firebase RTDB bridge — makes the Vercel-hosted UI a full peer of the local
// one:
//
//   /ctl/acState   canonical AC state. Web writes control fields here; the
//                  device applies diffs and echoes its actual state back.
//   /ctl/cmd/<id>  RPC queue pushed by the web (timers, schedules, presets,
//                  programs, settings, filter reset, hold clear). Each command
//                  is executed once and deleted. Anything queued while the
//                  device was offline is dropped at (re)connect — stale
//                  commands must never re-blast the AC.
//   /state/<node>  read-only mirrors of the REST API payloads (status,
//                  settings, schedules, programs, presets, timers, stats,
//                  log) + a heartbeat for online detection. Pushed only when
//                  their content changes, one node per pass to bound the time
//                  the (blocking) write can stall the main loop.
//
// Threading: the stream callback runs on the Firebase library's own task, so
// it only queues raw events; all parsing, command execution and RTDB writes
// happen in loop() on the main task.
class FirebaseManager {
 public:
  FirebaseManager(AcController& acController, AutomationEngine& engine,
                  StatsManager& stats, WebServerManager& webServer,
                  AppSettings& settings, EventLog& log);

  void begin();  // idempotent — safe to call on every Wi-Fi (re)connect
  void loop();
  void pushState(const ACState& state, CmdSource source);

 private:
  enum MirrorNode : uint8_t {
    MIRROR_STATUS,
    MIRROR_SETTINGS,
    MIRROR_SCHEDULES,
    MIRROR_PROGRAMS,
    MIRROR_PRESETS,
    MIRROR_TIMERS,
    MIRROR_STATS,
    MIRROR_LOG,
    MIRROR_COUNT,
  };

  struct StreamEvent {
    String path;
    String type;
    String data;
  };

  void startStreamIfReady();
  void drainStreamEvents();
  void processEvent(const StreamEvent& ev);
  void applyAcState(JsonVariantConst obj);
  void applyAcStateField(const String& field, const String& value);
  void executeCommand(const String& key, JsonVariantConst doc);
  void mirrorTick();
  void buildMirrorJson(MirrorNode node, String& out);
  void forceMirror(MirrorNode node) { forced_[node] = true; }
  bool writeMirror(MirrorNode node);

  static void streamCallback(FirebaseStream data);
  static void streamTimeoutCallback(bool timeout);

  AcController& acController_;
  AutomationEngine& engine_;
  StatsManager& stats_;
  WebServerManager& webServer_;
  AppSettings& settings_;
  EventLog& log_;

  FirebaseData fbdo_;    // all writes (main loop only)
  FirebaseData stream_;  // /ctl stream
  FirebaseAuth auth_;
  FirebaseConfig config_;

  bool began_ = false;
  bool enabled_ = false;
  bool streamStarted_ = false;
  unsigned long lastStreamAttemptMs_ = 0;

  std::mutex queueMutex_;
  std::vector<StreamEvent> queue_;

  String lastSent_[MIRROR_COUNT];
  bool forced_[MIRROR_COUNT] = {false};
  uint8_t mirrorIdx_ = 0;
  unsigned long lastMirrorMs_ = 0;
  unsigned long lastHeartbeatMs_ = 0;

  static constexpr size_t kMaxQueuedEvents = 20;
  static constexpr unsigned long kMirrorIntervalMs = 2000;
  static constexpr unsigned long kHeartbeatIntervalMs = 60000;
  static constexpr unsigned long kStreamRetryMs = 5000;

  static FirebaseManager* instance_;
};
