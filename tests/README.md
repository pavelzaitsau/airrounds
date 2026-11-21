# Host tests

This directory holds component and interface tests. See the
[test fixtures](fixtures/README.md) for cases and synthetic input generators.

The [toolchain examples](toolchain) check the cross-compilers and packaging.
See the [T0 build commands](../docs/t0.md).

## How the gate finds tests

`make test` runs every test it finds and fails if any test file is out of reach:

- Each `tests/<folder>/*_test.cpp` is a C++ host test. The exception is
  `tests/flipper/ui_emulator_test.cpp`, which runs in the UI emulator.
- Each `test_*.py` module runs through `unittest` discovery. Discovery only
  enters folders with an `__init__.py`. The gate stops if a test module sits
  in a folder without one.
- Each `*.test.cjs` file under `docs/` or `tests/` runs with `node --test`.

## Mac PCAP checks

`make test` runs the Mac checks and verifies a capture with a receipt from T2.
Run the Python tests on their own from the repository root.

```sh
python3 -m unittest discover -s tests -t . -p "test_*.py"
```

See the [Mac tool guide](../tools/mac/README.md) for the T3 samples.

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
Devboard, a microSD card, and the SPI adapter. The firmware uses SCK on
GPIO 36, MISO on GPIO 37, MOSI on GPIO 35, and CS on GPIO 10. Keep a
backup of the current flash before loading the bench image.

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
```

The checker should report one record, a matching receipt, and a beacon from
the requested BSSID.

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
board reports a storage error. The `firmware` read tests check the version
reply, its field rules, and that a refused, malformed, or missing version reply
leaves the Flipper link up.

Use the [UART firmware and wiring][uart-protocol] for the hardware check.
Keep a backup of the current flash before loading the UART image.

1. Use the example `hello` to learn the boot ID, then start a visit.
1. Resend the same start request. Check that the session ID stays the same.
1. Stop a running visit. Check for `finalizing`, then poll until it ends.
1. Run a visit and disconnect UART without removing board power.
1. Reconnect. Send a fresh `hello` and a fresh `status`. Check the result.
1. Send a start with version `02`. Check for `unsupported_version`.
1. Send an old stop during a new visit. Check that the new visit continues.
1. Send a `firmware` read with scope zero. Check that the reply reports
   `firmwareVersion` from `firmware/esp32/uart/firmware_version.h`.
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

## Passive rounds T6

`make test` runs the round scheduler with the production beacon filter and
file writer. It supplies three BSSIDs on channel 6, then three on distinct
channels. It checks that each saved PCAP contains the expected beacon and
has a valid receipt. A third case moves the middle BSSID off its configured
channel. That visit times out with no file, and the third point still saves.
A quiet round ends when both visit windows expire. The host test checks the
eight saved captures with `recoverPcap`. The gate also runs the Mac checker
and `tcpdump` on the captures from the PCAP and session tests.

The UART and Flipper tests cover empty and duplicate profiles, a changed
request order, replies from older firmware, and recovery after a lost reply.
They check that a stop during a save preserves the file and skips later
points. They also check that an SD error ends the round. The scheduler tests
reject a new start during listening or saving. The client tests reject
replies that change the round's access points or finished results.

### Bench steps

1. Enter three approved access points in Flipper. Use one channel for all three.
1. Run the round. Check that each BSSID has its own result and capture.
1. Run with three distinct channels. Check the results in request order.
1. Use a different channel for the middle point and run again. Check for
   **No beacon** followed by a result for the last point.
1. Copy the PCAPs and receipts. Run the [Mac checker][mac-checker] on each file with its BSSID.

### T6 bench results

On November 18, 2026, a Mac sent T6 commands to an ESP32-S2 over USB.
A temporary bench build used the production round, radio, and microSD code.
It also let the Mac read files from the card over USB. The same access point
was first in all three rounds.

| Case | Channels in visit order | Visit results | Round time |
| --- | --- | --- | --- |
| Three points on one channel | 11, 11, 11 | Saved, Saved, Saved | 4.13 s |
| Three points on distinct channels | 11, 4, 1 | Saved, Saved, Saved | 5.10 s |
| Middle point unavailable on its configured channel | 11, 11, 1 | Saved, No beacon, Saved | 8.00 s |

In the last run, the middle BSSID still used channel 4. The ESP32 listened
on channel 11 for that visit. The card held no PCAP or receipt for slot 2.
The last visit still saved its beacon.

The Mac checker matched all eight PCAPs to their expected BSSID and receipt.
`tcpdump` showed one beacon per file, with the expected BSSID and channel.

The first Flipper screen check lost its USB connection. On a repeat with
firmware 1.5.1, the T6 app opened **Status** and showed **Ready**. The console
reported 121,880 bytes of free heap during the repeat launch. The cause of
the first failure remains open.

We restored the original 4 MiB ESP32 image, verified it against the backup,
and reset the board. The local `artifacts/t6/hardware/` folder holds the
captures, receipts, UART replies, and bench logs.

### T6 rounds started on Flipper

On November 18, 2026, we repeated the three cases after review. AirRounds
ran on a Flipper Zero with firmware 1.5.1. The Mac sent button presses to
the Flipper interface. AirRounds sent each round to the ESP32-S2 over UART.
The Mac read status and copied the files from microSD over the ESP32 bench
USB port.

Two points from the earlier shared-channel list timed out on the first
attempt. We ran a fresh passive scan and selected three visible points on
channel 11.

We started AirRounds again for each case. Flipper's `top` sampled memory
about once per second. We kept the samples where the ESP32 reported that
the round was listening or saving. The stack value shows the least free
space on the 4,092-byte AirRounds thread stack since launch. The heap
value is the smallest free-heap sample for the whole Flipper.

| Case | Channels | Results in visit order | Active samples | Min. stack free | Min. sampled heap free |
| --- | --- | --- | --- | --- | --- |
| Three points on one channel | 11, 11, 11 | Saved, Saved, Saved | 6 | 2,212 B | 118,784 B |
| Three points on distinct channels | 11, 4, 1 | Saved, Saved, Saved | 3 | 2,284 B | 118,832 B |
| Middle point unavailable | 11, 11, 1 | Saved, No beacon, Saved | 7 | 2,280 B | 118,752 B |

For the last case, the middle point broadcast on channel 4 while Flipper
requested channel 11. AirRounds showed **No beacon** for that point and
**Saved** for the third. The card held no PCAP or receipt for the middle
point. The Mac checker and `tcpdump` confirmed the BSSID, channel, and
single beacon in all eight PCAPs. Each PCAP matched its receipt.

AirRounds stayed open and showed **Complete** after every round. Flipper
uptime increased through all three runs. We removed the temporary app
copy, then restored and verified the original ESP32 image.

The local `artifacts/t6/flipper-hardware/reviewed/` folder holds the screen
images, memory samples, UART replies, PCAPs, and receipts.

## Observation windows T8

`make test` uses synthetic beacons and a fake clock. It checks that sequence
numbers, timestamps, other changing fields, and a return to an earlier
value leave the cutoff in place. A new SSID, interval, or capability can
extend the visit. A full history, an invalid beacon, and a frame at the
cutoff cannot extend it.

The round tests check that an extension leaves time for the other points
and that each point gets its own result. They delay radio setup and saves
to check shorter visits and expiration. A save that crosses the deadline
keeps its result. A point that never started listening after an earlier
visit stays unvisited.

### T8 bench results

On November 18, 2026, a Mac ran T8 rounds over USB on an ESP32-S2
Wi-Fi Devboard with a microSD card. The board used its own clock, radio
setup, and SD writer. For the timing cases, the test passed synthetic
beacons in memory to the normal receive callback. It changed sequence
numbers and timestamps on repeats. The callback applies the receive
cutoff. The measured window ends when the main loop starts to shut down
the radio, which can be slightly later.

| Visit | Channel | Beacon times after listening began | Measured window | Result |
| --- | --- | --- | --- | --- |
| 1 | 1 | First at 0.100 s. Repeat at 4.800 s. | 5.000 s | Saved |
| 2 | 6 | First at 4.801 s. Repeat at 5.601 s. | 5.802 s | Saved |
| 3 | 11 | First at 4.801 s. New SSID at 5.651 s. New interval at 6.501 s. Earlier values at 6.901 s. | 7.001 s | Saved |

The first repeat left the 5 s window in place. The second repeat left the
extension from the first beacon in place. New values extended visit 3
to the 7 s limit. The board visited all three points in order and saved
the first beacon from each point.

To reach the round deadline, the test held back the first save result
until 22 s after the round started. The board still visited points 2 and
3 in order. Their windows shortened to 3.998 s and 2.461 s. The receive
callback rejected a beacon passed to it at 30.000559 s after the round
started. The board started shutting down the radio at 30.000819 s. The round finished with
**Saved**, **Saved**, and **No beacon** in visit order.

The test then held a save result until 31 s. The Mac saw the round report
`stopped` after 31.087 s. The first point kept its saved result. The other
two points stayed unvisited.

In a separate run, the board listened over the air to the three access
points from the T6 bench, on channels 11, 4, and 1. Each point had a
5.001 s window and saved one beacon. The Mac saw the round finish in
19.015 s, including radio setup and card writes.

The [Mac checker][mac-checker] matched all nine PCAPs to their access
points and receipts. `tcpdump` read one beacon from each file.

The first bench build did not show a USB port after reset. A second
build used the T6 startup pattern and ran all four cases. The board's
original flash was restored, checked against the full 4 MiB backup, and
reset. The local `artifacts/t8/hardware/` folder holds the timing log,
the flash logs, and the PCAPs with their receipts. The flash log records
the SHA-256 of the tested image.

[mac-checker]: ../tools/mac/README.md

[flipper-app]: ../flipper/README.md
