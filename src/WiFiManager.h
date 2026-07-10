#pragma once

#include <Arduino.h>

// Owns Wi-Fi STA connection + auto-reconnect. Call begin() once from setup()
// and loop() on every main loop iteration.
class WiFiManager {
 public:
  void begin();
  void loop();
  bool isConnected() const;

 private:
  void connect();

  unsigned long lastReconnectAttempt_ = 0;
  bool wasConnected_ = false;
  static constexpr unsigned long kReconnectIntervalMs = 5000;
};
