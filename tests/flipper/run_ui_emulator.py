#!/usr/bin/env python3
"""Run the native app logic against simulated Flipper services and Devboard UART."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        raise SystemExit("Install clang++ or set CXX to a C++ compiler.")
    with tempfile.TemporaryDirectory(prefix="airrounds-ui-") as work:
        sdk = Path(work)
        for name in (
            "expansion/expansion.h", "furi.h", "furi_hal_random.h", "furi_hal_serial.h",
            "furi_hal_serial_control.h",
            "gui/elements.h", "gui/gui.h", "gui/modules/byte_input.h", "gui/view_dispatcher.h",
        ):
            stub = sdk / name
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.touch()
        binary = sdk / "ui-emulator"
        subprocess.run([
            cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(sdk), "-I", str(HERE), "-include", str(HERE / "emulator_sdk.h"),
            str(ROOT / "flipper/airrounds.cpp"), str(HERE / "ui_emulator_test.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
