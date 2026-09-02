# LilyStrike Example Payloads

Example DuckyScript payloads for common engagement tasks. Load them via the
web UI (**BadUSB → open / File Manager → upload to `/scripts`**) or MCP
(`write_script`), edit the marked URLs/paths, then run from the Script Studio.

**Authorized use only** — run these exclusively on systems you own or have
explicit written permission to test. Every script also carries this notice.

## Install / execute

| Script | Target | What it does |
|---|---|---|
| `win_download_run.ds` | Windows | Run-dialog → PowerShell: download `.exe` to `%TEMP%`, execute hidden |
| `linux_download_run.ds` | Linux | Ctrl+Alt+T → `wget`, `chmod +x`, execute detached |
| `mac_download_run.ds` | macOS | Spotlight → Terminal → `curl`, execute detached |
| `multios_recon_install.ds` | Any | `DETECT_OS` fingerprint, then the matching payload per OS (`IF_OS` demo) |

## Exfiltration to the device

These copy files **to the dongle itself**: `USB_STORAGE enable` re-enumerates
USB so the SD card appears as a removable drive (volume label `DISK`)
alongside the keyboard. Files land in `exfil/` on the card and are encrypted
at rest with your encryption password.

| Script | Target | What it does |
|---|---|---|
| `win_exfil_documents.ds` | Windows | `robocopy` Documents (pdf/doc/xls/txt) → first removable drive |
| `mac_exfil_pictures.ds` | macOS | Copy `~/Pictures` + Documents to `/Volumes/DISK/exfil`, notification on finish |
| `linux_exfil_documents.ds` | Linux | Copy Documents + Pictures to `/media|/run/media/$USER/DISK/exfil` |

## Notes

- **Edit the URLs** in the download scripts before running (`example.com`
  placeholders).
- `$` caution: the interpreter substitutes `$name` tokens in `STRING` lines
  with script variables. Any literal shell `$var` must be wrapped so its
  whitespace-token starts with `"` or another non-`$` character (the examples
  already follow this rule).
- Exfil scripts need the removable-drive mount: give USB 3-5 s to
  re-enumerate after `USB_STORAGE enable` (the scripts already wait).
- Windows `robocopy` targets `DriveType=2` (first removable drive). If the
  host has other USB storage attached, check where the files went in the
  device log.
- Ending a script with `USB_STORAGE disable` re-locks the card (host
  re-enumerates again); the lines are present but commented out.
