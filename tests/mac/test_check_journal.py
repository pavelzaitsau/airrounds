from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools/mac/check_journal.py"

HEADER = ("format=airrounds-session-v4\n"
          "firmware=0.2 build=b revision=r protocol=03 radio=active mode=active\n"
          "boot=0000000000001234 session=00000007 state=complete outcome=saved points=1\n"
          "started_us=100 ended_us=5000000 budget_us=60000000\n"
          "point=1 bssid=020000000001 channel=6 outcome=saved pcap=-\n")


class CheckJournalTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.journal = Path(directory.name) / "session.log"

    def write(self, *tx_lines):
        self.journal.write_text(HEADER + "".join(line + "\n" for line in tx_lines))

    def run_check(self, *options):
        return subprocess.run([sys.executable, str(TOOL), str(self.journal), *options],
                              capture_output=True, text=True, timeout=30, check=False)

    def test_accepts_a_unicast_audit(self):
        self.write("tx point=1 frames=3 reason=7 client=122233445566")
        result = self.run_check("--require-audit")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_accepts_a_zero_audit(self):
        self.write("tx point=1 frames=0 reason=0 client=000000000000")
        self.assertEqual(self.run_check().returncode, 0)

    def test_rejects_a_group_client(self):
        self.write("tx point=1 frames=3 reason=7 client=012233445566")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("group address", result.stderr)

    def test_rejects_a_broadcast_client(self):
        self.write("tx point=1 frames=3 reason=7 client=ffffffffffff")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("broadcast", result.stderr)

    def test_rejects_frames_with_a_zero_client(self):
        self.write("tx point=1 frames=3 reason=7 client=000000000000")
        self.assertEqual(self.run_check().returncode, 1)

    def test_rejects_over_the_per_visit_cap(self):
        self.write("tx point=1 frames=7 reason=7 client=122233445566")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("per-visit cap", result.stderr)

    def test_rejects_over_the_round_cap(self):
        self.write("tx point=1 frames=6 reason=7 client=122233445566",
                   "tx point=2 frames=6 reason=7 client=122233445566",
                   "tx point=3 frames=6 reason=7 client=122233445566",
                   "tx point=4 frames=6 reason=7 client=122233445566",
                   "tx point=5 frames=6 reason=7 client=122233445566",
                   "tx point=6 frames=6 reason=7 client=122233445566")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("per-round cap", result.stderr)

    def test_rejects_a_malformed_tx_line(self):
        self.write("tx point=1 frames=three reason=7 client=122233445566")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("malformed", result.stderr)

    def test_require_audit_fails_on_a_passive_journal(self):
        self.journal.write_text(HEADER.replace("v4", "v3").replace("radio=active", "radio=passive"))
        self.assertEqual(self.run_check("--require-audit").returncode, 1)
        self.assertEqual(self.run_check().returncode, 0)


if __name__ == "__main__":
    unittest.main()
