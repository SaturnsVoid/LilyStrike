// ============================================================================
// scheduler.cpp - cron-lite script scheduling (see scheduler.h)
// ----------------------------------------------------------------------------
// handle() is deliberately cheap and called from loop(); script execution
// goes through the existing ducky runner so nothing types in parallel.
// Time source: NTP over the client network (configTime). Interval checks
// and one-shots both compare against epoch, so nothing fires until the
// clock is actually synced.
// ============================================================================
#include "scheduler.h"
#include "config.h"
#include "crypt.h"
#include "ducky.h"
#include "util.h"
#include <SD_MMC.h>
#include <time.h>
#include <WiFi.h>
#include <vector>

namespace scheduler {

static std::vector<Entry> s_entries;
static bool s_loaded = false;
static bool s_ntpStarted = false;
static time_t s_nextCheck = 0;

std::vector<Entry>& entries() { return s_entries; }

static void startNtp() {
    if (s_ntpStarted) return;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    s_ntpStarted = true;
    logLine("scheduler: NTP started");
}

void load() {
    s_entries.clear();
    String t;
    if (decryptFromFile("/schedule.enc", t)) {
        // line format: script;intervalMin;atEpoch;lastRunEpoch
        int start = 0;
        while (start < (int)t.length()) {
            int nl = t.indexOf('\n', start);
            String line = t.substring(start, nl < 0 ? t.length() : nl);
            start = (nl < 0) ? t.length() : nl + 1;
            line.trim();
            if (!line.length()) continue;
            int p1 = line.indexOf(';');
            int p2 = line.indexOf(';', p1 + 1);
            int p3 = line.indexOf(';', p2 + 1);
            if (p1 < 0 || p2 < 0 || p3 < 0) continue;
            Entry e;
            e.script = line.substring(0, p1);
            e.intervalMin = line.substring(p1 + 1, p2).toInt();
            e.atEpoch = strtoul(line.substring(p2 + 1, p3).c_str(), nullptr, 10);
            e.lastRunEpoch = strtoul(line.substring(p3 + 1).c_str(), nullptr, 10);
            if (e.script.length()) s_entries.push_back(e);
        }
    }
    s_loaded = true;
}

bool save() {
    String t;
    for (auto& e : s_entries)
        t += e.script + ";" + String(e.intervalMin) + ";" + String((uint32_t)e.atEpoch)
             + ";" + String((uint32_t)e.lastRunEpoch) + "\n";
    return encryptToFile("/schedule.enc", t);
}

void add(const Entry& e) { s_entries.push_back(e); save(); }
bool remove(size_t idx) {
    if (idx >= s_entries.size()) return false;
    s_entries.erase(s_entries.begin() + idx);
    return save();
}
void clearAll() { s_entries.clear(); save(); }

static void runScript(const String& name) {
    String text;
    decryptFromFile(("/scripts/" + name).c_str(), text);
    if (!text.length()) { logLine("scheduler: cannot read " + name); return; }
    // Same per-script layout handling as the web run path.
    String meta;
    if (decryptFromFile(("/scripts/" + name + ".meta").c_str(), meta)) {
        String layout;
        extractJsonStr(meta, "layout", layout);
        if (layout.length()) ducky::setLayout(layout);
    }
    logLine("scheduler: running " + name);
    ducky::run(text, name);   // blocks loop() while typing; web stays responsive
}

void handle() {
    if (!s_loaded) load();
    if (!s_entries.size()) return;
    if (WiFi.status() != WL_CONNECTED) return;   // no network, no clock
    startNtp();
    time_t now = time(nullptr);
    if (now < 1700000000) return;                // clock not synced yet
    if (now < s_nextCheck) return;               // max one check / 10 s
    s_nextCheck = now + 10;

    bool dirty = false;
    for (size_t i = 0; i < s_entries.size(); i++) {
        Entry& e = s_entries[i];
        if (ducky::isRunning()) break;           // never double-type
        bool due = false;
        if (e.intervalMin > 0) {
            due = (e.lastRunEpoch == 0) ||
                  (now - (time_t)e.lastRunEpoch) >= (time_t)(e.intervalMin * 60);
        } else if (e.atEpoch > 0) {
            due = (now >= (time_t)e.atEpoch);
        }
        if (due) {
            runScript(e.script);
            if (e.intervalMin > 0) { e.lastRunEpoch = now; dirty = true; }
            else { s_entries.erase(s_entries.begin() + i); dirty = true; i--; }
        }
    }
    if (dirty) save();
}

String statusJson() {
    if (!s_loaded) load();
    String out = "[";
    for (size_t i = 0; i < s_entries.size(); i++) {
        auto& e = s_entries[i];
        if (i) out += ",";
        out += "{\"script\":\"" + e.script + "\",\"interval\":" + String(e.intervalMin) +
               ",\"at\":" + String((uint32_t)e.atEpoch) +
               ",\"last\":" + String((uint32_t)e.lastRunEpoch) + "}";
    }
    return out + "]";
}

} // namespace scheduler
