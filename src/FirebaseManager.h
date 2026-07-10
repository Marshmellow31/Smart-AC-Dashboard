#pragma once

#include <Arduino.h>
#include <Firebase_ESP_Client.h>
#include "AcController.h"

class EventLog;

class FirebaseManager {
public:
    FirebaseManager(AcController& acController, EventLog& log);
    
    void begin();
    void loop();
    void pushState(const ACState& state, CmdSource source);

private:
    AcController& acController_;
    EventLog& log_;

    FirebaseData fbdo_;
    FirebaseAuth auth_;
    FirebaseConfig config_;
    FirebaseData stream_;

    bool connected_ = false;

    static void streamCallback(FirebaseStream data);
    static void streamTimeoutCallback(bool timeout);
    void handleStreamData(const String& path, const String& type, const String& value);
    
    // We need a static pointer for the stream callback to access the instance
    static FirebaseManager* instance_;
};
