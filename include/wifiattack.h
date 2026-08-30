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

bool busy();            // attacking || sniffing - SD writes should pause
enum Method : uint8_t {
    M_DEAUTH = 0,       // classic deauth frames
    M_DISASSOC,         // disassociation frames (type c0 -> a0)
    M_AUTH_FLOOD,       // authentication request flood
    M_EAPOL_LOGOFF,     // EAPOL-Logoff injection
    M_BEACON_SPAM       // fake beacon spam on the target channel
};
bool startDeauth(const String& ssid, uint32_t seconds, uint8_t method = 0);
void stop();                                              // abort + restore
bool startPcap(const String& name, uint8_t channel, uint32_t seconds);

// Live analyzer: while sniffing (or an attack is running), counts frames by
// type + tracks APs seen on the current channel. Cheap - read anytime.
struct ApInfo {
    char ssid[33];
    char bssid[18];      // "AA:BB:CC:DD:EE:FF"
    int8_t rssi;
    uint8_t channel;
    bool secure;         // encrypted network?
};
struct LiveStats {
    uint32_t mgmt = 0, data = 0, ctrl = 0, total = 0, bytes = 0;
    uint8_t channel = 0;
};
LiveStats liveStats();
std::vector<ApInfo> liveAps();
// Last captured frame metadata (for the live detail view)
struct FrameInfo {
    uint8_t type, subtype;
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
    char src[18], dst[18];
};
FrameInfo lastFrame();
void analyzerStart();      // enable counting + channel hopping (no SD writes)
void analyzerStop();
void stopPcap();

} // namespace wifiattack
