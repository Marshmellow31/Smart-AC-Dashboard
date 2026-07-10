#include "FirebaseManager.h"
#include "secrets.h"
#include "EventLog.h"

// Provide tokens generation process info.
#include <addons/TokenHelper.h>
// Provide RTDB payload printing info and other helper functions.
#include <addons/RTDBHelper.h>

FirebaseManager* FirebaseManager::instance_ = nullptr;

FirebaseManager::FirebaseManager(AcController& acController, EventLog& log) 
    : acController_(acController), log_(log) {
    instance_ = this;
}

void FirebaseManager::begin() {
    if (String(FIREBASE_API_KEY).length() == 0 || String(FIREBASE_API_KEY) == "YOUR_FIREBASE_API_KEY") {
        Serial.println("[Firebase] Missing credentials. Firebase sync disabled.");
        return;
    }

    Serial.println("[Firebase] Initializing...");
    
    config_.api_key = FIREBASE_API_KEY;
    config_.database_url = FIREBASE_DATABASE_URL;
    
    auth_.user.email = FIREBASE_USER_EMAIL;
    auth_.user.password = FIREBASE_USER_PASSWORD;

    // Assign the callback function for the long running token generation task
    config_.token_status_callback = tokenStatusCallback; 

    Firebase.begin(&config_, &auth_);
    Firebase.reconnectWiFi(true);
    
    // Setup stream
    if (!Firebase.RTDB.beginStream(&stream_, "/acState")) {
        Serial.printf("[Firebase] stream begin error: %s\n", stream_.errorReason().c_str());
    }
    
    Firebase.RTDB.setStreamCallback(&stream_, streamCallback, streamTimeoutCallback);
}

void FirebaseManager::loop() {
    // Reconnection is mostly handled automatically by the library
}

void FirebaseManager::pushState(const ACState& state, CmdSource source) {
    if (String(FIREBASE_API_KEY) == "YOUR_FIREBASE_API_KEY") return;
    if (!Firebase.ready()) return;
    
    FirebaseJson json;
    json.set("power", state.power);
    json.set("temp", state.temp);
    json.set("mode", acModeToString(state.mode));
    json.set("fan", fanSpeedToString(state.fan));
    
    if (Firebase.RTDB.updateNode(&fbdo_, "/acState", &json)) {
        // success
    } else {
        Serial.printf("[Firebase] Push failed: %s\n", fbdo_.errorReason().c_str());
    }
}

void FirebaseManager::streamCallback(FirebaseStream data) {
    if (!instance_) return;
    String path = data.dataPath();
    String type = data.dataType();
    String value = data.stringData();
    instance_->handleStreamData(path, type, value);
}

void FirebaseManager::streamTimeoutCallback(bool timeout) {
    if (timeout) {
        Serial.println("[Firebase] Stream timeout, reconnecting...");
    }
}

void FirebaseManager::handleStreamData(const String& path, const String& type, const String& value) {
    // Read current state
    ACState state = acController_.state();
    bool changed = false;
    
    if (type == "json") {
        FirebaseJsonData jsonData;
        FirebaseJson json;
        json.setJsonData(value);
        
        json.get(jsonData, "power");
        if (jsonData.success && state.power != jsonData.boolValue) { state.power = jsonData.boolValue; changed = true; }
        
        json.get(jsonData, "temp");
        if (jsonData.success && state.temp != jsonData.intValue) { state.temp = jsonData.intValue; changed = true; }
        
        json.get(jsonData, "mode");
        if (jsonData.success) {
            bool ok;
            AcMode m = acModeFromString(jsonData.stringValue, ok);
            if (ok && state.mode != m) { state.mode = m; changed = true; }
        }
        
        json.get(jsonData, "fan");
        if (jsonData.success) {
            bool ok;
            FanSpeed f = fanSpeedFromString(jsonData.stringValue, ok);
            if (ok && state.fan != f) { state.fan = f; changed = true; }
        }
    } else {
        if (path == "/power") { 
            bool p = (value == "true" || value == "1"); 
            if (state.power != p) { state.power = p; changed = true; }
        }
        else if (path == "/temp") { 
            int t = value.toInt();
            if (state.temp != t) { state.temp = t; changed = true; }
        }
        else if (path == "/mode") { 
            bool ok;
            AcMode m = acModeFromString(value, ok);
            if (ok && state.mode != m) { state.mode = m; changed = true; }
        }
        else if (path == "/fan") { 
            bool ok;
            FanSpeed f = fanSpeedFromString(value, ok);
            if (ok && state.fan != f) { state.fan = f; changed = true; }
        }
    }
    
    if (changed) {
        AcCommand cmd;
        cmd.power = state.power;
        cmd.temp = state.temp;
        cmd.mode = state.mode;
        cmd.fan = state.fan;
        
        acController_.apply(cmd, CmdSource::CLOUD, "firebase command");
    }
}
