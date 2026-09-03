# LilyStrike

**Advanced pentest & BadUSB toolkit for the LILYGO T-Dongle-S3**

LilyStrike turns a $10-looking USB dongle into a complete offensive-security toolkit: HID payload execution, WiFi recon & attacks, an encrypted storage volume, a full web UI with live WebSocket updates, and an MCP server so an AI assistant can drive the device directly.

> ⚠️ **AUTHORIZED USE ONLY.** LilyStrike is a penetration-testing tool. Using it against computers, networks, or WiFi systems you do not own or have *written* permission to test is illegal in most jurisdictions. The firmware displays an End-User License Agreement on first use and logs every action to an encrypted log on the card. You are responsible for how you use it.

---

## Hardware

| Component | Detail |
|---|---|
| Board | LILYGO T-Dongle-S3 (ESP32-S3, 16 MB flash, no PSRAM) |
| Display | ST7735 160×80 color LCD (SPI) |
| LED | APA102-2020 addressable RGB |
| Storage | microSD card (shares the USB connector through the MSC stack) |
| Button | BOOT (GPIO0) — safe-mode reset / WAIT_BUTTON trigger |
| USB | TinyUSB: HID keyboard + mouse + mass storage, simultaneously |

## Feature overview

- **DuckyScript interpreter** — HID payloads with variables, conditionals (`IF_OS` / `IF_SSID` / `IF_WIFI`), flow control, host-OS fingerprinting, and ~50 device commands (see [`docs/DUCKYSCRIPT.md`](docs/DUCKYSCRIPT.md))
- **Script Studio** — browser IDE with syntax highlighting, encrypted script storage on the SD card, per-script keyboard layout and description
- **Live packet analyzer** — channel-hopping 802.11 monitor: frame counts, AP list with BSSID/channel/security, last-frame decode, optional `.pcap` capture to SD
- **Deauth attack** — targeted deauthentication with 5 method variants and live TX counters
- **EvilAP + Karma** — rogue access point with a customizable captive portal (credential harvesting), plus Karma mode that spawns portals mirroring the networks devices are probing for
- **Host recon** — ARP sweep + TCP port scanner once the device has joined a network
- **False thumbdrive mode** — boots as an innocent read-only USB drive; screen and LED stay dark; hold nothing, look like storage
- **USB storage (MSC)** — expose the SD card over USB on demand (can be combined with encrypted storage)
- **Self-destruct** — one-tap (or scripted) wipe of all secrets and settings
- **Safe mode** — 3 consecutive failed boots ⇒ factory defaults in RAM, so the device is always recoverable
- **Scheduler & autostart** — run scripts on plug-in, on an interval, or at an absolute time
- **Power modes** — Low (80 MHz / 10 dBm), Normal (160 MHz / 17 dBm), High (240 MHz / 19.5 dBm)
- **MAC spoofing** — hardware default, per-boot random, or a fixed custom MAC
- **External tunnel** — reach the device's web UI from anywhere through a tiny relay (see `tools/relay_server.py`)
- **MCP server** — 12 tools so Claude/any MCP client can enumerate scripts, run payloads, and read device state (see [`docs/MCP.md`](docs/MCP.md))
- **Live web UI** — dark-themed, WebSocket-driven console: status strip updates every 2 s, log lines stream as they happen
- **Encrypted everything** — scripts, logs, settings and captures are AES-256-GCM encrypted at rest with your own encryption password

## Quick start

### 1. Build & flash

```bash
# requirements: PlatformIO CLI (pip install platformio)
cd ProjectCodename
pio run -t upload          # firmware
pio run -t uploadfs        # web UI (LittleFS image)
```

Insert a **microSD card** (FAT32, any size up to 256 GB tested). The card stores scripts, logs, and captures — all encrypted.

### 2. First connection

1. Power the dongle from any USB port. After boot the LCD shows the AP name and IP.
2. Join the WiFi access point:
   - SSID: `Dongle-Setup`
   - Password: `dongle1234`
3. Browse to **http://192.168.4.1** (or `http://lilystrike.local`).
4. Log in — default `admin` / `admin`.
5. Accept the EULA, then **immediately change** the web password (Settings → Login) and the AP credentials (Settings → WiFi).

### 3. Run your first payload

Plug the dongle into the **target** computer's USB port (it presents itself as an HID keyboard). In the web UI, open **BadUSB**, write a script:

```
REM open a terminal on Windows and print the device IP
DELAY 1000
GUI r
DELAY 500
STRING cmd
ENTER
DELAY 800
STRING echo hello from LilyStrike
ENTER
```

Press **▶ Run**. The target types it out with human-speed timing.

### 4. Join the device to your network (optional)

Either run a script containing:

```
CONNECT_AP YourNetwork yourPassword
```

or use the WiFi Tools page. Once joined, the device is reachable at its DHCP address *and* still exposes its own AP — both interfaces work simultaneously.

### 5. Enable the AI interface (optional)

Settings → **MCP enabled**, set a token (Settings → MCP), and point any MCP client at `POST http://<device-ip>/mcp` with the header `X-MCP-Token: <your-token>`. See [`docs/MCP.md`](docs/MCP.md).

## Defaults cheat-sheet

| Setting | Default | Where to change |
|---|---|---|
| AP SSID | `Dongle-Setup` | Settings → WiFi |
| AP password | `dongle1234` | Settings → WiFi |
| Web login | `admin` / `admin` | Settings → Login |
| mDNS name | `lilystrike.local` | Settings → Hostname |
| MCP server | disabled | Settings → MCP |
| Encryption password | *(empty — encryption disabled)* | Settings → Encryption |

## Example payloads

The [`examples/`](examples/) folder ships ready-made payloads: download-and-run
for Windows/Linux/macOS, a multi-OS `DETECT_OS` fingerprinting demo, and
document/picture exfiltration scripts that stage files onto the dongle's
encrypted SD card via USB storage. See [`examples/README.md`](examples/README.md).

## Repository layout

```
ProjectCodename/
├── src/               firmware sources (one file per subsystem)
├── include/           headers + pin map
├── data/www/          web UI (flashed to LittleFS with uploadfs)
├── examples/          ready-made payload scripts (install / exfil / multi-OS)
├── tools/relay_server.py  external-access relay (Flask, single file)
├── docs/              documentation (you are here)
└── platformio.ini     build config
```

## Documentation

| Doc | Contents |
|---|---|
| [`docs/DUCKYSCRIPT.md`](docs/DUCKYSCRIPT.md) | Full DuckyScript language reference + example payloads |
| [`docs/WEB-UI.md`](docs/WEB-UI.md) | Every page and feature of the web interface |
| [`docs/MCP.md`](docs/MCP.md) | MCP/AI integration: tools, auth, example client |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Security model, hardening, threat notes |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Common problems and fixes |

## Building from source

- **Platform:** pioarduino fork of platform-espressif32 (stock `espressif32` is frozen at an old core — do not switch).
- **USB mode:** `ARDUINO_USB_MODE=0` (TinyUSB) is required for HID; `CDC_ON_BOOT=0` because the TinyUSB stack is owned by the keyboard.
- **Raw WiFi TX:** the deauth/PCAP stack overrides a symbol in the WiFi library with `-Wl,-zmuldefs`. Don't remove that flag.
- Key libraries: AsyncTCP + ESPAsyncWebServer (mathieucarbou), ArduinoJson, Adafruit ST7735, pololu APA102.

## License / disclaimer

MIT licensed — see [`LICENSE`](LICENSE) (includes a responsible-use addendum). Provided for **lawful security research and education** on systems you own or have written permission to test. No warranty. The authors accept no liability for misuse. Check your local laws — possession of dual-use tooling is regulated in some jurisdictions.
