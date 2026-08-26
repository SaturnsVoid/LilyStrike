// ============================================================================
// webserver.h - WiFi AP + authenticated web UI + REST API
// ============================================================================
#pragma once
#include <Arduino.h>

namespace web {

bool begin();          // start AP + webserver (unless interface disabled)
void handle();         // call from loop()
void suspend();        // release port 80 (EvilAP takes over)
void resume();         // re-bind port 80
String localIP();

} // namespace web
