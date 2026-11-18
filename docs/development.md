# Development

The [AirRounds plan](plan.md) lists the steps, dependencies, and acceptance checks.

## Mac tools

We checked these tools on Apple Silicon on November 18, 2026. The
[T0 report](t0.md) records SDK setup, test build commands, and artifact hashes.

| Use | Result |
| --- | --- |
| Git and GitHub | Git 2.56.1 and GitHub CLI 2.104.0 |
| Native builds | Xcode Command Line Tools, Apple Clang 21.0.0, macOS SDK 27.2, and GNU Make 3.81. A C++17 test program built and ran. |
| Python | `PATH` has 3.15.2. uv also has 3.11 through 3.13, and 3.14 is available. Imports of `ssl`, `unittest`, and `venv` passed. |
| ESP32-S2 | The local toolchain has Arduino CLI 1.6.0, ESP32 core 2.0.11, Xtensa GCC 8.4.0, and Python esptool 4.5.1. The test build produced a `.bin`. |
| Flipper Zero | The local toolchain has ufbt 0.2.6, SDK f7 1.5.1 (API 88.2), and ARM GCC 12.3.1. The test build produced a `.fap`. |
| PCAP and UART | `tcpdump` read a synthetic PCAP record. `screen` is available. The check did not find `tshark`, Wireshark, or qFlipper. |
| Other build tools | The check did not find PlatformIO, CMake, or Ninja. Use PlatformIO for native tests if the pinned revision calls for it. |
| Emulation | The check did not find QEMU for Xtensa or ARM. Host tests use stand-ins for SD, UART, and time. Use the bench to check the radio, SD card, and firmware. |

## Build status

Both toolchains produce test artifacts. Builds of the working components and
compatibility with the firmware on the bench still need checking. Record Mac
and hardware results separately.

## Automated quality checks

See [quality checks](quality.md) for `make check`, sanitizer runs, and embedded
builds. Record host results separately from tests run on physical devices.

## Require signatures on pushes to main

Set up commit signing first. For SSH signatures, put the trusted public
signing keys in the file set by `gpg.ssh.allowedSignersFile`.
Run `git verify-commit HEAD` to check that Git trusts your signing key.

Install the hook once per clone.

```sh
hook=$(git rev-parse --git-path hooks/pre-push)
test ! -e "$hook" && install -m 755 .githooks/pre-push "$hook"
```

If you already have a pre-push hook, add this check to it. The hook checks
the signature of every commit the push adds to the remote `main` branch.

## Check the working tree

```sh
git diff --check
git diff --cached --check
git diff-tree --root --no-commit-id --check HEAD
```

Keep real captures in the local `captures/` directory and build output in
`artifacts/`. Use the [test fixtures](../tests/fixtures/README.md) to generate
synthetic PCAP files in a temporary directory.
