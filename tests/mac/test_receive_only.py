"""The receive-only build gate for firmware/esp32.

docs/acceptable-use.md names this test as the automated check that no transmit
path reaches the firmware. The no-transmission property the T9 security
assurance gap relies on rests on it. See docs/t11.md, "Receive-only test".

The gate uses an allowlist, not a denylist: every Wi-Fi radio identifier the
firmware names must be on the list below, and any other radio call or any
non-NULL radio mode fails. A denylist would miss a transmit path nobody thought
of; the allowlist fails on any radio call nobody reviewed.

Adding a name to the allowlist is a reviewed change: the commit states why the
call cannot transmit.
"""

import re
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware/esp32"
ACTIVE = ROOT / "firmware/active"
SOURCE_SUFFIXES = (".h", ".c", ".cpp", ".ino")

# Every esp_wifi_*, esp_now_*, or WiFi.* identifier the firmware uses today. Each one receives
# or configures the radio and cannot transmit a frame. esp_wifi_start only enables the radio in
# WIFI_MODE_NULL, which has no transmit queue.
RADIO_ALLOWLIST = frozenset({
    "esp_wifi_init",
    "esp_wifi_deinit",
    "esp_wifi_start",
    "esp_wifi_stop",
    "esp_wifi_set_storage",
    "esp_wifi_set_mode",
    "esp_wifi_set_channel",
    "esp_wifi_get_channel",
    "esp_wifi_get_country",
    "esp_wifi_set_promiscuous",
    "esp_wifi_set_promiscuous_rx_cb",
    "esp_wifi_set_promiscuous_filter",
    "esp_wifi_set_promiscuous_ctrl_filter",
})

# The only radio mode that cannot transmit. A station or access point mode has a transmit path.
ALLOWED_MODE = "WIFI_MODE_NULL"

RADIO_CALL = re.compile(r"esp_wifi_\w+|esp_now_\w+|WiFi\.\w+")
RADIO_MODE = re.compile(r"WIFI_MODE_\w+")


def source_files(root):
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in SOURCE_SUFFIXES)


def scan_tree(root):
    """Every allowlist violation under root, as (path, token) pairs. Comments are scanned too."""
    violations = []
    for path in source_files(root):
        text = path.read_text(errors="replace")
        for token in RADIO_CALL.findall(text):
            if token not in RADIO_ALLOWLIST:
                violations.append((path, token))
        for token in RADIO_MODE.findall(text):
            if token != ALLOWED_MODE:
                violations.append((path, token))
    return violations


class ReceiveOnlyTest(unittest.TestCase):
    def test_firmware_uses_only_allowlisted_radio_calls(self):
        self.assertTrue(source_files(FIRMWARE), "no firmware sources found to scan")
        violations = scan_tree(FIRMWARE)
        self.assertEqual(violations, [], f"unreviewed radio use in firmware/esp32: {violations}")

    def test_default_firmware_does_not_include_active(self):
        # The token grep cannot see an include, so a firmware/esp32 file that pulled in a
        # firmware/active header would link the transmit path into the default image unseen. See
        # tests/mac/test_active_transmit.py for the active build's own confinement gate.
        active_headers = {path.name for path in ACTIVE.rglob("*") if path.suffix in SOURCE_SUFFIXES}
        include = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
        violations = []
        for path in source_files(FIRMWARE):
            for line in path.read_text(errors="replace").splitlines():
                match = include.match(line)
                if match and (Path(match.group(1)).name in active_headers or
                              "active/" in match.group(1)):
                    violations.append((path, match.group(1)))
        self.assertEqual(violations, [],
                         f"a default-build file includes a firmware/active header: {violations}")

    def test_gate_fails_on_each_planted_transmit_path(self):
        # A gate that cannot fail proves nothing: plant each forbidden form and expect a failure.
        plantings = (
            "esp_wifi_80211_tx",
            "esp_wifi_internal_tx",
            "esp_wifi_connect",
            "esp_wifi_scan_start",
            "esp_now_send",
            "WiFi.begin",
            "WiFi.softAP",
            "WIFI_MODE_STA",
            "WIFI_MODE_AP",
            "// esp_wifi_80211_tx in a comment still fails",
        )
        for planting in plantings:
            with self.subTest(planting=planting):
                with tempfile.TemporaryDirectory(prefix="airrounds-receive-only-") as temporary:
                    copy = Path(temporary) / "esp32"
                    shutil.copytree(FIRMWARE, copy)
                    self.assertEqual(scan_tree(copy), [], "the copy is clean before planting")
                    planted = copy / "planted_transmit_path.h"
                    planted.write_text(f"void planted() {{ {planting}; }}\n")
                    self.assertTrue(scan_tree(copy),
                                    f"the gate missed the planted {planting!r}")


if __name__ == "__main__":
    unittest.main()
