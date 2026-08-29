// ============================================================================
// wifiattack.h - WiFi deauth + handshake/PCAP capture (Step 3b)
// ----------------------------------------------------------------------------
// Techniques combined from reference implementations (see /references):
//  * brute32's wsl_bypasser: override ieee80211_raw_frame_sanity_check (no-op)
//    so libnet80211.a lets raw deauth frames leave the radio (ESP32-S3).
//  * applejuice's promiscuous sniffer: stations are captured by watching
//    data frames whose DESTINATION is our own softAP MAC.
//  * brute32's pcap serializer: minimal libpcap format written straight to
//    the SD card (LINKTYPE_IEEE802_11 = 105).
//
// ETHICS/LAW: deauth attacks disrupt networks you aim them at. Only run on
// networks you own or are authorized to test. The UI and the plan both
// require explicit operator confirmation before starting.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace wifiattack {

struct Stats {
    uint32_t deauths = 0;       // deauth frames sent
    uint32_t stations = 0;      // distinct stations deauthed
    uint32_t eapol = 0;         // EAPOL (handshake) frames captured
    uint32_t captured = 0;      // total packets written to PCAP
};

bool attacking();
bool sniffing();
Stats stats();

// --- deauth attack ---
// Scans for `ssid`, then runs the deauth+capture loop for `seconds`.
// The device's normal AP/webserver goes DOWN during the attack (channel
// conflict) and is restored afterwards. BOOT button aborts early.
bool startDeauth(const String& ssid, uint32_t seconds);
void stop();

// --- passive promiscuous PCAP capture ---
// Captures everything on `channel` for `seconds` to /pcap/<name>.pcap
bool startPcap(const String& name, uint8_t channel, uint32_t seconds);
void stopPcap();

} // namespace wifiattack
