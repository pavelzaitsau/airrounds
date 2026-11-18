#!/usr/bin/env python3
"""Compile the ESP32 examples and the Flipper app, without flashing devices."""

import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess

from check import tool

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
            libraries += ["--library", ROOT / "firmware/esp32/uart"]
        run([cli, "compile", "--fqbn", fqbn, *libraries,
             "--build-property", f"tools.esptool_py.path={ESP / 'venv/bin'}",
             "--build-property", "tools.esptool_py.cmd=esptool.py",
             "--build-path", ROOT / "build/embedded" / name,
             "--output-dir", destination, sketch], environment)
        verify_image(destination / (name + ".ino.bin"), environment)
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
    print(f"Embedded build artifacts: {OUTPUT}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"Build timed out: {error.cmd}") from None
