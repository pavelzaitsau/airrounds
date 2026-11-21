# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def firmware_version():
    header = (ROOT / "firmware/esp32/uart/firmware_version.h").read_text()
    match = re.search(r'firmwareVersion\[\] = "([^"]+)";', header)
    if match is None:
        raise AssertionError("firmware_version.h declares no firmwareVersion")
    return match.group(1)


def flipper_version():
    manifest = (ROOT / "flipper/application.fam").read_text()
    match = re.search(r'fap_version\s*=\s*"([^"]+)"', manifest)
    if match is None:
        raise AssertionError("application.fam declares no fap_version string")
    return match.group(1)


class VersionTest(unittest.TestCase):
    def test_firmware_and_flipper_app_share_one_version(self):
        self.assertEqual(firmware_version(), flipper_version())

    def test_version_is_major_dot_minor(self):
        # The project keeps fap_version as a "major.minor" string, the form the firmware uses too.
        # fbt turns each part into an integer, so "0.01" would reach the app as "0.1".
        self.assertRegex(flipper_version(), r"^(0|[1-9]\d*)\.(0|[1-9]\d*)$")


if __name__ == "__main__":
    unittest.main()
