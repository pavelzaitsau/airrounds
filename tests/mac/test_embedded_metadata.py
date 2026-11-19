import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

with patch.object(sys, "path", [str(Path(__file__).resolve().parents[2] / "tools/quality"), *sys.path]):
    import embedded


class FirmwareMetadataTest(unittest.TestCase):
    def test_revision_in_worktree_and_nested_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary).resolve()
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("GIT_")}

            def git(*args):
                return subprocess.run(
                    ["git", *args], cwd=checkout, env=environment, check=True,
                    capture_output=True, text=True,
                ).stdout.strip()

            git("init")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-m", "Fixture")
            revision = git("rev-parse", "HEAD")
            export = checkout / "export"
            export.mkdir()

            with patch.dict(os.environ, environment, clear=True):
                with patch.object(embedded, "ROOT", checkout):
                    self.assertIn(f"-DAIRROUNDS_REVISION={revision}", embedded.firmware_metadata({})[1])
                with patch.object(embedded, "ROOT", export):
                    self.assertIn("-DAIRROUNDS_REVISION=unknown", embedded.firmware_metadata({})[1])
                    overridden = embedded.firmware_metadata({"AIRROUNDS_REVISION": revision})
                    self.assertIn(f"-DAIRROUNDS_REVISION={revision}", overridden[1])
