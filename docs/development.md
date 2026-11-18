# Development

The [AirRounds plan](plan.md) lists the steps, dependencies, and acceptance checks.

## Mac tools

We checked these tools on Apple Silicon on November 18, 2026.

| Use | Result |
| --- | --- |
| Git and GitHub | Git 2.56.1 and GitHub CLI 2.104.0 |
| Native builds | Xcode Command Line Tools, Apple Clang 21.0.0, macOS SDK 27.2, and GNU Make 3.81. A C++17 test program built and ran. |
| Python | `PATH` has 3.15.2. uv also has 3.11 through 3.13, and 3.14 is available. Imports of `ssl`, `unittest`, and `venv` passed. |
| ESP32-S2 | `esptool` 5.6.0 is available. Set up Arduino CLI, the ESP32 core, and the Xtensa compiler before building. |
| Flipper Zero | Set up `ufbt` and the matching SDK and ARM compiler before building. |
| PCAP and UART | `tcpdump` read a synthetic PCAP record. `screen` is available. The check did not find `tshark`, Wireshark, or qFlipper. |
| Other build tools | The check did not find PlatformIO, CMake, or Ninja. Use PlatformIO for native tests if the pinned revision calls for it. |
| Emulation | The check did not find QEMU for Xtensa or ARM. Host tests use stand-ins for SD, UART, and time. Use the bench to check the radio, SD card, and firmware. |

## T0 records

1. Record the exact revision of each component and the installed firmware
   versions. Pin the SDKs, dependencies, and a compatible Python version.
2. Check the build command and CLI requirements in the pinned ESP32 workflow.
   The earlier target was a Flipper-specific build with Arduino ESP32 core 2.0.11.
   Install the CLI and ESP32-S2 core that match the chosen revision.
3. Choose `ufbt` and an SDK for the firmware on the Flipper. Check that both run
   on the Mac.
4. Record the two build commands, the artifact SHA-256 hashes, and the test
   results. Keep Mac checks, tests with stand-ins, and bench results separate.

## Check the working tree

```sh
git diff --check
git diff --cached --check
git diff-tree --root --no-commit-id --check HEAD
```

Keep real captures in the local `captures/` directory and build output in
`artifacts/`. Use the [test fixtures](../tests/fixtures/README.md) to generate
synthetic PCAP files in a temporary directory.
