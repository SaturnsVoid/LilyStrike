// ============================================================================
// scheduler.h - Script scheduling, cron-lite (Step 4 addition)
// ----------------------------------------------------------------------------
// Entries: script name + trigger. Triggers: every N minutes ("interval") or
// once at a specific epoch time ("at"). Stored encrypted at /schedule.enc.
// NTP time sync when a client network is available so "at" works across
// reboots. Never runs two scripts at once - defers if ducky is busy.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

namespace scheduler {

struct Entry {
    String script;          // .ds name in /scripts
    uint32_t intervalMin;   // >0 = recurring every N minutes
    uint32_t atEpoch;       // one-shot epoch (intervalMin==0)
    uint32_t lastRunEpoch;
};

void load();
bool save();
std::vector<Entry>& entries();
void add(const Entry& e);
bool remove(size_t idx);
void clearAll();
void handle();                      // call from loop()
String statusJson();

} // namespace scheduler
