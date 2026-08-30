// ============================================================================
// wifiattack.h - WiFi deauth + handshake/PCAP capture (Step 3b)
// ----------------------------------------------------------------------------
// Techniques after studying four reference implementations (brute32,
// applejuice, Marauder, WifiPhisher). The WifiPhisher approach won:
//   * deauth frames transmit via WIFI_IF_STA (not AP!)
//   * channel lockout via esp_wifi_remain_on_channel (ROC) on the STA
//   * reason code 0x07 (Class 3 from nonassociated STA)
//   * esp_wifi_register_80211_tx_cb to COUNT REAL successful transmissions
//   * sniffer callback only enqueues to RAM; a writer task does SD I/O
//     (SD I/O from the WiFi callback context destabilizes the radio)
//   * world-safe country + max TX power + power-save off at attack start
//
// ETHICS/LAW: deauth attacks disrupt networks. Only run on networks you own
// or are authorized to test.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace wifiattack {

struct Stats {
    uint32_t deauths = 0;       // deauth frames sent (TX-confirmed)
    uint32_t deauthDrops = 0;   // frames the radio refused/dropped
    uint32_t stations = 0;      // stations discovered
    uint32_t eapol = 0;         // EAPOL (handshake) frames captured
    uint32_t captured = 0;      // total packets written to PCAP
};

bool attacking();
bool sniffing();
Stats stats();

bool startDeauth(const String& ssid, uint32_t seconds);   // attempt-once flow
void stop();                                              // abort + restore
bool startPcap(const String& name, uint8_t channel, uint32_t seconds);
void stopPcap();

} // namespace wifiattack
