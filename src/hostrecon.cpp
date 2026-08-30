// ============================================================================
// hostrecon.cpp - ARP sweep + TCP connect port scan (see hostrecon.h)
// ----------------------------------------------------------------------------
// ARP sweep technique from WifiPhisher networking/scanner.c: batch
// etharp_request() broadcasts, wait, then read lwIP's ARP table via
// etharp_find_addr(). Runs entirely inside lwIP - no raw sockets needed.
// Port scan: lwip_connect() with short timeouts per port (TCP connect scan).
// ============================================================================
#include "hostrecon.h"
#include "config.h"
#include "ducky.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <netif/etharp.h>
#include <vector>
#include <algorithm>

namespace hostrecon {

static volatile bool s_scanning = false;
bool scanning() { return s_scanning; }

static String macStr(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0],m[1],m[2],m[3],m[4],m[5]);
    return String(b);
}

std::vector<Host> arpSweep() {
    std::vector<Host> out;
    if (WiFi.status() != WL_CONNECTED) return out;
    s_scanning = true;

    IPAddress mine = WiFi.localIP();
    IPAddress gw = WiFi.gatewayIP();
    uint32_t base = (mine[0]<<24)|(mine[1]<<16)|(mine[2]<<8);
    uint8_t myLast = mine[3];

    // Ask every address in the /24 (skip ours). ARP replies populate lwIP's
    // table; we poll entries afterward. Batches + yields keep lwIP alive.
    for (int last = 1; last < 255; last++) {
        if (last == myLast) continue;
        IPAddress ip((base>>24)&0xFF, (base>>16)&0xFF, (base>>8)&0xFF, last);
        ip4_addr_t dest;
        dest.addr = static_cast<uint32_t>(ip);
        etharp_request(netif_default, &dest);
        if ((last % 16) == 0) delay(2);
    }
    delay(600);   // let replies land

    for (int last = 1; last < 255; last++) {
        if (last == myLast) continue;
        IPAddress ip((base>>24)&0xFF, (base>>16)&0xFF, (base>>8)&0xFF, last);
        ip4_addr_t dest;
        dest.addr = static_cast<uint32_t>(ip);
        struct eth_addr* eth = nullptr;
        const ip4_addr_t* ipret = nullptr;
        if (etharp_find_addr(netif_default, &dest, &eth, &ipret) >= 0 && eth) {
            Host h;
            h.ip = ip.toString();
            h.mac = macStr(eth->addr);
            out.push_back(h);
        }
        if ((last % 32) == 0) delay(1);
    }
    s_scanning = false;
    return out;
}

std::vector<uint16_t> portScan(const String& ipStr, const std::vector<uint16_t>& ports) {
    std::vector<uint16_t> open;
    if (WiFi.status() != WL_CONNECTED) return open;
    s_scanning = true;

    IPAddress ip;
    if (!ip.fromString(ipStr)) { s_scanning = false; return open; }

    for (uint16_t port : ports) {
        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) break;
        // short timeout: closed ports die on refusal, filtered ones timeout
        timeval tv = { .tv_sec = 0, .tv_usec = 120000 };
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = lwip_htons(port);
        sa.sin_addr.s_addr = static_cast<uint32_t>(ip);
        if (lwip_connect(sock, (sockaddr*)&sa, sizeof(sa)) == 0)
            open.push_back(port);
        lwip_close(sock);
        delay(1);
    }
    s_scanning = false;
    return open;
}

} // namespace hostrecon
