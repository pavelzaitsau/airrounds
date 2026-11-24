#!/usr/bin/env python3
"""Run checks on AirRounds code; SDKs and vendored skills are not lint targets."""

import argparse
import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "quality"
# The UI emulator links the Flipper app and has its own runner; every other
# tests/<folder>/*_test.cpp is a host test.
EMULATOR_TEST = ROOT / "tests/flipper/ui_emulator_test.cpp"
SOURCES = sorted(source for source in (ROOT / "tests").glob("*/*_test.cpp")
                 if source != EMULATOR_TEST)
INCLUDES = [f"-I{ROOT / 'firmware/esp32' / folder}" for folder in ("storage", "session", "uart")]
FLAGS = ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion",
         "-Wsign-conversion", "-Wshadow", "-pthread", *INCLUDES]
# serial_readback_test interposes close() through dlsym; glibc before 2.34 keeps it in libdl.
LINK = ["-ldl"] if sys.platform.startswith("linux") else []


def tool(name):
    variable = "CXX" if name == "clang++" else name.upper().replace("-", "_")
    override = os.environ.get(variable)
    if override:
        return override
    # Homebrew LLVM is keg-only. LLVM_BIN also supports custom/Linux installs.
    for directory in (os.environ.get("LLVM_BIN", ""), "/opt/homebrew/opt/llvm/bin",
                      "/usr/local/opt/llvm/bin"):
        candidate = Path(directory) / name
        if directory and candidate.is_file():
            return str(candidate)
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"Missing {name}; see docs/quality.md")
    return found


# Every command the gate starts runs under these limits. The kernel enforces them, so a test that
# loops or writes without end dies even after the gate exits and leaves it orphaned.
TIMEOUT_SECONDS = 180
CPU_SECONDS = 300
FILE_BYTES = 1 << 30
# Every guard owns a directory; a guard started below another one puts its directory inside the
# enclosing one. Each child creates a file named after its process group in its guard's directory
# before it runs. A guard ending for any reason kills every group in its subtree, so a guard that
# had to kill an inner guard still reaches the inner guard's children.
GROUPS_ENV = "AIRROUNDS_GUARD_DIR"
STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)
KILL_ROUNDS = 20
# A stopped command gets this long after SIGTERM to remove its temporary files.
GRACE_SECONDS = 2


def capped(limit, soft):
    """Lower a limit to `soft`; never ask for more than the inherited hard limit allows."""
    current_soft, hard = resource.getrlimit(limit)
    if hard != resource.RLIM_INFINITY:
        soft = min(soft, hard)
    if current_soft != resource.RLIM_INFINITY:
        soft = min(soft, current_soft)
    resource.setrlimit(limit, (soft, hard))


def limit_child():
    capped(resource.RLIMIT_CPU, CPU_SECONDS)
    capped(resource.RLIMIT_FSIZE, FILE_BYTES)


class Stopped(BaseException):
    """A stop signal arrived; the guard kills its children and the gate exits."""


def stopper():
    """A handler that raises once. CPython can run a second, already caught signal inside the
    guard's cleanup; raising again there would cut the cleanup short."""
    caught = []

    def stop(number, _frame):
        if not caught:
            caught.append(number)
            raise Stopped(number)

    return stop


def signal_groups(groups, number):
    for group in groups:
        try:
            os.killpg(group, number)
        except (ProcessLookupError, PermissionError):
            pass


def registered(directory):
    """Every process group registered in a guard directory and the directories below it."""
    groups = set()
    for _root, _directories, files in os.walk(directory):
        groups.update(int(name) for name in files if name.isdigit())
    return groups


def kill_subtree(directory):
    """Kill every group in the subtree until no new group appears, then remove the subtree."""
    killed = set()
    for _ in range(KILL_ROUNDS):
        groups = registered(directory)
        if groups <= killed:
            break
        signal_groups(groups - killed, signal.SIGKILL)
        killed |= groups
        # A guard killed in the middle of a start leaves a child that registers a moment later.
        time.sleep(0.05)
    shutil.rmtree(directory, ignore_errors=True)


def start_child(directory, mask):
    """Runs in the child before exec: register the new group, then apply the limits."""

    def prepare():
        # Creating the file fails once the guard has removed its directory, so a child that
        # starts after its guard finished exits here instead of running unguarded.
        descriptor = os.open(os.path.join(directory, str(os.getpid())),
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(descriptor)
        limit_child()
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)

    return prepare


def stop_process(process, stopped):
    """End the command's own group: SIGTERM first when the gate is stopping, then SIGKILL."""
    if stopped and process.poll() is None:
        signal_groups([process.pid], signal.SIGTERM)
        try:
            process.wait(timeout=GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            pass
    signal_groups([process.pid], signal.SIGKILL)
    process.wait()


def guarded(command, timeout=TIMEOUT_SECONDS, **kwargs):
    """Run a command under the limits; on any exit, kill its process group and every group below."""
    environment = kwargs.pop("env", None)
    environment = dict(os.environ if environment is None else environment)
    enclosing = os.environ.get(GROUPS_ENV)
    main_thread = threading.current_thread() is threading.main_thread()
    # Stop signals wait while the guard starts or cleans up, so a second signal cannot cut a
    # cleanup short. They arrive only while the command runs.
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, STOP_SIGNALS) if main_thread else set()
    handlers = {}
    process = None
    directory = None
    stopped = False
    try:
        parent = enclosing if enclosing and os.path.isdir(enclosing) else None
        directory = tempfile.mkdtemp(prefix="airrounds-guard-", dir=parent)
        environment[GROUPS_ENV] = directory
        if main_thread:
            # A signal the caller ignores, as `nohup` ignores SIGHUP, stays ignored.
            stop = stopper()
            handlers = {number: signal.signal(number, stop) for number in STOP_SIGNALS
                        if signal.getsignal(number) not in (signal.SIG_IGN, None)}
        process = subprocess.Popen(command, cwd=ROOT, start_new_session=True,
                                   preexec_fn=start_child(directory, mask), env=environment,
                                   **kwargs)
        if main_thread:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        stdout, stderr = process.communicate(timeout=timeout)
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    except BaseException:
        stopped = True
        raise
    finally:
        if main_thread:
            signal.pthread_sigmask(signal.SIG_BLOCK, STOP_SIGNALS)
        # A finished command can leave background children; a stopped one leaves its whole tree.
        if process is not None:
            stop_process(process, stopped)
            try:
                os.unlink(os.path.join(directory, str(process.pid)))
            except OSError:
                pass
        if directory:
            kill_subtree(directory)
        for number, handler in handlers.items():
            signal.signal(number, handler)
        if main_thread:
            # A stop signal that waited now reaches the caller's own handler.
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)


def run(command, **kwargs):
    command = [str(arg) for arg in command]
    print("+ " + shlex.join(command), flush=True)
    result = guarded(command, **kwargs)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, command)


ASAN_OPTIONS = "halt_on_error=1:detect_leaks=1"
LEAK_CANARY = "#include <cstdlib>\nint main() { void* volatile p = std::malloc(64); p = 0; }\n"


def leak_canary(directory, flags, environment):
    # A toolchain without LeakSanitizer would let every leak pass, so prove it reports one.
    source = directory / "leak_canary.cpp"
    binary = directory / "leak_canary"
    source.write_text(LEAK_CANARY)
    run([tool("clang++"), *flags, source, "-o", binary])
    print("+ " + shlex.join([str(binary)]) + "  # expect a reported leak", flush=True)
    result = guarded([str(binary)], timeout=60, env=environment, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True)
    if result.returncode == 0 or "LeakSanitizer: detected memory leaks" not in result.stderr:
        raise SystemExit("LeakSanitizer did not report the canary leak; see docs/quality.md")


def node_tests():
    scripts = sorted(path for folder in ("docs", "tests") for path in (ROOT / folder).rglob("*.test.cjs"))
    if not scripts:
        raise SystemExit("No Node tests found")
    return scripts


def unreachable_tests():
    """Return test files that the gate would not run."""
    missing = [path for path in (ROOT / "tests").rglob("*_test.cpp")
               if path not in SOURCES and path != EMULATOR_TEST]
    # unittest discovery skips a folder without __init__.py and its test modules.
    for path in (ROOT / "tests").rglob("test_*.py"):
        folder = path.parent
        while folder != ROOT and (folder / "__init__.py").is_file():
            folder = folder.parent
        if folder != ROOT:
            missing.append(path)
    return sorted(missing)


def database():
    BUILD.mkdir(parents=True, exist_ok=True)
    commands = [{"directory": str(ROOT), "file": str(source),
                 "arguments": [tool("clang++"), *FLAGS, "-c", str(source)]}
                for source in SOURCES]
    (BUILD / "compile_commands.json").write_text(json.dumps(commands, indent=2) + "\n")


def tests(mode):
    directory = BUILD / mode
    directory.mkdir(parents=True, exist_ok=True)
    flags = ["-O1", "-g", "-fno-omit-frame-pointer"]
    if mode == "asan":
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    elif mode == "tsan":
        flags += ["-fsanitize=thread"]
    environment = os.environ.copy()
    if mode == "asan":
        # LeakSanitizer is off by default on macOS; enable it explicitly.
        environment["ASAN_OPTIONS"] = ASAN_OPTIONS
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        leak_canary(directory, flags, environment)
    elif mode == "tsan":
        environment["TSAN_OPTIONS"] = "halt_on_error=1"
    captures = {
        "pcap_capture_test": ("first.pcap", []),
        "passive_session_test": ("passive.pcap", ["--bssid", "02:00:00:00:00:01"]),
        "capture_mode_test": ("eapol.pcap", ["--eapol", "--bssid", "02:00:00:00:00:01"]),
    }
    for source in SOURCES:
        binary = directory / source.stem
        run([tool("clang++"), *FLAGS, *flags, source, "-o", binary, *LINK])
        # Each execution needs new filenames for the exclusive-create tests.
        with tempfile.TemporaryDirectory(prefix="airrounds-test-") as temporary:
            run([binary, temporary], env=environment)
            if mode == "test" and source.stem in captures:
                filename, options = captures[source.stem]
                capture = Path(temporary) / filename
                run([tool("tcpdump"), "-nn", "-e", "-r", capture])
                run([sys.executable, ROOT / "tools/mac/check_pcap.py", capture,
                     "--receipt", Path(temporary) / (filename + ".ok"), *options])
    if mode == "test":
        # Discovery only enters packages; Python 3.12+ fails when it finds no tests.
        run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", ".",
             "-p", "test_*.py"])
        for script in node_tests():
            run([tool("node"), "--test", script])
        run([sys.executable, ROOT / "tests/flipper/run_ui_emulator.py"],
            env=dict(environment, CXX=tool("clang++"), ASAN_OPTIONS=ASAN_OPTIONS))


def formatting(fix=False):
    # flipper/shared holds generated vendored headers; the drift check owns them, and clang-format
    # would reorder their flattened includes, so leave them out of the formatting set.
    sources = sorted(path for folder in ("firmware", "flipper", "tests")
                     for path in (ROOT / folder).rglob("*")
                     if path.suffix in (".c", ".cpp", ".h", ".ino")
                     and not {"build", "dist", ".ufbt"}.intersection(path.relative_to(ROOT).parts)
                     and path.relative_to(ROOT).parts[:2] != ("flipper", "shared"))
    if not sources:
        raise SystemExit("No C/C++ sources found for formatting")
    run([tool("clang-format"), *(["-i"] if fix else ["--dry-run", "--Werror"]), *sources])


def tidy():
    database()
    for source in SOURCES:
        run([tool("clang-tidy"), "-p", BUILD, source])


def cppcheck():
    # unix32 models ESP32/Flipper integer widths, not either board's peripherals.
    for platform, defines in (("native", []), ("unix32", ["-DARDUINO_ARCH_ESP32"])):
        run([tool("cppcheck"), "--std=c++11", f"--platform={platform}",
             "--enable=warning,style,performance,portability,missingInclude", "--error-exitcode=1",
             "--inline-suppr", "--suppress=missingIncludeSystem",
             # The host tests run their asserts (they undefine NDEBUG), so an assert on a
             # side-effecting call is deliberate, and an assert on a constant input is the check.
             "--suppress=assertWithSideEffect", "--suppress=knownConditionTrueFalse",
             # Cppcheck findings vary by release; the gate was tuned on 2.22.0. An older runner adds
             # false positives the tuned release does not: a non-reentrant readdir in a
             # single-threaded test directory scan, and a container bound and a descriptor leak it
             # cannot track through a test's file-descriptor reuse. unmatchedSuppression keeps these
             # harmless on 2.22.0, where the findings do not fire.
             "--suppress=unmatchedSuppression", "--suppress=readdirCalled",
             "--suppress=containerOutOfBounds:*/tests/storage/pcap_capture_test.cpp",
             "--suppress=resourceLeak:*/tests/storage/sd_writer_test.cpp",
             "--library=posix", *defines, *INCLUDES, *SOURCES])


def analyze():
    BUILD.mkdir(parents=True, exist_ok=True)
    for source in SOURCES:
        run([tool("clang++"), *FLAGS, "--analyze", "-Xanalyzer", "-analyzer-werror",
             source, "-o", BUILD / (source.stem + ".plist")])


def vendored():
    # The catalog builds flipper/ in isolation, so the shared protocol headers are vendored into
    # flipper/shared/. Fail if that copy has drifted from the firmware source of truth.
    run([sys.executable, ROOT / "tools/ci/vendor_flipper_shared.py", "--check"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("all", "format", "format-check", "test", "asan",
                                          "tsan", "tidy", "cppcheck", "analyze", "vendored"))
    selected = parser.parse_args().check
    if not SOURCES:
        raise SystemExit("No host tests found")
    skipped = unreachable_tests()
    if skipped:
        raise SystemExit("The gate would skip these tests: " +
                         ", ".join(str(path.relative_to(ROOT)) for path in skipped))
    checks = {"format": lambda: formatting(True), "format-check": formatting,
              "test": lambda: tests("test"), "asan": lambda: tests("asan"),
              "tsan": lambda: tests("tsan"), "tidy": tidy,
              "cppcheck": cppcheck, "analyze": analyze, "vendored": vendored}
    # Run the vendored drift check first under "all": a stale flipper/shared/ would otherwise make
    # the ui_emulator in "test" fail with a confusing redefinition instead of a clear drift message.
    everything = ["vendored", "format-check", "test", "asan", "tsan", "tidy", "cppcheck", "analyze"]
    for name in (everything if selected == "all" else [selected]):
        checks[name]()


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"Check timed out: {error.cmd}") from None
    except Stopped as error:
        raise SystemExit(128 + error.args[0]) from None
