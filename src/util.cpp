// ============================================================================
// util.cpp - minimal JSON field extraction for our own API payloads
// ----------------------------------------------------------------------------
// These are NOT general-purpose JSON parsers - they only handle the flat,
// well-formed bodies this firmware itself produces/consumes. Values must not
// contain escaped quotes (we escape nothing server-side except via sanitize).
// ============================================================================
#include "util.h"

static int findKey(const String& json, const char* key) {
    String pat = "\"" + String(key) + "\"";
    int i = json.indexOf(pat);
    if (i < 0) return -1;
    i = json.indexOf(':', i + pat.length());
    if (i < 0) return -1;
    // skip whitespace
    while ((size_t)++i < json.length() && isspace(json[i])) {}
    return i;
}

bool extractJsonStr(const String& json, const char* key, String& out) {
    int i = findKey(json, key);
    if (i < 0 || json[i] != '"') return false;
    int end = json.indexOf('"', i + 1);
    if (end < 0) return false;
    out = json.substring(i + 1, end);
    return true;
}

bool extractJsonArr(const String& json, const char* key, std::vector<String>& out) {
    out.clear();
    int i = findKey(json, key);
    if (i < 0 || json[i] != '[') return false;
    int end = json.indexOf(']', i);
    if (end < 0) return false;
    String body = json.substring(i + 1, end);
    int start = 0;
    while (start <= (int)body.length()) {
        int q1 = body.indexOf('"', start);
        if (q1 < 0) break;
        int q2 = body.indexOf('"', q1 + 1);
        if (q2 < 0) break;
        out.push_back(body.substring(q1 + 1, q2));
        start = q2 + 1;
    }
    return true;
}
