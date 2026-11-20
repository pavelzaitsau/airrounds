#!/usr/bin/env python3
"""Compile the ESP32 examples and the Flipper app, without flashing devices."""

import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess

from check import tool
import memory

ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(os.environ.get("AIRROUNDS_TOOLS", ROOT / ".airrounds-tools")).resolve()
ESP = (TOOLS / "arduino").resolve()
FLIPPER = TOOLS / "flipper"
OUTPUT = ROOT / "artifacts" / "quality"


def run(command, environment, cwd=ROOT, capture=False):
    print("+ " + shlex.join(str(arg) for arg in command), flush=True)
    result = subprocess.run([str(arg) for arg in command], cwd=cwd, env=environment,
                            check=True, timeout=600, text=True,
                            stdout=subprocess.PIPE if capture else None)
    if capture:
        print(result.stdout, end="", flush=True)
    return result


def verify_image(path, environment):
    # esptool 4.5.1 can print an invalid checksum/hash and still exit successfully.
    output = run([ESP / "venv/bin/esptool.py", "--chip", "esp32s2", "image_info", path],
                 environment, capture=True).stdout
    for field in ("Checksum", "Validation Hash"):
        if not re.search(rf"^{field}: .+ \(valid\)$", output, re.MULTILINE):
            raise SystemExit(f"Image failed {field} verification: {path}")


def firmware_metadata(environment):
    revision = environment.get("AIRROUNDS_REVISION")
    if revision is None:
        revision = "unknown"
        if (ROOT / ".git").exists():
            result = subprocess.run(["git", "rev-parse", "--show-toplevel", "HEAD"], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10)
            lines = result.stdout.splitlines()
            if result.returncode == 0 and len(lines) == 2 and Path(lines[0]).resolve() == ROOT:
                revision = lines[1]
    if not re.fullmatch(r"[0-9a-f]{40}|unknown", revision):
        raise SystemExit("AIRROUNDS_REVISION must be a full Git revision or unknown")
    fingerprint = hashlib.sha256()
    sources = ROOT / "firmware/esp32"
    for path in sorted(sources.rglob("*")):
        if path.suffix in (".h", ".ino") and path.is_file():
            fingerprint.update(path.relative_to(sources).as_posix().encode() + b"\0")
            fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
    build = fingerprint.hexdigest()[:16]
    print(f"AirRounds source build ID {build}, revision {revision}", flush=True)
    # -fstack-usage only writes a report; it leaves the generated code unchanged.
    return ["--build-property",
            f"compiler.cpp.extra_flags=-DAIRROUNDS_BUILD_ID={build} -DAIRROUNDS_REVISION={revision}"
            " -fstack-usage"]


def flipper_callgraph(application, output):
    """Recompile the Flipper app with stack reports; return the call graph path."""
    database = json.loads((application / ".vscode/compile_commands.json").read_text())
    entry = next((item for item in database if item["file"].endswith("airrounds.cpp")), None)
    if entry is None:
        raise SystemExit("airrounds.cpp is missing from the Flipper compilation database")
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    arguments[arguments.index("-o") + 1] = str(output / "airrounds.o")
    output.mkdir(parents=True, exist_ok=True)
    run([*arguments, "-fstack-usage", "-fcallgraph-info=su"], None, cwd=entry["directory"])
    return output / "airrounds.ci"


def main():
    cli = ESP / "bin/arduino-cli"
    ufbt = FLIPPER / "venv/bin/ufbt"
    for executable in (cli, ESP / "venv/bin/esptool.py", ufbt):
        if not executable.is_file():
            raise SystemExit(f"Missing {executable}; follow docs/t0.md or set AIRROUNDS_TOOLS")
    environment = os.environ.copy()
    environment.update({
        "ARDUINO_DIRECTORIES_DATA": str(ESP / "data"),
        "ARDUINO_DIRECTORIES_DOWNLOADS": str(ESP / "downloads"),
        "ARDUINO_DIRECTORIES_USER": str(ESP / "user"),
        "UFBT_HOME": str(FLIPPER / "ufbt"),
        "PATH": str(ESP / "venv/bin") + os.pathsep + environment["PATH"],
    })
    cppcheck_flags = ["--platform=unix32", "--enable=warning,performance,portability,missingInclude",
                      "--error-exitcode=1", "--inline-suppr", "--suppress=missingIncludeSystem"]
    sketches = sorted({path.parent for path in (ROOT / "tests/toolchain/esp32").glob("*/*.ino")})
    if not sketches:
        raise SystemExit("No ESP32 examples found")
    sketches.append(ROOT / "firmware/esp32/airrounds_uart")
    for sketch in sketches:
        name = sketch.name
        destination = OUTPUT / "esp32" / name
        destination.mkdir(parents=True, exist_ok=True)
        fqbn = "esp32:esp32:esp32s2"
        libraries = ["--library", ROOT / "firmware/esp32/storage"]
        if name in ("airrounds_passive_smoke", "airrounds_uart"):
            fqbn += ":CDCOnBoot=cdc"
            libraries += ["--library", ROOT / "firmware/esp32/session"]
        if name == "airrounds_uart":
            libraries += ["--library", ROOT / "firmware/esp32/uart", *firmware_metadata(environment)]
        compiled = run([cli, "compile", "--fqbn", fqbn, *libraries,
                        "--build-property", f"tools.esptool_py.path={ESP / 'venv/bin'}",
                        "--build-property", "tools.esptool_py.cmd=esptool.py",
                        "--build-path", ROOT / "build/embedded" / name,
                        "--output-dir", destination, sketch], environment, capture=True)
        verify_image(destination / (name + ".ino.bin"), environment)
        if name == "airrounds_uart":
            memory.esp32_memory(compiled.stdout, ROOT / "build/embedded" / name / "sketch" /
                                (name + ".ino.cpp.su"))
        # Review project code with the actual SDK defines/include paths. These two
        # upstream Arduino header diagnostics are outside AirRounds' source scope.
        run([tool("cppcheck"), *cppcheck_flags,
             f"--project={ROOT / 'build/embedded' / name / 'compile_commands.json'}",
             f"--file-filter=*/sketch/{name}.ino.cpp",
             f"--suppress=missingInclude:{ESP / 'data'}/*",
             f"--suppress=returnTempReference:{ESP / 'data'}/*",
             f"--suppress=duplInheritedMember:{ESP / 'data'}/*"], environment)
    application = ROOT / "flipper"
    run([ufbt], environment, cwd=application)
    destination = OUTPUT / "flipper"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(application / "dist/airrounds.fap", destination)
    # Match the pinned ARM GCC 12.3 SDK and newlib's printf attribute macro.
    # CMSIS uses linker-defined table boundaries that Cppcheck sees as objects.
    run([tool("cppcheck"), *cppcheck_flags,
         f"--project={application / '.vscode/compile_commands.json'}",
         "-D__GNUC__=12", "-D__GNUC_MINOR__=3", "-D_ATTRIBUTE(x)=__attribute__(x)",
         f"--suppress=missingInclude:{FLIPPER / 'ufbt/current/sdk_headers'}/*",
         f"--suppress=comparePointers:{FLIPPER / 'ufbt/current/sdk_headers/f7_sdk/lib/cmsis_core/cmsis_gcc.h'}"],
        environment)
    memory.flipper_stack(flipper_callgraph(application, ROOT / "build/embedded/flipper-stack"),
                         application / "application.fam")
    print(f"Embedded build artifacts: {OUTPUT}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"Build timed out: {error.cmd}") from None
