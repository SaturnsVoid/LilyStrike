# Security Model

What the device protects, what it deliberately doesn't, and how to operate it safely.

## At rest

- **Scripts, logs, settings and captures** are encrypted with AES-256-GCM using your **encryption password** (Settings → Encryption). The key is derived from that password and never stored — no password, no decryption. Files without the envelope are stored raw.
- The **encryption password is not recoverable.** Lose it and the SD data is gone.
- Changing the password affects new writes; existing files still need the password they were written with.
- Removing the card gives an attacker nothing but ciphertext.

## In transit

- **The web UI and API are plain HTTP.** This is a deliberate hardware-class tradeoff (no TLS on this MCU in a product sense). Treat the device's WiFi network as the trust boundary:
  - Use a strong AP password (Settings → WiFi; default `dongle1234` must be changed).
  - On networks you don't control, assume the UI can be observed; use the tunnel only over links you trust.
- **Login sessions** are random 24-hex-char tokens (hardware RNG), stored in RAM only, 6 concurrent max, HttpOnly + SameSite=Strict. Reboot = all sessions gone. Logout kills only your own.
- **Login lockout**: 5 consecutive failures → 30 s cooldown, doubling to 5 min.
- **WebSocket** requires the session cookie; unauthenticated upgrades are rejected before the socket upgrades.
- **CSRF**: the session cookie's `SameSite=Strict` blocks cross-site requests from web pages the operator visits.
- **MCP** is disabled by default and gated by its own token.

## Attack surface (for the owner's awareness)

| Surface | Exposure | Notes |
|---|---|---|
| AP `Dongle-Setup` | WPA2, LAN-only | Rename + repassword on first boot |
| Web UI / REST | HTTP on port 80 | Session-gated (except login/static) |
| `/ws` events | Status + log stream | Session-gated at handshake |
| `/mcp` | JSON-RPC | Token-gated; disabled by default |
| EvilAP portal | Port 80 during EvilAP | Deliberately public (it's the lure) |
| USB MSC | SD over USB | Off unless enabled; read-only in thumbdrive mode |
| Physical | BOOT button, SD card | Card is encrypted; button triggers safe-mode/WAIT_BUTTON |

## Safe mode

If the firmware crashes 3 times in a row, the next boot runs with **factory defaults in RAM** (saved settings are untouched): AP reverts to `Dongle-Setup`/`dongle1234`, autostart is off, MCP off. Fix the cause, then Reboot — the counter clears after a clean boot. This exists so a bad payload can never permanently lock you out.

## Data the device keeps

- `/logs/system.log.enc` — every log line (logins, script runs, MCP calls, attacks) with timestamps. Readable from Status → Log with the encryption password.
- Harvested EvilAP credentials — encrypted on SD, viewable in the EvilAP page, clearable.
- Captures — `/pcap/*.pcap`.
- **Self-destruct** (UI or script) wipes secrets and settings; **Format SD** destroys everything.

## Known limitations (honest list)

- No TLS — see "in transit" above.
- The whole SD card is browsable through the authenticated Files API; path sandboxing is pointless there by design.
- The MCP token can be passed as a URL query (compatibility) — prefer the header.
- Password comparison isn't constant-time; over WiFi the timing attack is impractical, but it's noted for completeness.
- The dongle is a physical HID device — by the time you're reading UI logs, the payload already ran. Plan payloads accordingly.

## Hardening checklist (first 10 minutes)

1. Change web login (Settings → Login).
2. Change AP SSID/password (Settings → WiFi).
3. Set an **encryption password** (Settings → Encryption) — do this before storing anything sensitive.
4. Set the MCP token if you use MCP; leave MCP disabled otherwise.
5. Review the EULA and local law for every engagement; keep written authorization.
6. Consider the interface kill switches when transporting the device (temporary off = BOOT press re-enables; permanent = reflash).
