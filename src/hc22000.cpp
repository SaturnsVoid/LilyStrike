// ============================================================================
// hc22000.cpp - WPA handshake pcap -> hashcat 22000 converter
// ----------------------------------------------------------------------------
// Walks a LilyStrike capture (libpcap, LINKTYPE_IEEE802_11 with FCS, produced
// by wifiattack.cpp) and extracts EAPOL message pairs:
//   M1: KeyInfo ACK=1 MIC=0  (AP->client, carries ANonce)
//   M2: KeyInfo ACK=0 MIC=1  (client->AP, carries SNonce + MIC)
// Emits "WPA*02*..." lines (M1+M2 pairs) for hashcat -m 22000 / john wpapcap.
// The ESSID per BSSID is taken from beacons found in the same capture.
// NOT implemented: 03/04 message pairs, PMF (802.11w) - those sessions yield
// nothing (by design of PMF), anything hashcat can't use anyway.
// ============================================================================
#include "hc22000.h"
#include "crypt.h"
#include <SD_MMC.h>
#include <vector>
#include <string>

namespace hc {

struct SsidFor { uint8_t bssid[6]; char ssid[33]; };
struct PendingM1 { uint8_t bssid[6], sta[6], anonce[32]; };

static bool macEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }
static bool macIsBcast(const uint8_t* m) { return (m[0] & 0x01) != 0; }

static String hex(const uint8_t* p, int n) {
    static const char* H = "0123456789abcdef";
    String s; s.reserve(n * 2);
    for (int i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
    return s;
}
static String macHex(const uint8_t* m) { return hex(m, 6); }

String fromPcap(const String& path) {
    String out;
    sdLock();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { sdUnlock(); return out; }

    uint8_t gh[24];
    if (f.read(gh, 24) != 24 || gh[0] != 0xd4) { f.close(); sdUnlock(); return out; }

    std::vector<SsidFor> ssids;
    std::vector<PendingM1> m1s;

    while (f.available()) {
        uint8_t rh[16];
        if (f.read(rh, 16) != 16) break;
        uint32_t incl = rh[8] | (rh[9] << 8) | (rh[10] << 16) | ((uint32_t)rh[11] << 24);
        if (incl == 0 || incl > 2600) break;                 // corrupt/truncated
        std::vector<uint8_t> pkt(incl);
        if (f.read(pkt.data(), incl) != incl) break;

        if (incl < 36) continue;
        uint16_t rtLen = pkt[2] | (pkt[3] << 8);             // radiotap length (LE)
        if (rtLen < 8 || rtLen >= incl - 4) continue;
        const uint8_t* w = pkt.data() + rtLen;
        int wlen = (int)incl - rtLen - 4;                    // strip FCS
        if (wlen < 26) continue;

        uint8_t type = (w[0] >> 2) & 0x3, sub = (w[0] >> 4) & 0xF;

        // beacons: remember BSSID -> ESSID (first seen wins)
        if (type == 0 && sub == 8 && wlen >= 40) {
            uint8_t sl = w[37];
            if (sl > 0 && sl <= 32 && 38 + sl <= wlen && !macIsBcast(w + 16)) {
                bool known = false;
                for (auto& s : ssids) if (macEq(s.bssid, w + 16)) { known = true; break; }
                if (!known) {
                    SsidFor s2; memcpy(s2.bssid, w + 16, 6);
                    memset(s2.ssid, 0, 33); memcpy(s2.ssid, w + 38, sl);
                    ssids.push_back(s2);
                }
            }
            continue;
        }
        if (type != 2) continue;                             // data frames only

        uint8_t toDS = w[1] & 0x01, fromDS = w[1] & 0x02;
        if (toDS && fromDS) continue;                        // WDS/mesh
        const uint8_t* ap   = toDS ? w + 4  : w + 10;        // addr1 / addr2
        const uint8_t* sta  = toDS ? w + 10 : w + 4;
        const uint8_t* bss  = w + 16;                        // addr3 = BSSID
        if (macIsBcast(sta) || macIsBcast(bss)) continue;

        // LLC/SNAP + EAPOL ethertype?
        if (wlen < 32 + 4) continue;
        static const uint8_t LLC[6] = {0xAA,0xAA,0x03,0x00,0x00,0x00};
        if (memcmp(w + 24, LLC, 6) != 0 || w[30] != 0x88 || w[31] != 0x8E) continue;

        const uint8_t* e = w + 32;                           // EAPOL header
        int elen = wlen - 32;
        if (elen < 97) continue;                             // EAPOL-Key needs >= 97
        if (e[1] != 0x03) continue;                          // packet type = EAPOL-Key
        uint16_t keyInfo = (e[6] << 8) | e[7];               // key body: info at e[5..6]
        // EAPOL: ver(1) type(1) len(2) -> key body at e+4:
        //   desc(1) info(2) keylen(2) replay(8) nonce(32) iv(16) rsc(8) id(8) mic(16)
        const uint8_t* key  = e + 4;
        keyInfo = (key[1] << 8) | key[2];
        const uint8_t* nonce = key + 5 + 8;                  // key[13..44]
        const uint8_t* mic   = key + 5 + 8 + 32 + 16 + 8 + 8; // key[77..92]
        // 802.11i Key Information bits: bit8 (0x0100)=KEY_ACK, bit9
        // (0x0200)=KEY_MIC, bit10 (0x0400)=SECURE. The old 0x0080/0x0100
        // constants were INSTALL/ACK - no real handshake ever classified.
        bool ack = keyInfo & 0x0100, hasMic = keyInfo & 0x0200;
        bool secure = keyInfo & 0x0400;

        if (ack && !hasMic) {                                // M1: remember ANonce
            bool upd = false;
            for (auto& m : m1s)
                if (macEq(m.bssid, bss) && macEq(m.sta, sta)) {
                    memcpy(m.anonce, nonce, 32); upd = true; break;
                }
            if (!upd) {
                PendingM1 m; memcpy(m.bssid, bss, 6); memcpy(m.sta, sta, 6);
                memcpy(m.anonce, nonce, 32); m1s.push_back(m);
            }
            continue;
        }
        if (!ack && hasMic && !secure) {                     // M2: pair with M1 (M4 has SECURE too)
            for (auto& m : m1s) {
                if (!macEq(m.bssid, bss) || !macEq(m.sta, sta)) continue;
                String essid = "";
                for (auto& s : ssids) if (macEq(s.bssid, bss)) { essid = s.ssid; break; }
                if (!essid.length()) essid = "unknown";
                out += "WPA*02*" + hex(mic, 16) + "*" + hex(m.anonce, 32) + "*" +
                       hex(nonce, 32) + "*" + macHex(bss) + "*" + macHex(sta) + "*:" + essid + "\n";
                break;
            }
        }
    }
    f.close();
    sdUnlock();
    return out;
}

} // namespace hc
