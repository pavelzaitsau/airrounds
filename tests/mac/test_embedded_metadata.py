from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

with patch.object(sys, "path", [str(Path(__file__).resolve().parents[2] / "tools/quality"), *sys.path]):
    import embedded


def isolated_git_environment():
    """Return the environment without the caller's Git settings, hooks, or templates."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                       GIT_TEMPLATE_DIR="")
    return environment


class FirmwareMetadataTest(unittest.TestCase):
    def test_revision_in_worktree_and_nested_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary).resolve()
            environment = isolated_git_environment()

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

            with patch.dict(os.environ, environment, clear=True), redirect_stdout(io.StringIO()):
                with patch.object(embedded, "ROOT", checkout):
                    self.assertIn(f"-DAIRROUNDS_REVISION={revision}", embedded.firmware_metadata({})[1])
                with patch.object(embedded, "ROOT", export):
                    self.assertIn("-DAIRROUNDS_REVISION=unknown", embedded.firmware_metadata({})[1])
                    overridden = embedded.firmware_metadata({"AIRROUNDS_REVISION": revision})
                    self.assertIn(f"-DAIRROUNDS_REVISION={revision}", overridden[1])


class FirmwareMetadataGuardTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.environment = isolated_git_environment()

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.root, env=self.environment,
                              check=True, capture_output=True, text=True).stdout.strip()

    def metadata(self, root, environment=None):
        with patch.dict(os.environ, self.environment, clear=True):
            with patch.object(embedded, "ROOT", root):
                with redirect_stdout(io.StringIO()):
                    return embedded.firmware_metadata(environment or {})[1]

    def test_export_inside_another_checkout_is_unknown(self):
        self.git("init")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-m", "Outer")
        export = self.root / "export"
        # An empty .git folder is not a repository, so Git reports the outer checkout.
        (export / ".git").mkdir(parents=True)
        self.assertIn("-DAIRROUNDS_REVISION=unknown", self.metadata(export))

    def test_revision_override_must_be_a_full_revision(self):
        for value in ("abc123", "HEAD", "G" * 40, "a" * 41):
            with self.subTest(value=value):
                with self.assertRaisesRegex(SystemExit, "full Git revision or unknown"):
                    self.metadata(self.root, {"AIRROUNDS_REVISION": value})
        self.assertIn("-DAIRROUNDS_REVISION=unknown",
                      self.metadata(self.root, {"AIRROUNDS_REVISION": "unknown"}))

    def build_id(self):
        flags = self.metadata(self.root, {"AIRROUNDS_REVISION": "unknown"})
        return flags.split("-DAIRROUNDS_BUILD_ID=")[1].split()[0]

    def test_build_id_follows_firmware_sources_only(self):
        sources = self.root / "firmware/esp32"
        (sources / "uart").mkdir(parents=True)
        header = sources / "uart/frame.h"
        header.write_text("int a;\n")
        (sources / "notes.txt").write_text("one\n")
        first = self.build_id()
        self.assertRegex(first, r"^[0-9a-f]{16}$")
        (sources / "notes.txt").write_text("two\n")
        self.assertEqual(self.build_id(), first, "a non-source file must not change the ID")
        header.write_text("int b;\n")
        changed = self.build_id()
        self.assertNotEqual(changed, first, "a header change must change the ID")
        header.rename(sources / "uart/renamed.h")
        renamed = self.build_id()
        self.assertNotEqual(renamed, changed, "a renamed header must change the ID")
        (sources / "sketch.ino").write_text("void setup() {}\n")
        self.assertNotEqual(self.build_id(), renamed, "a sketch must count as source")

    def test_firmware_build_writes_stack_usage(self):
        self.assertIn(" -fstack-usage", self.metadata(self.root, {"AIRROUNDS_REVISION": "unknown"}))


class EmbeddedRunnerTest(unittest.TestCase):
    def test_failed_command_stops_the_build(self):
        with redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                embedded.run([sys.executable, "-c", "raise SystemExit(3)"], dict(os.environ))
            result = embedded.run([sys.executable, "-c", "print('ok')"], dict(os.environ),
                                  capture=True)
        self.assertEqual(result.stdout, "ok\n")

    def verify(self, output):
        reply = subprocess.CompletedProcess([], 0, output, "")
        with patch.object(embedded, "run", return_value=reply) as run:
            embedded.verify_image(Path("image.bin"), {})
        self.assertTrue(run.call_args.kwargs["capture"])

    def test_image_needs_a_valid_checksum_and_hash(self):
        valid = "Checksum: 4d (valid)\nValidation Hash: 3f9a (valid)\n"
        self.verify(valid)
        for output in ("", "Checksum: 4d (valid)\n", "Validation Hash: 3f9a (valid)\n",
                       valid.replace("Checksum: 4d (valid)", "Checksum: 4d (invalid, 4e)")):
            with self.subTest(output=output):
                with self.assertRaisesRegex(SystemExit, "failed .* verification"):
                    self.verify(output)

    def callgraph(self, entry):
        recorder = []
        with tempfile.TemporaryDirectory() as temporary:
            application = Path(temporary)
            (application / ".vscode").mkdir()
            database = [{"directory": "/sdk", "file": "/app/other.cpp",
                         "arguments": ["g++", "-o", "other.o", "-c", "/app/other.cpp"]}, entry]
            (application / ".vscode/compile_commands.json").write_text(json.dumps(database))
            output = application / "stack"

            def run(command, environment, cwd=None, capture=False):
                recorder.append((command, cwd))

            with patch.object(embedded, "run", run):
                result = embedded.flipper_callgraph(application, output)
            self.assertEqual(result, output / "airrounds.ci")
            self.assertTrue(output.is_dir())
            return recorder, output

    def test_callgraph_rebuilds_the_app_with_stack_reports(self):
        for entry in ({"directory": "/sdk", "file": "/app/airrounds.cpp",
                       "arguments": ["g++", "-c", "/app/airrounds.cpp", "-o", "build/airrounds.o"]},
                      {"directory": "/sdk", "file": "/app/airrounds.cpp",
                       "command": "g++ -c /app/airrounds.cpp -o build/airrounds.o"}):
            with self.subTest(form="arguments" if "arguments" in entry else "command"):
                recorder, output = self.callgraph(entry)
                (command, cwd), = recorder
                self.assertEqual(cwd, "/sdk")
                self.assertEqual(command, ["g++", "-c", "/app/airrounds.cpp", "-o",
                                           str(output / "airrounds.o"), "-fstack-usage",
                                           "-fcallgraph-info=su"])

    def test_callgraph_needs_the_app_in_the_database(self):
        with self.assertRaisesRegex(SystemExit, "airrounds.cpp is missing"):
            self.callgraph({"directory": "/sdk", "file": "/app/third.cpp", "arguments": ["g++"]})
