// ============================================================================
// util.h - tiny hand-rolled JSON helpers (avoids ArduinoJson on hot paths)
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

// Extract "key":"value" string field from a flat JSON object.
bool extractJsonStr(const String& json, const char* key, String& out);

// Extract "key":["a","b"] array-of-strings from a flat JSON object.
bool extractJsonArr(const String& json, const char* key, std::vector<String>& out);

// Convert CRLF/CR line endings to plain LF (cross-platform text hygiene).
String normalizeEol(const String& s);
