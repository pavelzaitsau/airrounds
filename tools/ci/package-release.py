#!/usr/bin/env python3
"""Merge the passive ESP32-S2 image and write the release manifest for the Flipper flasher.

`make embedded` builds the passive firmware and the Flipper `.fap`. This step merges the boot
image segments into one file at offset 0 and writes the manifest the flasher reads: the image
offset, size, SHA-256, and MD5, plus the build identity and the passive variant marker.

The MD5 is the digest the board returns from its ROM SPI_FLASH_MD5 command over the written region,
so the flasher compares the board's digest against this value. The bench confirms the merge offsets
against the core 2.0.11 build output; a wrong offset flashes a broken image the MD5 still passes.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(os.environ.get("AIRROUNDS_TOOLS", ROOT / ".airrounds-tools")).resolve()
ESP_OUTPUT = ROOT / "artifacts" / "quality" / "esp32" / "airrounds_uart"
BUILD = ROOT / "build" / "embedded" / "airrounds_uart"
RELEASE = ROOT / "artifacts" / "release"
# The ESP32-S2 boot image layout for a single-app partition table, from the core 2.0.11 build.
BOOTLOADER_OFFSET = "0x1000"
PARTITIONS_OFFSET = "0x8000"
BOOT_APP0_OFFSET = "0xe000"
APPLICATION_OFFSET = "0x10000"


def find(pattern, *directories):
    for directory in directories:
        matches = sorted(Path(directory).glob(pattern))
        if matches:
            return matches[0]
    raise SystemExit(f"Missing {pattern} under {[str(path) for path in directories]}")


def read_version(path, pattern):
    match = re.search(pattern, path.read_text())
    if not match:
        raise SystemExit(f"No version in {path}")
    return match.group(1)


def main():
    esptool = TOOLS / "arduino/venv/bin/esptool.py"
    core = TOOLS / "arduino/data/packages/esp32/hardware/esp32/2.0.11"
    application = ESP_OUTPUT / "airrounds_uart.ino.bin"
    bootloader = find("*.bootloader.bin", ESP_OUTPUT, BUILD)
    partitions = find("*.partitions.bin", ESP_OUTPUT, BUILD)
    boot_app0 = find("boot_app0.bin", core / "tools/partitions")
    if not application.is_file():
        raise SystemExit(f"Missing {application}; run make embedded first")

    RELEASE.mkdir(parents=True, exist_ok=True)
    image = RELEASE / "airrounds.bin"
    subprocess.run([str(esptool), "--chip", "esp32s2", "merge_bin", "-o", str(image),
                    BOOTLOADER_OFFSET, str(bootloader), PARTITIONS_OFFSET, str(partitions),
                    BOOT_APP0_OFFSET, str(boot_app0), APPLICATION_OFFSET, str(application)],
                   check=True, timeout=120)

    data = image.read_bytes()
    manifest = {
        "build_id": os.environ.get("AIRROUNDS_BUILD_ID", "unidentified"),
        "revision": os.environ.get("GITHUB_SHA", "unknown"),
        "fap_version": read_version(ROOT / "flipper/application.fam", r'fap_version="([^"]+)"'),
        "firmware_version": read_version(ROOT / "firmware/esp32/uart/firmware_version.h",
                                         r'firmwareVersion\[\] = "([^"]+)"'),
        "variant": "passive",
        "image": {
            "file": "airrounds.bin",
            "offset": 0,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "md5": hashlib.md5(data).hexdigest(),
        },
    }
    (RELEASE / "airrounds.manifest").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Release image {image} ({len(data)} bytes); manifest written", flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"Packaging timed out: {error.cmd}") from None
