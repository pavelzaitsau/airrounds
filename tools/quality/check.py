#!/usr/bin/env python3
"""Run checks on AirRounds code; SDKs and vendored skills are not lint targets."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

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


def run(command, **kwargs):
    command = [str(arg) for arg in command]
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, timeout=180, **kwargs)


ASAN_OPTIONS = "halt_on_error=1:detect_leaks=1"
LEAK_CANARY = "#include <cstdlib>\nint main() { void* volatile p = std::malloc(64); p = 0; }\n"


def leak_canary(directory, flags, environment):
    # A toolchain without LeakSanitizer would let every leak pass, so prove it reports one.
    source = directory / "leak_canary.cpp"
    binary = directory / "leak_canary"
    source.write_text(LEAK_CANARY)
    run([tool("clang++"), *flags, source, "-o", binary])
    print("+ " + shlex.join([str(binary)]) + "  # expect a reported leak", flush=True)
    result = subprocess.run([str(binary)], cwd=ROOT, env=environment, capture_output=True,
                            text=True, timeout=60, check=False)
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
    sources = sorted(path for folder in ("firmware", "flipper", "tests")
                     for path in (ROOT / folder).rglob("*")
                     if path.suffix in (".c", ".cpp", ".h", ".ino")
                     and not {"build", "dist", ".ufbt"}.intersection(path.relative_to(ROOT).parts))
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
             "--inline-suppr", "--suppress=missingIncludeSystem", "--library=posix",
             *defines, *INCLUDES, *SOURCES])


def analyze():
    BUILD.mkdir(parents=True, exist_ok=True)
    for source in SOURCES:
        run([tool("clang++"), *FLAGS, "--analyze", "-Xanalyzer", "-analyzer-werror",
             source, "-o", BUILD / (source.stem + ".plist")])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("all", "format", "format-check", "test", "asan",
                                          "tsan", "tidy", "cppcheck", "analyze"))
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
              "cppcheck": cppcheck, "analyze": analyze}
    for name in (checks if selected == "all" else [selected]):
        if selected == "all" and name == "format":
            continue
        checks[name]()


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"Check timed out: {error.cmd}") from None
