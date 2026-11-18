#!/usr/bin/env python3
"""Run checks on AirRounds code; SDKs and vendored skills are not lint targets."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "quality"
SOURCES = sorted((ROOT / "tests" / "storage").glob("*_test.cpp"))
FLAGS = ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion",
         "-Wsign-conversion", "-Wshadow", "-pthread", "-I", str(ROOT / "firmware/esp32/storage")]


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
        environment["ASAN_OPTIONS"] = "halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    elif mode == "tsan":
        environment["TSAN_OPTIONS"] = "halt_on_error=1"
    for source in SOURCES:
        binary = directory / source.stem
        run([tool("clang++"), *FLAGS, *flags, source, "-o", binary])
        # Each execution needs new filenames for the exclusive-create tests.
        with tempfile.TemporaryDirectory(prefix="airrounds-test-") as temporary:
            run([binary, temporary], env=environment)
            if mode == "test" and source.stem == "pcap_capture_test":
                run([tool("tcpdump"), "-nn", "-r", Path(temporary) / "first.pcap"])


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
             *defines, "-I", ROOT / "firmware/esp32/storage", *SOURCES])


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
