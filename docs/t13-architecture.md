# T13 architecture and code plan

This document plans the firmware, the Flipper app, the protocol, and the CI
changes for [board flashing, self-test, and the CI release](t13.md), and the
order to build them. It writes no code. The requirements are in [T13](t13.md).

The four workstreams split into three code areas and one pipeline: a protocol and
firmware change for the self-test and the microSD gate, a Flipper flasher that
speaks the ESP32-S2 ROM loader, and a GitHub Actions release. The flasher is the
one new subsystem; the rest extend existing modules.

## Two protocols on one wire

The Flipper UART carries two unrelated protocols, never at the same time.

- The AirRounds ASCII protocol in [uart.md](uart.md) runs while the board runs
  AirRounds firmware. The self-test and the microSD gate extend it.
- The ESP32-S2 ROM serial loader, a binary SLIP protocol, runs only during a
  flash, after the Flipper resets the board into the ROM bootloader.

A flash suspends the ASCII protocol. The flow resets the board into the
bootloader, flashes, resets back into the application, and then sends `hello` to
confirm the new firmware version. The two protocols never share a session.

## Protocol change: feature bit 6

The self-test and the card status ride on a new feature bit, so an older board
without the bit reports both as unsupported.

| Item | Rule |
| --- | --- |
| Feature bit | Bit 6 supports the board self-test and the microSD status. The `features` field already holds 8 bits; bit 6 is the next free one after bit 5. |
| `card` command | Scope zero, a read request ID with the top bit set. It returns the mount state fast, with no card write. |
| `selftest` command | Scope zero, a read request ID with the top bit set. It runs the full self-test, which writes and removes a test PCAP. |
| Older board | A board without bit 6 answers `card` and `selftest` with `bad_request`. The app shows the self-test as unavailable and skips the card gate. |

The `card` reply appends `card <state>`: `0` for absent or unmountable, `1` for
mounted. The `selftest` reply appends `selftest <sd><radio><probe><write>`, four
one-character results: `p` pass, `f` fail, `b` blocked, `-` not run. A failed
microSD mount reports the test PCAP write as `b`. Both replies fit the 239-byte
buffer.

## Firmware: self-test and the card gate

Two firmware changes, both in `firmware/esp32/`, both receive-only.

- A new `firmware/esp32/uart/selftest.h` runs the four checks in order: a microSD
  mount, a radio init, a short receive probe, and a test PCAP write that the board
  reopens and verifies. It reuses `PcapCapture` for the write and the verify, and
  removes the test file after the check. A failed check blocks the checks that
  depend on it.
- `airrounds_uart.ino` dispatches `card` and `selftest` and sets feature bit 6.
  The `card` handler probes the mount without a write.
- The round preflight closes the [T12 gap][t12-gap]: `PassiveRound` checks the
  card mount before it opens the radio, and reports `storage_error` before the
  round, not at save time. The board already writes a checkpoint before listening;
  the preflight adds an explicit early mount probe with its own result.

The self-test transmits nothing. The receive-only source gate
`tests/mac/test_receive_only.py` stays green, because `selftest.h` lives under
`firmware/esp32/` and names no transmit token.

## Flipper flasher

A new `flipper/esp_flasher.h` drives the ESP32-S2 ROM serial loader over the
Flipper UART. It is Flipper-side only and adds nothing to the firmware.

### Bootloader entry

The Flipper drives two GPIO lines to enter the ROM bootloader: IO0 low, then an
EN low-to-high pulse, then IO0 released. After a verified flash it drives IO0
high and pulses EN to boot the application.

The pin assignment is the open hardware item. The flasher reserves two spare
Flipper GPIO through `furi_hal_gpio_init` and `furi_hal_gpio_write`. The bench
confirms that the Devboard routes IO0 and EN to the Flipper header. If it routes
neither, the flasher falls back to an on-screen BOOT-and-RESET prompt and the
operator holds the buttons. See [risks](#risks).

### ROM loader sequence

The wire format is the public ESP32-S2 ROM serial loader, for which `esptool` is
the reference. The flasher implements the raw ROM loader, with no stub and no
compression.

| Step | Command | Content |
| --- | --- | --- |
| Sync | `SYNC` `0x08` | The sync frame, retried until the ROM answers. |
| Baud | `CHANGE_BAUDRATE` `0x0f` | Raise the rate to 460800 after sync, for throughput. The bench tunes it. |
| Begin | `FLASH_BEGIN` `0x02` | The erase size, the packet count, and the packet size, at offset `0x0`. |
| Write | `FLASH_DATA` `0x03` | One SLIP-framed packet per block, with the sequence number and the per-packet checksum. The ROM block size is 1024 bytes. |
| End | `FLASH_END` `0x04` | Finish the write, with the reboot flag clear. |
| Verify | `SPI_FLASH_MD5` `0x13` | The MD5 of the written region, compared to the manifest MD5. |

The flasher streams the release image from the Flipper microSD block by block,
so the Flipper never holds the whole image in RAM. A `FLASH_DATA` failure
or an MD5 mismatch fails the flash and keeps the error on screen. The board stays
in the bootloader after a failed write, reflashable on a retry.

### Image on the Flipper card

The release ships the image and the manifest to a fixed app data path:

- `/ext/apps_data/airrounds/airrounds.bin`: the merged image.
- `/ext/apps_data/airrounds/airrounds.manifest`: the manifest JSON.

The flasher reads the path through the `APP_DATA_PATH` macro. A missing image or
a manifest mismatch stops the flash before any GPIO drive, with a named error.

## Flipper app and the menus

`flipper/airrounds.cpp` wires the two empty scan-menu entries, and the app gates
capture on the card.

- **Test board** calls `selftest` through a new `session_client` wrapper and
  shows a row per check with a pass or fail label. An older board without feature
  bit 6 shows the entry as unavailable.
- **Flash board hardware** runs the flash flow: `esp_flasher`, then the progress
  and the terminal state. When the image on the card is an active build, the flow
  first shows the active-mode attestation prompt, and the operator confirms the
  active limits before the Flipper drives any GPIO. A passive image skips the
  prompt.
- The card gate reads `card` before the app enters Scan or Capture. State `0`
  blocks both with an explicit "no card" error and leaves Flash board and Test
  board available.

The attestation prompt is new to T13 and specific to an active flash. The
[T12 descriptor](t12-architecture.md#flipper-descriptor) kept no on-device
attestation, because the operator attestation was a policy step. An active build
flashes a transmit image, so the flash flow adds the on-device confirm. The
public passive release never shows it.

## CI release

A GitHub Actions workflow in `.github/workflows/` builds both gates and the
release on a Linux x64 runner.

### Linux toolchain

The toolchain in `.airrounds-tools` is arm64 macOS only. A setup step installs
the Linux x64 equivalents at the pinned versions: Arduino CLI 1.6.0, ESP32 core
2.0.11, Xtensa GCC 8.4.0, Python esptool 4.5.1, and `ufbt` with the f7 SDK 1.5.1.
`tools/quality/check.py` already selects `-ldl` on Linux and reads `AIRROUNDS_TOOLS`,
so the gate runner needs no source change.

### Build and package

- Run `make check`, `make asan`, and `make tsan` on the runner's LLVM.
- Run `make embedded`, which builds the passive ESP image and the `.fap`. The
  active image builds only in the opt-in `-DAIRROUNDS_ACTIVE=1` build, outside the
  public release.
- Merge the image offsets into one file at offset `0x0`:

  ```bash
  esptool.py --chip esp32s2 merge_bin -o airrounds.bin \
    0x1000 bootloader.bin 0x8000 partitions.bin \
    0xe000 boot_app0.bin 0x10000 airrounds.ino.bin
  ```

  The merge pads the gaps with `0xFF` and sizes the file to the last segment, so
  the image is about 780 KB, not the full 4 MB. The bench confirms the offsets
  against the core 2.0.11 build output.
- Write the manifest and publish the `.fap`, the merged image, and the manifest
  to a GitHub release on a `v*` tag.

### Manifest

The manifest is JSON. The flasher reads `image.md5` for the verify and `variant`
to decide the active-mode attestation, and the About screen reads the versions.
A `passive` variant skips the attestation; an `active` variant shows it.

```json
{
  "build_id": "<16 hex>",
  "revision": "<40 hex git sha>",
  "fap_version": "0.3",
  "firmware_version": "0.3",
  "variant": "passive",
  "image": { "file": "airrounds.bin", "offset": 0,
             "size": 798400, "sha256": "<64 hex>", "md5": "<32 hex>" }
}
```

## Code steps

Build in this order. Each step lands with its host tests and keeps every memory
gate green.

| Step | Depends on | Content |
| --- | --- | --- |
| B1 | None | Protocol and firmware: feature bit 6, the `card` and `selftest` commands, `firmware/esp32/uart/selftest.h`, the `airrounds_uart.ino` dispatch, and the `uart.md` wire-format update. Tests: the four self-test results, the blocked-on-SD rule, the `card` states, and an older board answering `bad_request`. |
| B2 | B1 | microSD gate: the `PassiveRound` early mount preflight, and the app card gate on `card` before Scan and Capture. Tests: a preflight `storage_error` before the radio opens, and the UI emulator blocking capture with no card. |
| B3 | None | Flipper flasher: `flipper/esp_flasher.h` with the ROM loader, the SLIP framing, the GPIO boot and reset, the SD streaming, and the `SPI_FLASH_MD5` verify. Tests: the SLIP encode and decode, the checksum, and the manifest parse on the host. |
| B4 | None | CI release: the Linux toolchain setup, the `merge_bin` step, the manifest writer, and the `.github/workflows/` release on a `v*` tag. |
| B5 | B1, B3, B4 | Menus: **Test board** drives `selftest`; **Flash board hardware** runs the flash and the progress, and the active-mode attestation prompt when the manifest `variant` is active. Tests: the self-test rows, the flash states, and the attestation gate in the UI emulator. |
| B6 | B1 to B5 | Acceptance: `make check`, `make asan`, `make tsan`, and `make embedded` pass; the CI release publishes the artifacts; a bench run flashes the board from the Flipper, passes the self-test, and captures a handshake on an own WPA2-PSK network. The [T13 acceptance criteria](t13.md#acceptance-criteria). |

## Memory

The firmware change adds the self-test, which reuses `PcapCapture` and its
existing buffers, so the ESP32 RAM budget barely moves; `make embedded` is the
gate. The flasher is Flipper-side: it streams the image in 1024-byte blocks, so
its RAM is one block plus the SLIP buffer. The `fap` stack is 4096 bytes today;
B3 confirms the flasher fits it or bumps `stack_size` in
[application.fam](../flipper/application.fam).

## Risks

- **IO0 and EN wiring.** Automatic bootloader entry needs the Flipper to control
  both lines. If the Devboard routes neither to the header, the flasher falls back
  to a manual BOOT-and-RESET prompt. The bench confirms the wiring before B5.
- **ROM loader over the Flipper UART.** The raised baud may be unreliable on the
  Flipper serial. The flasher starts at 115200, raises the rate only after a
  successful sync, and drops back on a sync failure. The bench tunes the rate and
  the block size.
- **Image offsets.** The `merge_bin` offsets must match the core 2.0.11 build
  output. A wrong offset flashes a broken image that the MD5 verify still passes,
  because the verify checks the bytes written, not the layout. B4 reads the
  offsets from the build output, and the bench boots the flashed board to confirm.
- **Active-build attestation is app-side.** The attestation prompt gates an
  active flash in the app, not in the hardware. It is a policy gate, like the
  [T12 operator attestation](acceptable-use.md#active-operation), not an RF
  control.
- **Linux build reproducibility.** The Linux image may differ from a local macOS
  build. The manifest records the image hashes per build, so the flasher verifies
  the image it shipped, whichever runner built it.

## References

- [T13 requirements](t13.md)
- [UART protocol](uart.md): the ASCII protocol and the feature bits.
- [T12 architecture](t12-architecture.md): the capture-mode protocol and the
  descriptor.
- [Acceptable use: active operation](acceptable-use.md#active-operation)
- [Development plan](plan.md)

[t12-gap]: t12.md#follow-ups
