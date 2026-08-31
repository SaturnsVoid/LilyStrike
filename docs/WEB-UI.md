# Web UI Guide

Browse to the device (default `http://192.168.4.1` or `http://lilystrike.local`), log in, and you get a single-page console with a dark terminal theme. A **live link** dot in the sidebar glows green when the WebSocket is connected; the header strip shows the script state, free RAM, and network status, updated every 2 seconds by push (no polling).

Login sessions: up to 6 concurrent (tabs, devices). Sessions live in RAM only — **a reboot logs everyone out**. Logout (bottom of the sidebar) invalidates only your own session.

---

## BadUSB (home page)

The Script Studio: a browser IDE with syntax highlighting (flow keywords, device commands, string literals, numbers), a script list on the left, and a gutter with line numbers.

- **Script list** — shows all saved scripts with your short description; click to load.
- **Name / description / layout** — each script carries a filename, a description shown in the sidebar list, and its own keyboard layout (see below).
- **▶ Run** — executes against the host the dongle is plugged into. Stop aborts.
- **Save / Delete** — scripts are AES-256-GCM encrypted on the SD card.
- The **Reference** link in the sidebar contains the full command list with copy-paste examples.

### Keyboard layouts

Per-script layout from: en_US, de_DE, es_ES, fr_CH, fr_FR, it_IT, pt_PT, pt_BR, sv_SE, da_DK, hu_HU, ja_JP. Layout affects what actually gets typed for non-ASCII characters — pick the target's layout, not yours.

## Files

Full-file-manager view of the SD card: browse, view/edit text, upload (base64 upload handles binary), create folders, delete. Files with the LilyStrike encryption envelope (or `.ds` scripts) are decrypted/encrypted transparently on view/save. **Everything on the card is reachable here by design.**

## WiFi Tools

Three panels in one page:

1. **WiFi Scanner** — AP+STA scan (~3 s): SSID, BSSID, channel, security badge, signal bars. Buttons per network hand the target to the Deauth or EvilAP pages pre-filled.
2. **Live Packet Analyzer** — channel-hopping monitor mode. Device goes **offline** while running (single radio; AP returns automatically, max 2 min). Shows frame totals (mgmt/data/ctrl), the last frame decoded (type, source/dest MAC, length, RSSI), and an AP list with BSSID, channel, security, and signal. Results are saved encrypted and shown on reload.
3. **Host Reconnaissance** — needs the device joined to a network (script `CONNECT_AP`): **ARP sweep** lists live hosts (IP + MAC); **port scan** probes one host (empty = top 25 ports, or comma list like `22,80,443`).

## Deauth

Sends deauthentication frames to force clients off a target AP. Pre-fill a target from the WiFi Scanner. Five method variants are selectable; the device is **offline during the attack** (single radio) and restores the AP afterwards. Live TX counters report frames sent.

*Lawful use only — deauthing networks you don't own is a crime in most places.*

## EvilAP

Rogue access point with a captive portal for credential harvesting:

- **Start/Stop** — the portal takes over the web server's port; the main UI is suspended while EvilAP runs.
- **Templates** — Generic WiFi, Apple, Google portal looks, plus a fully custom HTML editor.
- **Creds viewer** — harvested credentials are listed in the page and can be cleared.
- **Karma mode** — passively listens for probe requests from nearby devices (list with hit counts), then spawns a clone portal under a probed SSID. `/api/karma/stop` or the page's Stop ends it.

Victims who "log in" get their credentials recorded and pass-through internet (if the device has a backhaul).

## Live Control

Manual HID playground: click/type/mouse-pad panels that inject HID events live. Useful for demos, testing a target's response, or finishing a job a script started.

## Reference

The built-in DuckyScript manual: every command, argument description, and a copyable example. Same content as [`DUCKYSCRIPT.md`](DUCKYSCRIPT.md), always shipped with the firmware.

## Status

- Firmware version, free RAM (current + minimum watermark), CPU frequency, uptime
- SD card capacity/free space
- USB host present + fingerprinted OS + lock-key state
- WiFi AP clients, station network + IP
- Script state machine (Standby / Running / Finished) with the last script name
- MCP state (enabled? AI client connected? total calls?)
- Safe-mode banner if the device booted with defaults
- **Live log** — streams every log line the moment it happens

## Settings

Grouped cards, each with its own save button; empty fields = unchanged.

| Group | Contents |
|---|---|
| WiFi | AP SSID/password, hidden-AP toggle |
| Login | Web username/password (change `admin`/`admin` **immediately**) |
| Encryption | The at-rest password for scripts/logs/settings on SD. Empty = encryption off. **Changing it re-encrypts nothing retroactively** — old files need the old password |
| Display | Screen on at boot, brightness |
| Interface | Temporary disable (press BOOT to re-enable) / **permanent kill switch** (irreversible without reflash) |
| MCP | Enable server, token (min 8 chars) — takes effect immediately, no reboot |
| Power mode | Low (80 MHz/10 dBm) — Normal (160 MHz/17 dBm) — High (240 MHz/19.5 dBm, needs a strong USB port) |
| MAC spoofing | Hardware / random-per-boot / custom MAC |

### Danger zone

- **Reboot** — clean restart
- **Factory reset** — wipe all settings (keeps SD data)
- **Format SD** — destroy everything on the card
- **Self-destruct** — wipe secrets + settings and reboot (also scriptable — see `SELF_DESTRUCT`)

---

## Autostart & Scheduler

- **Autostart** (BadUSB page → sidebar): ordered list of scripts run in sequence when the dongle is plugged into a host. The chain continues across scripts unless you press Stop; a failed read logs an error and moves on.
- **Scheduler**: add any script with either `intervalMin` (re-run every N minutes) or `at` (absolute epoch time). Entries survive reboots (stored encrypted).

## MCP / AI

See [`MCP.md`](MCP.md) — 12 tools, token auth, works from any MCP client on the same network.
