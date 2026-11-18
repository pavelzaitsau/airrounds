# Host tests

This directory holds component and interface tests. See the
[test fixtures](fixtures/README.md) for cases and synthetic input generators.

The [toolchain examples](toolchain) check the cross-compilers and packaging.
See the [T0 build commands](../docs/t0.md).

## Mac PCAP checks

`make test` runs the Mac checks and verifies a capture with a receipt from T2.
Run the Mac tests on their own from the repository root.

```sh
python3 -m unittest -v tests.mac.test_check_pcap
```

See the [Mac tool guide](../tools/mac/README.md) for the T3 samples and
the independent `tcpdump` check.

## SD writes and PCAP finalization

Run the host tests from the repository root.

```sh
make test
make asan
make tsan
```

Use `make check` to include formatting and static analysis. See
[quality checks](../docs/quality.md) for tools, profiles, and exclusions.

The tests cover a failed open, short and zero writes, and reported card
errors. They check that overflow keeps the blocks in order when the writer
is busy. A host file size limit also forces a real `write` failure.

The PCAP tests cover close, read, and receipt errors. They also check an
interrupted process, changed bytes, and truncation at a record boundary.
`tcpdump` reads the synthetic capture as a separate check.

To compile the SD example, use the [T0 ESP32 settings](../docs/t0.md).
Build `tests/toolchain/esp32/airrounds_storage_smoke` with a separate output
directory and the flag `--library "$PWD/firmware/esp32/storage"`. The example
uses synthetic data. Physical card checks belong to the bench test.

For the finalization example, use the same build settings with
`tests/toolchain/esp32/airrounds_pcap_smoke`. It uses a synthetic Ethernet frame
to exercise both finalization and recovery.

## One passive access point

`make test` runs the T4 host test. The test supplies a matching beacon,
frames from a different access point and channel, malformed input, and
frames at the time limit. It checks the empty list, a quiet visit, a file
open error, and the contents of the closed PCAP and receipt. It also
checks a beacon that spans two write blocks and discovery with two
matching access points. The USB readback tests cover a short write and
files that differ from the expected size.

Build the USB bench image with `make embedded`. The image is under
`artifacts/quality/esp32/airrounds_passive_smoke/`. Use an ESP32-S2 Wi-Fi
Devboard, a microSD card, and the SPI adapter with CS on GPIO 10. Keep a
backup of the current flash before loading the bench image. The SPI pinout
is in the board's adapter wiring guide.

Open the USB serial port at 115200 baud. Send each command with a newline.
Use the name of an approved access point in `FIND`. Use its BSSID and
channel in `START`.

```text
PING
START
FIND Lab
START 02:00:00:00:00:01 6
```

Expect `READY T4` for `PING`. An empty `START` returns
`RESULT empty_list` before the board tunes the radio or opens the card.
`FIND` listens for beacons with the exact name on the board's allowed
2.4 GHz channels. It reports `RESULT found` with a BSSID and channel when
it sees one match. It returns `ambiguous_ssid` if the same name appears
at more than one BSSID or on more than one channel. Discovery creates no
PCAP.

A positive visit prints `TUNED channel=6` for the example channel.
Wait for `RESULT saved`. The output includes the file path and the
bytes read back from the card. The file and its receipt also stay on the card.
If the result includes `transfer=error`, copy the saved files from the card.

Verify the copied PCAP and receipt on a Mac. Replace the filenames and BSSID
with the values from the visit.

```sh
python3 tools/mac/check_pcap.py visit.pcap \
  --receipt visit.pcap.ok --bssid 02:00:00:00:00:01
tcpdump -nn -e -r visit.pcap
```

The checker should report one record and a matching receipt. Inspect the
beacon's source, BSSID, and advertised channel in the `tcpdump` output.

The November 18, 2026 host run saved an 87-byte synthetic capture.
The Mac checker verified its receipt. `tcpdump` read one Lab beacon
for `02:00:00:00:00:01` on channel 6.

On the ESP32-S2, the bench run returned `empty_list` for an empty request
and `not_observed` on the control channel. The selected channel produced
one beacon. The Mac verified the PCAP and receipt read back from microSD.
The hardware capture, receipt, and bench serial log are in the local
`artifacts/t4/hardware/` folder.

### Verify the original firmware after restore

On November 18, 2026, the original preinstalled v1.20.0 firmware booted from
`app0` after the flash restore and reset. It reported the microSD card as
connected.

We sent `info` from the Mac through the Flipper USB-to-UART bridge.
The bridge used 115200 baud, with Flipper pin 13 for TX and pin 14 for RX.


## UART protocol

`make test` feeds UART bytes to the production protocol. It checks retries,
a split frame, lost replies, link recovery, wrong versions, and delayed
requests and replies. Reply tests cover cut-off replies and boot text
before a reply. The tests also check that reception stops before storage
starts, that a quiet stop reports its final result right away, and that the
board reports a storage error.

Use the [UART firmware and wiring][uart-protocol] for the hardware check.
Keep a backup of the current flash before loading the UART image.

1. Use the example `hello` to learn the boot ID, then start a visit.
1. Resend the same start request. Check that the session ID stays the same.
1. Stop a running visit. Check for `finalizing`, then poll until it ends.
1. Run a visit and disconnect UART without removing board power.
1. Reconnect. Send a fresh `hello` and a fresh `status`. Check the result.
1. Send a start with version `02`. Check for `unsupported_version`.
1. Send an old stop during a new visit. Check that the new visit continues.
1. For `saved`, copy the PCAP and receipt from the card and run the Mac check.

### T5a bench results

On November 18, 2026, a Mac sent commands through the Flipper USB-to-UART
bridge to an ESP32-S2 Wi-Fi Devboard with a microSD card. A temporary bench
build used the Devboard's own USB port to switch its UART pins off and on.
The board stayed powered during both runs.

In each run, the board reported `running`. The test then disabled the UART
pins for 5.9 s. A `status` request during the break timed out. After the pins
came back, a fresh `hello` returned the same boot ID. A fresh `status` returned
the result for the original session ID.

| Session | Channel | Case | Recovered state and result |
| --- | --- | --- | --- |
| 1 | 1 | Control channel | `complete`, `not_observed` |
| 2 | 11 | Approved access point | `complete`, `saved` |

The saved row records the board's report. A separate Mac check of this run's
PCAP and receipt is still open. The test used Flipper as a serial bridge.
See the [Flipper app checks][flipper-t5b] for the T5b test scope.

Before the test, we read all 4 MiB of flash and verified the backup against
the device. After the test, we wrote back the full backup and verified the
flash contents. The original preinstalled firmware booted from `app0` and
reported that the microSD card was connected.

The local `artifacts/t5a/hardware/` folder holds the backup and its SHA-256.
It also holds `uart-gpio-test-final.log`, `restore-gpio-bench-final.log`, and
`restored-runtime.log`.

[flipper-t5b]: #flipper-app-t5b
[uart-protocol]: ../docs/uart.md

## Flipper app T5b

`make test` connects the Flipper client to the ESP32 protocol in a host test.
It checks stop, saving, save success, and a storage error. It drops replies
to start and stop, then checks recovery without another command. It also
checks incompatible firmware, late replies, and fresh status after opening
the screen or restarting the app. It checks that a Stop press applies only
to the selected visit and that the app reports a rejected command. It also
checks that a new command uses a fresh ID after a lost write.

`make embedded` builds the app for the pinned Flipper SDK and checks its
imports. Use the [app guide][flipper-app] to install it for a hardware test.

1. Start a visit and open **Status**. Check that the app shows **Listening**.
1. Break UART while keeping the ESP32 powered. Check for **UART link lost**.
1. Reconnect UART and check that the result appears.
1. Leave **Status** before a visit ends. Return after it ends and check that
   the screen reports the current result.
1. Stop a visit while the board is listening. Check the stop and save
   results. Use a storage fault to check the error result.

### T5b bench results

On November 18, 2026, we ran `airrounds.fap` on a Flipper Zero with firmware
1.5.1. An ESP32-S2 bench build used its USB port to switch its UART pins
off and on. The board stayed powered during the link break.
The bench FAP had this SHA-256 value.

`446eb8dbe5ea769248da3fbe7539f113e65aaa104512aaf1b2af4a7ecbadc6b4`

| Check | Result on Flipper |
| --- | --- |
| Start and stop a quiet visit | **Listening**, then **Stopped** and **No beacon** |
| Disable UART during a new visit | **UART link lost** and **Reconnecting...** |
| Leave Status and return while UART is down | **UART link lost** |
| Reconnect UART after about 6.6 s | **Complete** and **No beacon** |
| Leave Status and open it again | **Complete** and **No beacon** |
| Run a visit on the approved access point | **Complete** and **Saved** |

The host tests cover the save-in-progress and error messages. The bench
records the board's saved result. A separate Mac check of the capture and
receipt is still open.

We restored the earlier 4 MiB flash image after the test. The flash tool
verified the written data before resetting the ESP32.

The local `artifacts/t5b/hardware/final/` folder holds the screen images,
`flipper-bench.log`, and `esp-restore.log`.

### Reviewed T5 bench results

On November 18, 2026, we ran the updated Flipper app with the updated ESP32
protocol. The FAP used for this run had this SHA-256 value.

`cd253558b7b0dbe2b30b82356a4cc63f1b2122da7a50cb42a6ac844775231841`

The ESP32 bench build used its separate USB port to switch the UART pins
and copy mutation replies to the test log.

| Check | Result |
| --- | --- |
| Press **Stop** while Flipper shows **Listening** on the quiet channel | The first ESP32 reply was `stopped` and `not_observed`. Flipper showed **Stopped** and **No beacon**. |
| Disable UART during a second visit and reopen Status | Flipper showed **UART link lost** while the ESP32 stayed powered. |
| Reconnect after about 6.6 s | Flipper showed **Complete** and **No beacon**. The result was still visible after leaving Status and opening it again. |

After the run, we restored the earlier 4 MiB ESP32 flash image. The flash
tool verified the written data and reset the board. The local
`artifacts/t5/hardware/reviewed/` folder holds the screen images, the UART
test log, and the flash restore log.

[flipper-app]: ../flipper/README.md
