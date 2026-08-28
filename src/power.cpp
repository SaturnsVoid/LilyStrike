#include "power.h"
#include <Preferences.h>
#include <esp32-hal-cpu.h>
#include <WiFi.h>

namespace power {
static Mode s_mode = PM_NORMAL;

void load() {
    Preferences p; p.begin("power", true);
    uint8_t m = p.getUChar("mode", PM_NORMAL);
    s_mode = (m <= PM_HIGH) ? (Mode)m : PM_NORMAL;
    p.end();
}
void set(Mode m) {
    s_mode = m;
    Preferences p; p.begin("power", false);
    p.putUChar("mode", (uint8_t)m);
    p.end();
}
Mode mode() { return s_mode; }

void apply() {
    switch (s_mode) {
        case PM_LOW:    setCpuFrequencyMhz(80);  WiFi.setTxPower((wifi_power_t)40); break; // 10 dBm
        case PM_HIGH:   setCpuFrequencyMhz(240); WiFi.setTxPower((wifi_power_t)78); break; // 19.5 dBm
        default:        setCpuFrequencyMhz(160); WiFi.setTxPower((wifi_power_t)68); break; // 17 dBm
    }
}
}
