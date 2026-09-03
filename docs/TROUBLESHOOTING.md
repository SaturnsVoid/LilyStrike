# Troubleshooting

## Can't reach the web UI

| Symptom | Cause / Fix |
|---|---|
| `Dongle-Setup` not visible | Power cycle the dongle; check the LCD — if it shows "thumbdrive"/dark, it booted in False Thumbdrive mode (re-insert or check MSC settings). If the WiFi interface was disabled in Settings (kill switch), press BOOT once (temporary) or reflash (permanent). |
| Connected to AP but no page | Browse to `http://192.168.4.1` **by IP**, not by name. mDNS (`.local`) needs mDNS support (Windows: install Bonjour, or use the IP). |
| Page loads but login loops | Cookie blocked by the browser (extensions/privacy modes) or you're bouncing between `192.168.4.1` and `lilystrike.local` — sessions are per-hostname. Use **one** address. |
| Was logged in, now everything 401s | The device **rebooted** — sessions are RAM-only by design. Log in again. If it happens repeatedly, you're hitting a crash: see the log (Status → Log) — boot lines at the end of the log = reboot. |
| 429 on login | Lockout after 5 bad passwords: 30 s doubling to 5 min. Wait it out. |

## Safe mode

**Sign:** yellow banner in the UI; AP name/password back to defaults; autostart doesn't run.

**Cause:** 3+ consecutive crashed boots. The device runs factory defaults **in RAM** — your saved settings/scripts are intact.

**Fix:** remove the cause (bad script in autostart is the usual suspect — clear the Autostart queue), then Reboot from Settings. A clean boot clears the counter.

## BadUSB payloads

| Symptom | Cause / Fix |
|---|---|
| Nothing types | Dongle must be plugged into the **target's** USB port (HID). Check USB mode: it needs TinyUSB (`ARDUINO_USB_MODE=0` — default in this repo). |
| Wrong characters typed (é vs e, y/z swap) | Keyboard layout mismatch — set the script's layout to the **target's** layout (script settings dropdown). |
| Types fine but caps are inverted | `DISABLE_CAPS` at the top of the script, or the target's Caps Lock state raced you. |
| Script starts typing immediately at boot | It's in the **Autostart** queue — review it in the BadUSB sidebar. |
| Script runs forever / types garbage | Press **Stop** in the UI (aborts between lines), or `STOP` in-script. Safe mode will save you if it survives reboots. |
| Indented lines "do nothing" | They run — the IDE's old highlighting bug made them look dead; update the UI (v56+). If a *command* is unknown, the device logs `unknown command` but continues. |

## WiFi features

| Symptom | Cause / Fix |
|---|---|
| Analyzer/deauth/Karma kicks the web UI offline | **By design** — one radio. The AP returns when the operation finishes (max 2 min for the analyzer). WebSocket clients reconnect automatically. |
| Deauth "sends" but nothing happens | Target on 5 GHz (this radio handles 2.4 GHz), client isolation, or 11ax protections. Check the channel in WiFi Tools; try the method variants. |
| PCAP opens with "malformed packets" in Wireshark | Keep FCS enabled in Wireshark (linktype 105 expects trailing FCS). |
| Karma list stays empty | Devices probe rarely — leave it listening a few minutes on a busy channel; watch the hit counter. |
| ARP sweep / port scan empty | The device must be joined to the network first (`CONNECT_AP` or WiFi Tools) — these tools use the station interface. |

## Storage

| Symptom | Cause / Fix |
|---|---|
| "SD CARD ERROR" on boot | Card not seated / unsupported format. FAT32, class 10 or better. |
| Scripts list empty but card has files | Encryption password mismatch (files written under a different password decrypt to nothing). |
| Uploaded file corrupt | Use `/api/filebin` (the Files page does) for binary; the text editor path normalizes line endings. |
| Card asks to be formatted by the host while MSC enabled | MSC gives the host raw access — disable USB_STORAGE before using the web file manager. |
| Capture produces no `.22000` file (or "no handshake pairs") | The sniffer cannot hear the dongle's OWN transmissions - a handshake where the dongle itself is the reconnecting client yields only M1/M3. Deauth a network where a **different** device (phone, laptop) is connected; that client's M2/M4 complete the pair. WPA3/PMF clients ignore deauth entirely. |
| Device restarts when a script runs `USB_STORAGE enable/disable` | Normal, not a crash: the USB descriptor (keyboard vs keyboard+drive) only changes at boot. The script stops at that line; run it again - the second run proceeds (the enable is a no-op the second time). |
| Device boot-loops (web UI never comes up, USB keeps re-connecting every few seconds) | Likely a corrupted card hanging the SD mount at boot. Remove the microSD and power on — if it boots, reformat the card (FAT32) and restore your files. The device is designed to run without a card. |

## MCP / AI

| Symptom | Cause / Fix |
|---|---|
| 401 `bad or missing X-MCP-Token` | Wrong token (check Settings → MCP), or header name typo (`X-MCP-Token`). |
| `-32002 MCP is disabled` | Enable it in Settings → MCP. Takes effect immediately. |
| Tools/list works, calls time out | A long script is running — poll `device_status` instead of firing more calls. |
| Client can't connect at all | The MCP endpoint is on the same HTTP server as the UI — reachable at `http://<device-ip>/mcp`, LAN or AP network. |

## Tunnel

| Symptom | Cause / Fix |
|---|---|
| Relay page never loads | Confirm `TUNNEL ON` (script or system_config) and that the relay URL/token are saved; the relay needs the device to be joined to a network with a route to the relay server. |
| Browser shows old responses | The relay queues one pending request per token — refresh to re-send. |

## Recovery matrix

| Situation | Recovery |
|---|---|
| Forgot web password | Flash/reflash does **not** reset NVS — use **Factory reset** from Settings if you still have access; otherwise safe mode (see below). |
| Locked out completely | Force 3 crashes (power-cycle during boot 3×) → **safe mode** boots with defaults (`admin`/`admin`, AP `Dongle-Setup`/`dongle1234`) → fix settings → Reboot. Your SD data is untouched. |
| SD data unreadable | Wrong/lost encryption password = data unrecoverable (AES-256-GCM). |
| Device won't boot at all | Hold BOOT while plugging in (download mode) and reflash: `pio run -t upload && pio run -t uploadfs`. |
| Want everything gone | Self-destruct (secrets+settings) or Format SD (all data). |
