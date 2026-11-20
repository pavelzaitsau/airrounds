from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
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
                    patch.object(check, "guarded", return_value=result) as runner:
                with redirect_stdout(io.StringIO()):
                    check.leak_canary(Path(directory), [], {})
                self.assertIn("leak_canary", runner.call_args.args[0][0])
                options = runner.call_args.kwargs
                self.assertEqual((options["stdout"], options["stderr"], options["text"]),
                                 (subprocess.PIPE, subprocess.PIPE, True))

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




class GuardTest(unittest.TestCase):
    """The gate must not leave a hung test running or let a test fill the disk."""

    TOOLS = str(Path(check.__file__).parent)

    def guard_directories(self):
        """Guard directories where this test's guards create theirs: below the gate, or in TMPDIR."""
        root = os.environ.get(check.GROUPS_ENV) or tempfile.gettempdir()
        return {name for name in os.listdir(root) if name.startswith("airrounds-guard-")}

    def setUp(self):
        before = self.guard_directories()
        # Runs after the per-test cleanups, so a test that leaves its guard directory fails.
        self.addCleanup(lambda: self.assertEqual(self.guard_directories() - before, set(),
                                                 "a guard left its directory behind"))

    def alive(self, pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        return True

    def wait_dead(self, pid):
        for _ in range(50):
            if not self.alive(pid):
                return True
            time.sleep(0.1)
        return False

    def marker(self, name):
        """A file for the PID of a `sleep 300` that a test starts; cleanup kills it on failure."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / name

        def kill_leftover():
            try:
                os.kill(int(path.read_text()), signal.SIGKILL)
            except (OSError, ValueError):
                pass

        self.addCleanup(kill_leftover)
        return path

    def gate_script(self, marker, body):
        """Python that imports the gate and runs `body`; `script` writes a sleep's PID first."""
        return ("import signal, sys\n"
                f"sys.path.insert(0, {self.TOOLS!r})\n"
                "import check\n"
                f"script = 'sleep 300 & echo $! > {marker}; wait'\n" + body)

    def wait_for(self, marker):
        for _ in range(100):
            if marker.exists() and marker.read_text().strip():
                return int(marker.read_text())
            time.sleep(0.1)
        self.fail("the command did not start")

    def test_timeout_kills_the_whole_process_group(self):
        marker = self.marker("orphan.pid")
        with self.assertRaises(subprocess.TimeoutExpired):
            check.guarded(["/bin/sh", "-c", f"sleep 300 & echo $! > {marker}; wait"], timeout=1)
        self.assertTrue(self.wait_dead(self.wait_for(marker)), "the grandchild outlived the timeout")

    def test_finished_command_leaves_no_background_child(self):
        marker = self.marker("background.pid")
        result = check.guarded(["/bin/sh", "-c", f"sleep 300 & echo $! > {marker}"])
        self.assertEqual(result.returncode, 0)
        self.assertTrue(self.wait_dead(self.wait_for(marker)),
                        "a background child outlived its finished test")

    def test_outer_timeout_kills_a_nested_guard_tree(self):
        marker = self.marker("nested.pid")
        script = self.gate_script(marker, "check.run(['/bin/sh', '-c', script])\n")
        with self.assertRaises(subprocess.TimeoutExpired):
            check.guarded([sys.executable, "-c", script], timeout=3, stdout=subprocess.DEVNULL)
        self.assertTrue(self.wait_dead(self.wait_for(marker)),
                        "a command under a nested guard outlived the outer timeout")

    def test_stop_signals_kill_the_children(self):
        for number in check.STOP_SIGNALS:
            with self.subTest(signal=number):
                marker = self.marker(f"stopped-{number}.pid")
                body = ("try:\n"
                        "    check.run(['/bin/sh', '-c', script])\n"
                        "except check.Stopped as error:\n"
                        "    sys.exit(128 + error.args[0])\n")
                gate = subprocess.Popen([sys.executable, "-c", self.gate_script(marker, body)],
                                        stdout=subprocess.DEVNULL, start_new_session=True)
                self.addCleanup(lambda process=gate: process.poll() is None and process.kill())
                child = self.wait_for(marker)
                gate.send_signal(number)
                self.assertEqual(gate.wait(timeout=10), 128 + number)
                self.assertTrue(self.wait_dead(child), "a child outlived the stop signal")

    def test_ignored_hangup_stays_ignored(self):
        marker = self.marker("nohup.pid")
        body = ("signal.signal(signal.SIGHUP, signal.SIG_IGN)\n"
                f"check.run(['/bin/sh', '-c', 'echo $$ > {marker}; sleep 1'])\n")
        gate = subprocess.Popen([sys.executable, "-c", self.gate_script(marker, body)],
                                stdout=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(lambda: gate.poll() is None and gate.kill())
        self.wait_for(marker)
        gate.send_signal(signal.SIGHUP)
        self.assertEqual(gate.wait(timeout=10), 0, "an ignored SIGHUP stopped the gate")

    def test_finished_guard_leaves_the_shared_directory_empty(self):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory, True)
        with patch.dict(os.environ, {check.GROUPS_ENV: directory}):
            check.guarded(["/usr/bin/true"])
        self.assertEqual(os.listdir(directory), [], "a finished guard stayed registered")

    def test_late_child_does_not_run_unguarded(self):
        # A child of a guard whose outermost guard is gone must fail before it runs.
        missing = Path(tempfile.mkdtemp()) / "gone"
        self.addCleanup(shutil.rmtree, missing.parent, True)
        with self.assertRaises(subprocess.SubprocessError):
            subprocess.Popen(["/usr/bin/true"], start_new_session=True,
                             preexec_fn=check.start_child(str(missing), set())).wait()

    def test_nested_guard_lives_inside_the_enclosing_one(self):
        body = f"print(__import__('os').environ[{check.GROUPS_ENV!r}])"
        script = self.gate_script(self.marker("unused.pid"),
                                  f"check.run([sys.executable, '-c', {body!r}])\n")
        result = check.guarded([sys.executable, "-c", script], stdout=subprocess.PIPE, text=True)
        inner = Path(result.stdout.split()[-1])
        self.assertEqual(result.returncode, 0)
        self.assertTrue(inner.name.startswith("airrounds-guard-"))
        self.assertTrue(inner.parent.name.startswith("airrounds-guard-"),
                        "the inner guard did not put its directory inside the outer one")
        self.assertFalse(inner.parent.exists(), "the outer guard left its directory")

    def test_stopped_command_gets_sigterm_first(self):
        # SIGTERM gives a command such as the emulator runner time to remove its temporary files.
        marker = self.marker("term.txt")
        script = ("import signal, sys, time\n"
                  "def done(*_):\n"
                  f"    open({str(marker)!r}, 'w').write('term')\n"
                  "    sys.exit(0)\n"
                  "signal.signal(signal.SIGTERM, done)\n"
                  "print('ready', flush=True)\n"
                  "time.sleep(300)\n")
        with self.assertRaises(subprocess.TimeoutExpired):
            check.guarded([sys.executable, "-c", script], timeout=2, stdout=subprocess.DEVNULL)
        self.assertEqual(marker.read_text(), "term")

    def test_child_starts_with_no_blocked_signal(self):
        script = "import signal; print(sorted(signal.pthread_sigmask(signal.SIG_BLOCK, [])))"
        result = check.guarded([sys.executable, "-c", script], stdout=subprocess.PIPE, text=True)
        self.assertEqual(result.stdout.strip(), "[]")

    def test_second_stop_signal_during_cleanup(self):
        # The command is a nested guard, so the cleanup takes several rounds and both kinds of
        # protection matter: the blocked signals and the handler that raises only once.
        for delay in (0.0, 0.0001, 0.001, 0.02, 0.06, 0.1, 0.3):
            with self.subTest(delay=delay):
                marker = self.marker(f"twice-{delay}.pid")
                inner = self.gate_script(marker, "check.run(['/bin/sh', '-c', script])\n")
                body = ("try:\n"
                        f"    check.run([sys.executable, '-c', {inner!r}])\n"
                        "except check.Stopped as error:\n"
                        "    sys.exit(128 + error.args[0])\n")
                gate = subprocess.Popen([sys.executable, "-c", self.gate_script(marker, body)],
                                        stdout=subprocess.DEVNULL, start_new_session=True)
                self.addCleanup(lambda process=gate: process.poll() is None and process.kill())
                child = self.wait_for(marker)
                gate.send_signal(signal.SIGTERM)
                time.sleep(delay)
                gate.send_signal(signal.SIGHUP)
                self.assertIn(gate.wait(timeout=20), (128 + signal.SIGTERM, -signal.SIGHUP,
                                                      128 + signal.SIGHUP))
                self.assertTrue(self.wait_dead(child), "a second signal cut the cleanup short")

    def test_stop_signal_during_cleanup_waits_for_it(self):
        # A signal that arrives while a finished command is being cleaned up must not cut the
        # cleanup short; it reaches the caller's handler afterwards.
        caught = []
        previous = signal.signal(signal.SIGTERM, lambda number, _frame: caught.append(number))
        self.addCleanup(signal.signal, signal.SIGTERM, previous)
        marker = self.marker("cleanup.pid")
        real_stop = check.stop_process

        def signal_then_stop(process, stopped):
            os.kill(os.getpid(), signal.SIGTERM)
            real_stop(process, stopped)

        with patch.object(check, "stop_process", signal_then_stop):
            result = check.guarded(["/bin/sh", "-c", f"sleep 300 & echo $! > {marker}"])
        self.assertEqual(result.returncode, 0)
        self.assertTrue(self.wait_dead(self.wait_for(marker)), "the cleanup was cut short")
        self.assertEqual(caught, [signal.SIGTERM], "the waiting signal was lost")

    def test_child_cannot_write_past_the_file_limit(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(check, "FILE_BYTES", 1 << 20):
            target = Path(directory) / "big"
            script = (f"import os\nwith open({str(target)!r}, 'wb') as f:\n"
                      "    f.write(b'0' * (4 << 20))\n")
            result = check.guarded([sys.executable, "-c", script], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)
            self.assertLessEqual(target.stat().st_size, 1 << 20)

    def test_child_cannot_spin_past_the_cpu_limit(self):
        with patch.object(check, "CPU_SECONDS", 1):
            result = check.guarded([sys.executable, "-c", "while True: pass"], timeout=30,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)

    def test_lower_inherited_limits_are_kept(self):
        # Under a shell `ulimit` lower than the gate's, commands start and keep the lower limit.
        script = ("import resource, subprocess, sys\n"
                  f"sys.path.insert(0, {self.TOOLS!r})\n"
                  "import check\n"
                  "def lower(limit, value):\n"
                  "    soft, hard = resource.getrlimit(limit)\n"
                  "    for bound in (soft, hard):\n"
                  "        if bound != resource.RLIM_INFINITY:\n"
                  "            value = min(value, bound)\n"
                  "    resource.setrlimit(limit, (value, value))\n"
                  "    return value\n"
                  "cpu = lower(resource.RLIMIT_CPU, 100)\n"
                  "size = lower(resource.RLIMIT_FSIZE, 512000)\n"
                  "result = check.guarded([sys.executable, '-c', "
                  "'import resource; print(*resource.getrlimit(resource.RLIMIT_CPU), "
                  "*resource.getrlimit(resource.RLIMIT_FSIZE))'], stdout=subprocess.PIPE, "
                  "text=True)\n"
                  "print(result.returncode, result.stdout.strip(), cpu, size)\n")
        output = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True,
                                timeout=60, check=True).stdout.split()
        code, cpu_soft, cpu_hard, size_soft, size_hard, cpu, size = output
        self.assertEqual(code, "0")
        self.assertEqual((cpu_soft, cpu_hard, size_soft, size_hard), (cpu, cpu, size, size))


if __name__ == "__main__":
    unittest.main()
