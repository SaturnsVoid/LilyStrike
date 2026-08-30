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
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/tcpip.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/tcpip.h>
#include <vector>
#include <algorithm>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/tcpip.h>

// Resolve the STA lwIP netif the WifiPhisher way - netif_default can be the
// AP netif in AP+STA mode, and calling etharp against the wrong netif is
// what reset the device.
static struct netif* staNetif() {
    esp_netif_t* en = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!en) return nullptr;
    return (struct netif*)esp_netif_get_netif_impl(en);
}

namespace hostrecon {

static volatile bool s_scanning = false;
bool scanning() { return s_scanning; }

static struct netif* staNetif() {
    esp_netif_t* en = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!en) return nullptr;
    return (struct netif*)esp_netif_get_netif_impl(en);
}

static String macStr(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0],m[1],m[2],m[3],m[4],m[5]);
    return String(b);
}

std::vector<Host> arpSweep() {
    std::vector<Host> out;
    if (WiFi.status() != WL_CONNECTED) return out;
    struct netif* nif = staNetif();        // BUGFIX: was netif_default (AP netif
    if (!nif) { s_scanning = false; return out; }   // in AP+STA -> crash)
    s_scanning = true;

    IPAddress mine = WiFi.localIP();
    uint32_t base = (mine[0]<<24)|(mine[1]<<16)|(mine[2]<<8);
    uint8_t myLast = mine[3];

    // Phase 1: ARP requests ON the tcpip thread via callback_wait.
    // Direct etharp_request from loopTask resets the device (no core lock
    // in this lwIP build; linkoutput ran from the wrong thread).
    struct SweepCtx { struct netif* nif; uint32_t base; uint8_t myLast; };
    SweepCtx ctx{nif, base, myLast};
    auto sendReqs = [](void* c) {
        auto* m = (SweepCtx*)c;
        for (int last = 1; last < 255; last++) {
            if (last == m->myLast) continue;
            ip4_addr_t dest;
            dest.addr = m->base | last;
            etharp_request(m->nif, &dest);
            if ((last % 16) == 0) delay(2);
        }
    };
    tcpip_callback_wait(sendReqs, &ctx);

    delay(600);   // let replies land

    for (int last = 1; last < 255; last++) {
        if (last == myLast) continue;
        ip4_addr_t dest;
        dest.addr = base | last;
        struct eth_addr* eth = nullptr;
        const ip4_addr_t* ipret = nullptr;
        if (etharp_find_addr(nif, &dest, &eth, &ipret) >= 0 && eth) {
            Host h;
            IPAddress ip((base>>24)&0xFF, (base>>16)&0xFF, (base>>8)&0xFF, last);
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
