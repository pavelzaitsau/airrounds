"""The active-build transmit gate for firmware/active.

tests/mac/test_receive_only.py proves the default build has no transmit path. This gate proves the
complementary property for the opt-in active build: the one raw transmit call is confined to
firmware/active, and that directory adds no other transmit path and no unreviewed radio mode. See
docs/t12-architecture.md and docs/acceptable-use.md, "Active operation".
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVE = ROOT / "firmware/active"
SOURCE_SUFFIXES = (".h", ".c", ".cpp", ".ino")

# The single raw transmit call the active build is allowed; every other transmit path stays banned.
TRANSMIT_CALL = "esp_wifi_80211_tx"
# Radio calls firmware/active may name. Only the one transmit call; it names no esp_wifi_* setup call,
# because it reuses the shared PassiveVisit radio bring-up.
RADIO_ALLOWLIST = frozenset({TRANSMIT_CALL})
# The transmit-capable mode the active build sets through PassiveVisit::setRadioMode, plus the
# receive-only mode it may still name.
MODE_ALLOWLIST = frozenset({"WIFI_MODE_NULL", "WIFI_MODE_STA"})

RADIO_CALL = re.compile(r"esp_wifi_\w+|esp_now_\w+|WiFi\.\w+")
RADIO_MODE = re.compile(r"WIFI_MODE_\w+")


def source_files(root):
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in SOURCE_SUFFIXES)


class ActiveTransmitTest(unittest.TestCase):
    def test_active_sources_exist(self):
        self.assertTrue(source_files(ACTIVE), "no firmware/active sources found to scan")

    def test_exactly_one_confined_transmit_call(self):
        calls = []
        for path in source_files(ACTIVE):
            calls += [(path, token) for token in RADIO_CALL.findall(path.read_text(errors="replace"))]
        transmit = [call for call in calls if call[1] == TRANSMIT_CALL]
        self.assertEqual(len(transmit), 1,
                         f"firmware/active must hold exactly one {TRANSMIT_CALL} call: {transmit}")
        other = [call for call in calls if call[1] not in RADIO_ALLOWLIST]
        self.assertEqual(other, [], f"unreviewed radio call in firmware/active: {other}")

    def test_only_allowed_radio_modes(self):
        violations = []
        for path in source_files(ACTIVE):
            for token in RADIO_MODE.findall(path.read_text(errors="replace")):
                if token not in MODE_ALLOWLIST:
                    violations.append((path, token))
        self.assertEqual(violations, [], f"unreviewed radio mode in firmware/active: {violations}")

    def test_gate_fails_on_a_second_transmit_path(self):
        # A gate that cannot fail proves nothing: a second transmit path must break it.
        text = "void extra() { esp_now_send(); esp_wifi_80211_tx(); }\n"
        calls = [token for token in RADIO_CALL.findall(text)]
        other = [token for token in calls if token not in RADIO_ALLOWLIST]
        self.assertTrue(other, "the gate must reject a second transmit path")


if __name__ == "__main__":
    unittest.main()
