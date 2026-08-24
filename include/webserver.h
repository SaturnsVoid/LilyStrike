// ============================================================================
// webserver.h - WiFi AP + authenticated web UI + REST API
// ============================================================================
#pragma once
#include <Arduino.h>

namespace web {

bool begin();          // start AP + webserver (unless interface disabled)
void handle();         // call from loop()
String localIP();

} // namespace web
