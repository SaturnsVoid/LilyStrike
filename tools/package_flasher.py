#!/usr/bin/env python3
"""
package_flasher.py - build the LilyStrike web-flasher bundle (ESP Web Tools).

Run after `pio run -t buildfs`:
    python3 tools/package_flasher.py

Produces flasher/ : index.html + manifest.json + all flash parts (hashed),
ready for GitHub Pages or zipping as-is (see flasher/serve.py for localhost).
"""
import hashlib, json, os, shutil, subprocess, sys

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "t-dongle-s3")
OUT   = os.path.join(ROOT, "flasher")
CORE_TOOLS = "/home/user/.platformio/packages/framework-arduinoespressif32/tools/partitions"

VERSION = "1.0.1"
PARTS = [  # (label, source path, flash offset)
    ("bootloader.bin", os.path.join(BUILD, "bootloader.bin"),                       0x0),
    ("partitions.bin", os.path.join(BUILD, "partitions.bin"),                       0x8000),
    ("boot_app0.bin",  os.path.join(CORE_TOOLS, "boot_app0.bin"),                   0xE000),
    ("firmware.bin",   os.path.join(BUILD, "firmware.bin"),                         0x10000),
    ("littlefs.bin",   os.path.join(BUILD, "littlefs.bin"),                         0xC90000),
]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    missing = [p for _, p, _ in PARTS if not os.path.exists(p)]
    if missing:
        sys.exit(f"missing build outputs: {missing}\nRun: pio run -t buildfs")
    os.makedirs(OUT, exist_ok=True)

    parts = []
    for name, src, offset in PARTS:
        dst = os.path.join(OUT, name)
        shutil.copyfile(src, dst)
        parts.append({"path": name, "offset": offset, "sha256": sha256(dst)})
        print(f"  {name:16} {os.path.getsize(dst):>9} B  @ 0x{offset:06X}")

    manifest = {
        "name": "LilyStrike",
        "version": VERSION,
        "new_install_improv": False,          # first-config is via the device AP
        "new_install_enable_erase": True,     # optional full-erase checkbox
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": parts,
        }],
    }
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("manifest.json written")
    print(f"\nBundle ready in {OUT}/  (serve over HTTPS or localhost)")

if __name__ == "__main__":
    main()
