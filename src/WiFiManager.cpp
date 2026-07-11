#include "WiFiManager.h"

#include <WiFi.h>

#include "secrets.h"

void WiFiManager::begin() {
  connect();
}

void WiFiManager::connect() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void WiFiManager::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wasConnected_ = false;
    unsigned long now = millis();
    if (now - lastReconnectAttempt_ > kReconnectIntervalMs) {
      Serial.println("Wi-Fi disconnected. Retrying...");
      // Drop any half-finished association before retrying — calling
      // WiFi.begin() again while the driver is still mid-negotiation (common
      // when the router itself is flapping, e.g. rebooting after a power
      // outage) can wedge the ESP32 Wi-Fi stack instead of cleanly retrying.
      WiFi.disconnect();
      connect();
      lastReconnectAttempt_ = now;
    }
    return;
  }

  if (!wasConnected_) {
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
    wasConnected_ = true;
  }
}

bool WiFiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}
