// ============================================================================
// util.cpp - minimal JSON field extraction for our own API payloads
// ----------------------------------------------------------------------------
// These are NOT general-purpose JSON parsers - they only handle the flat,
// well-formed bodies this firmware itself produces/consumes.
// extractJsonStr decodes backslash escapes (\n, \t, \", \\) so multi-line
// script bodies survive the trip from the browser editor. CR characters are
// dropped so Windows/Unix/Mac line endings all normalise to LF.
// ============================================================================
#include "util.h"

static int findKey(const String& json, const char* key) {
    String pat = "\"" + String(key) + "\"";
    int i = json.indexOf(pat);
    if (i < 0) return -1;
    i = json.indexOf(':', i + pat.length());
    if (i < 0) return -1;
    while ((size_t)++i < json.length() && isspace(json[i])) {}
    return i;
}

String normalizeEol(const String& s) {
    String r = s;
    r.replace("\r\n", "\n");
    r.replace('\r', '\n');
    return r;
}

bool extractJsonStr(const String& json, const char* key, String& out) {
    int i = findKey(json, key);
    if (i < 0 || json[i] != '"') return false;
    out = "";
    int p = i + 1;
    while (p < (int)json.length()) {
        char c = json[p];
        if (c == '"') return true;                       // closing quote
        if (c == '\\' && p + 1 < (int)json.length()) {
            char e = json[++p];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;                          // dropped -> LF only
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default:  out += e; break;
            }
        } else if (c != '\r') {
            out += c;
        }
        p++;
    }
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
        int q2 = -1;
        for (int p = q1 + 1; p < (int)body.length(); p++) {   // skip escapes
            if (body[p] == '\\') { p++; continue; }
            if (body[p] == '"') { q2 = p; break; }
        }
        if (q2 < 0) break;
        out.push_back(normalizeEol(body.substring(q1 + 1, q2)));
        start = q2 + 1;
    }
    return true;
}
