#!/usr/bin/env bash
# Provision the AirRounds embedded toolchain under .airrounds-tools for a Linux x64 CI runner.
#
# It mirrors the macOS setup in docs/t0.md at the pinned versions: Arduino CLI 1.6.0, ESP32 core
# 2.0.11 (which pulls the Linux Xtensa GCC), Python esptool 4.5.1, ufbt 0.2.6, and the verified f7
# SDK 1.5.1 (which pulls the Linux ARM GCC). make embedded reads AIRROUNDS_TOOLS and sets its own
# PATH and Arduino directories, so this script only has to populate the tree.
set -euo pipefail

root="${1:-$PWD}"
tools="${AIRROUNDS_TOOLS:-$root/.airrounds-tools}"
esp="$tools/arduino"
flipper="$tools/flipper"
mkdir -p "$esp/bin" "$flipper"

if ! command -v uv >/dev/null 2>&1; then
    curl -LsSf https://astral.sh/uv/install.sh | sh
    export PATH="$HOME/.local/bin:$PATH"
fi

# Arduino CLI 1.6.0 and the ESP32 core 2.0.11.
curl -fsSL "https://raw.githubusercontent.com/arduino/arduino-cli/v1.6.0/install.sh" \
    | BINDIR="$esp/bin" sh -s 1.6.0
export ARDUINO_DIRECTORIES_DATA="$esp/data"
export ARDUINO_DIRECTORIES_DOWNLOADS="$esp/downloads"
export ARDUINO_DIRECTORIES_USER="$esp/user"
index="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
"$esp/bin/arduino-cli" core update-index --additional-urls "$index"
"$esp/bin/arduino-cli" core install esp32:esp32@2.0.11 --additional-urls "$index"

# esptool 4.5.1 in its own virtual environment, as make embedded expects.
UV_CACHE_DIR="$esp/uv-cache" uv venv --python 3.12 "$esp/venv"
UV_CACHE_DIR="$esp/uv-cache" uv pip install --python "$esp/venv/bin/python" esptool==4.5.1

# ufbt 0.2.6 and the verified f7 SDK 1.5.1.
export UFBT_HOME="$flipper/ufbt"
UV_CACHE_DIR="$flipper/uv-cache" uv venv --python 3.12 "$flipper/venv"
UV_CACHE_DIR="$flipper/uv-cache" uv pip install --python "$flipper/venv/bin/python" ufbt==0.2.6
archive="$flipper/flipper-z-f7-sdk-1.5.1.zip"
curl -fsSL -o "$archive" \
    "https://update.flipperzero.one/builds/firmware/1.5.1/flipper-z-f7-sdk-1.5.1.zip"
echo "dfde39df32ba8a9789b26afc1d702579ad8399a1c285770637cfe3287b2e2ffe  $archive" | sha256sum -c
"$flipper/venv/bin/ufbt" update --hw-target f7 --local "$archive"

echo "AirRounds embedded toolchain ready under $tools"
