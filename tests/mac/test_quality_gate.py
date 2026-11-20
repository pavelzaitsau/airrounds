from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

with patch.object(sys, "path", [str(Path(__file__).resolve().parents[2] / "tools/quality"), *sys.path]):
    import check


class TestDiscoveryTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        (self.root / "tests").mkdir()
        (self.root / "tests/__init__.py").touch()

    def touch(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def unreachable(self):
        emulator = self.root / "tests/flipper/ui_emulator_test.cpp"
        sources = sorted(path for path in (self.root / "tests").glob("*/*_test.cpp")
                         if path != emulator)
        with patch.multiple(check, ROOT=self.root, EMULATOR_TEST=emulator, SOURCES=sources):
            return [path.relative_to(self.root).as_posix() for path in check.unreachable_tests()]

    def test_reachable_layout_passes(self):
        self.touch("tests/storage/a_test.cpp")
        self.touch("tests/flipper/ui_emulator_test.cpp")
        self.touch("tests/mac/__init__.py")
        self.touch("tests/mac/test_tool.py")
        self.assertEqual(self.unreachable(), [])

    def test_nested_cpp_test_is_reported(self):
        self.touch("tests/storage/deep/b_test.cpp")
        self.assertEqual(self.unreachable(), ["tests/storage/deep/b_test.cpp"])

    def test_python_test_outside_a_package_is_reported(self):
        self.touch("tests/tools/test_new.py")
        self.touch("tests/mac/__init__.py")
        self.touch("tests/mac/inner/test_inner.py")
        self.assertEqual(self.unreachable(),
                         ["tests/mac/inner/test_inner.py", "tests/tools/test_new.py"])

    def test_node_tests_are_found_in_docs_and_tests(self):
        first = self.touch("docs/ui/a.test.cjs")
        second = self.touch("tests/web/b.test.cjs")
        self.touch("docs/ui/helper.cjs")
        with patch.object(check, "ROOT", self.root):
            self.assertEqual(check.node_tests(), [first, second])

    def test_missing_node_tests_fail(self):
        with patch.object(check, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "No Node tests found"):
                check.node_tests()


class LeakCanaryTest(unittest.TestCase):
    def canary(self, returncode, stderr):
        result = subprocess.CompletedProcess([], returncode, "", stderr)
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(check, "run"), patch.object(check, "tool", return_value="clang++"), \
                    patch.object(check.subprocess, "run", return_value=result) as runner:
                with redirect_stdout(io.StringIO()):
                    check.leak_canary(Path(directory), [], {})
                self.assertIn("leak_canary", runner.call_args.args[0][0])
                options = runner.call_args.kwargs
                self.assertEqual((options["capture_output"], options["text"], options["check"]),
                                 (True, True, False))

    def test_reported_leak_passes(self):
        self.canary(1, "==1==ERROR: LeakSanitizer: detected memory leaks\n")

    def test_clean_exit_fails(self):
        with self.assertRaisesRegex(SystemExit, "LeakSanitizer did not report"):
            self.canary(0, "")

    def test_failure_without_a_leak_report_fails(self):
        # Apple clang aborts with "detect_leaks is not supported on this platform".
        with self.assertRaisesRegex(SystemExit, "LeakSanitizer did not report"):
            self.canary(134, "AddressSanitizer: detect_leaks is not supported on this platform.\n")

    def test_leak_detection_is_enabled(self):
        self.assertIn("detect_leaks=1", check.ASAN_OPTIONS)



class Recorder:
    """Stands in for check.run/embedded.run and remembers every command."""

    def __init__(self):
        self.commands = []

    def __call__(self, command, *args, **kwargs):
        self.commands.append(([str(part) for part in command], kwargs))
        return subprocess.CompletedProcess(command, 0, "", "")

    def matching(self, word):
        return [(command, kwargs) for command, kwargs in self.commands if word in " ".join(command)]


class HostTestModesTest(unittest.TestCase):
    def run_mode(self, mode):
        recorder = Recorder()
        canary = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = [root / "tests/storage/pcap_capture_test.cpp", root / "tests/uart/other_test.cpp"]
            with patch.multiple(check, run=recorder, BUILD=root / "build", SOURCES=sources,
                                tool=lambda name: name,
                                node_tests=lambda: [root / "docs/a.test.cjs"],
                                leak_canary=lambda *args: canary.append(args)):
                check.tests(mode)
        return recorder, canary

    def compiles(self, recorder):
        return [command for command, _ in recorder.commands if command[0] == "clang++"]

    def test_asan_mode_uses_both_sanitizers_and_the_canary(self):
        recorder, canary = self.run_mode("asan")
        self.assertEqual(len(canary), 1)
        for command in self.compiles(recorder):
            self.assertIn("-fsanitize=address,undefined", command)
            self.assertIn("-fno-sanitize-recover=all", command)
        runs = [kwargs["env"] for _, kwargs in recorder.commands if "env" in kwargs]
        self.assertEqual(len(runs), 2)
        for environment in runs:
            self.assertEqual(environment["ASAN_OPTIONS"], check.ASAN_OPTIONS)
            self.assertIn("halt_on_error=1", environment["UBSAN_OPTIONS"])
        self.assertFalse(recorder.matching("tcpdump") or recorder.matching("unittest"))

    def test_tsan_mode_uses_thread_sanitizer_only(self):
        recorder, canary = self.run_mode("tsan")
        self.assertEqual(canary, [])
        for command in self.compiles(recorder):
            self.assertIn("-fsanitize=thread", command)
            self.assertNotIn("-fsanitize=address,undefined", command)
        for _, kwargs in recorder.commands:
            if "env" in kwargs:
                self.assertEqual(kwargs["env"]["TSAN_OPTIONS"], "halt_on_error=1")

    def test_test_mode_checks_captures_and_runs_every_suite(self):
        recorder, canary = self.run_mode("test")
        self.assertEqual(canary, [])
        for command in self.compiles(recorder):
            self.assertFalse([flag for flag in command if flag.startswith("-fsanitize")])
        self.assertEqual(len(recorder.matching("tcpdump")), 1)
        (checker, _), = recorder.matching("check_pcap.py")
        self.assertTrue(checker[2].endswith("first.pcap") and "--receipt" in checker)
        self.assertEqual(len(recorder.matching("unittest discover")), 1)
        self.assertEqual(len(recorder.matching("node --test")), 1)
        (_, emulator), = recorder.matching("run_ui_emulator.py")
        self.assertEqual(emulator["env"]["ASAN_OPTIONS"], check.ASAN_OPTIONS)


class FormattingTest(unittest.TestCase):
    def format_command(self, files, fix=False):
        recorder = Recorder()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for name in files:
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).touch()
            with patch.multiple(check, run=recorder, ROOT=root, tool=lambda name: name):
                check.formatting(fix)
            command, _ = recorder.commands[0]
            return [part.replace(str(root) + "/", "") for part in command]

    def test_check_mode_never_rewrites_and_skips_build_output(self):
        files = ["firmware/a.cpp", "flipper/build/b.cpp", "flipper/dist/c.h", "tests/.ufbt/d.h",
                 "tests/e.txt", "tests/f.ino"]
        self.assertEqual(self.format_command(files),
                         ["clang-format", "--dry-run", "--Werror", "firmware/a.cpp", "tests/f.ino"])

    def test_fix_mode_rewrites_in_place(self):
        self.assertEqual(self.format_command(["flipper/a.h"], fix=True),
                         ["clang-format", "-i", "flipper/a.h"])

    def test_no_sources_fails(self):
        with self.assertRaisesRegex(SystemExit, "No C/C\\+\\+ sources"):
            self.format_command([])


class MainSelectionTest(unittest.TestCase):
    def main(self, selected, sources=("a_test.cpp",), skipped=()):
        calls = []
        names = {"formatting": lambda fix=False: calls.append(("format", fix)),
                 "tests": lambda mode: calls.append(("tests", mode)),
                 "tidy": lambda: calls.append("tidy"), "cppcheck": lambda: calls.append("cppcheck"),
                 "analyze": lambda: calls.append("analyze")}
        with patch.multiple(check, SOURCES=[Path(name) for name in sources],
                            unreachable_tests=lambda: [check.ROOT / name for name in skipped],
                            **names), patch.object(sys, "argv", ["check.py", selected]):
            check.main()
        return calls

    def test_all_runs_every_check_without_rewriting_files(self):
        self.assertEqual(self.main("all"), [("format", False), ("tests", "test"), ("tests", "asan"),
                                            ("tests", "tsan"), "tidy", "cppcheck", "analyze"])

    def test_single_checks(self):
        self.assertEqual(self.main("format"), [("format", True)])
        self.assertEqual(self.main("tsan"), [("tests", "tsan")])

    def test_missing_or_skipped_tests_stop_the_gate(self):
        with self.assertRaisesRegex(SystemExit, "No host tests found"):
            self.main("test", sources=())
        with self.assertRaisesRegex(SystemExit, "would skip these tests: tests/x/y_test.cpp"):
            self.main("test", skipped=("tests/x/y_test.cpp",))


class RunnerTest(unittest.TestCase):
    def test_failed_command_stops_the_gate(self):
        with redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                check.run([sys.executable, "-c", "raise SystemExit(3)"])

    def test_real_sources_cover_every_host_test(self):
        tests = sorted(path for path in (check.ROOT / "tests").glob("*/*_test.cpp"))
        self.assertIn(check.EMULATOR_TEST, tests)
        self.assertEqual(check.SOURCES, [path for path in tests if path != check.EMULATOR_TEST])
        self.assertEqual(check.unreachable_tests(), [])

    def test_tool_lookup(self):
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / "fake-tool"
            fake.touch()
            with patch.dict(check.os.environ, {"LLVM_BIN": directory}, clear=True):
                self.assertEqual(check.tool("fake-tool"), str(fake))
            with patch.dict(check.os.environ, {"CXX": "/opt/cxx", "FAKE_TOOL": "/opt/fake"},
                            clear=True):
                self.assertEqual(check.tool("clang++"), "/opt/cxx")
                self.assertEqual(check.tool("fake-tool"), "/opt/fake")
            with patch.dict(check.os.environ, {"PATH": directory}, clear=True):
                with self.assertRaisesRegex(SystemExit, "Missing airrounds-missing-tool"):
                    check.tool("airrounds-missing-tool")


if __name__ == "__main__":
    unittest.main()
