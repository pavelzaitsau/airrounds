# AirRounds development plan

This plan covers passive diagnostics through T11. Phase T12 adds the first
active mode, in a separate opt-in build; the default build stays receive-only.
See [T12](t12.md). Phase T13 adds Flipper-driven board flashing, a board
self-test, a microSD gate, and a CI release. See [T13](t13.md). Both toolchains
are installed and produced test `.bin` and `.fap` files. See the
[T0 report](t0.md) for the results and open checks.

## Bench and first release

- Use a Flipper Zero, an ESP32-S2 Wi-Fi Devboard, and a microSD adapter for the
  board.
- The current scan workflow runs on one to five approved 2.4 GHz access
  points. The original three-point profile remains a legacy workflow.
- Show progress and the result for each access point on Flipper. ESP32 writes
  a PCAP file and a log to its microSD card. Transfer the files to a Mac for
  an independent check.
- Test quiet air, no clients, an unavailable access point, and an SD card
  failure. Set time limits after bench measurements.
- Keep the original step IDs T0 through T6, T8, and T9, and add T10. T3
  checks PCAP integrity and access point context. The original plan's active
  step, T7, is outside this release.

## Earlier findings

- The original plan records separate revisions for the companion app, the
  ESP32 source, and the previously installed image. In T0, record exact
  revisions and working tree states. Identify the installed image and check SDK
  compatibility.
- Earlier builds of the preinstalled board firmware used Arduino ESP32 core
  2.0.11 and a Flipper-specific build target. Match the Flipper SDK to its
  firmware. Confirm tools and commands in T0.
- Eighteen earlier tests passed with stand-ins. One test used the real write
  module and a mock SD card. A full write saved 4 of 4 bytes, a failed open
  saved 0, and a short write saved 2. The module cleared the buffer in all
  three cases. These results apply to the revisions tested at the time.
  AirRounds still needs its own tests and hardware checks.
- Receipt, write, and file verification are distinct events. A receive count
  and a file on disk do not prove that the session data was saved.

## Components and session limits

Flipper stores the profile, shows progress, and starts or stops the session.
ESP32 controls session state, scheduling, receipt, and writes. A Mac checks the
files after transfer. See the [component map](architecture.md).

The session fixes its BSSID list at startup and rejects an empty list. It tracks
the current access point separately, even when two points share a channel.
The mode stays passive under older settings. An incompatible protocol blocks
startup. After reboot, the device retains the unfinished session for diagnosis
and waits for a new request to start.

The radio callback checks the minimum record length and matches the current
access point. It places accepted records in a bounded queue. Parse data,
perform I/O, and report detailed events outside the callback. Count overflows
and limit record size and memory use. End the session with an explicit error
if storage fails.

## States and file checks

- A session moves through `idle → preflight → running → finalizing → complete`.
  A user stop goes through `finalizing` to `stopped`. A storage or finalization
  failure ends in `error`.
- An access point moves through `pending`, `observing`, and `finalizing` before
  it gets a result. For `saved`, reread the closed file and record the result in
  the log. Use `not_observed` when there was no observation and
  `no_usable_sample` when there was no usable sample. Mark unfinished points
  `stopped` after a stop. A write failure gives the current point
  `storage_error`. A power loss leaves unfinished data `incomplete`.
- Check the header, record boundaries, and at least one complete record of the
  expected type for the BSSID. Keep the original SSID bytes when available.
  Leave a hidden or unknown SSID unknown.
- Skip an access point after it reaches `saved`. A session can end in `complete`
  with different results for different access points. After interruption, count
  data as saved only if the verified file agrees with the log.

## Observation windows

T8 visits points in profile order until the round deadline. Each visit starts
with up to 5 s to accept beacons. A new combination of SSID bytes, beacon
interval, and capability flags can extend the window. Repeats leave the
cutoff in place. Both the 7 s visit cap and the round deadline limit the
cutoff. The round gets 10 s per selected point. The board shares the remaining
time among points still to visit.

The board can finish a save after the round deadline. The
[ESP32 guide](../firmware/esp32/README.md#run-a-round-over-uart) describes
the limits and what happens when a visit uses its time.

## Storage and UART

- Give each session its own directory. Store a PCAP per visit or access point
  and keep a session log. Name files with internal IDs or BSSIDs.
- On a short write, record the actual offset. If you can confirm where the
  write stopped, retry only the remaining bytes. If the offset is unknown,
  mark the file incomplete. Finish the previous file before opening a new one.
  Count queue overflows separately.
- Log build IDs and revisions, settings, the access point and channel, time
  intervals, receive and loss counts, bytes written, file size, and the result.
  A Mac can compute checksums after transfer.
- Confirm that the session stopped separately from confirming that it saved.
  Stop receipt, drain the queue, close and check the file, then record the
  result. On error, keep the diagnostics you can and mark the result incomplete.
  A PCAP and its log can fail independently.
- UART negotiates the protocol version and features. The `start`, `stop`, and
  `status` messages identify both the request and the session. Repeated requests
  are safe. A stale reply leaves the new session alone. Use `status` to recover
  a lost event. Transfer the PCAP from the card.

## Steps and acceptance checks

| Step | Depends on | Checks |
| --- | --- | --- |
| T0: Baseline builds | None | Record exact revisions, SDKs, dependencies, build commands, and hashes for `.bin` and `.fap`. Repeat the available baseline tests. |
| T1: SD writes | T0 | Detect failed opens, short writes, full cards, and overflow. Retry without copying the accepted prefix. Test the production module with a mock SD card. |
| T2: Finish a PCAP | T1 | Confirm that consecutive files do not interfere. Check `saved`, `error`, and `incomplete` after failure and restart. |
| T3: Check a PCAP | T0. Integrate after T2. | Check valid, truncated, damaged, and unsupported synthetic inputs. Open the valid file in a separate analyzer on a Mac. |
| T4: One access point | T2, T3 | Keep receipt on the chosen access point and channel. Reject an empty list. Confirm saved status against the closed file. |
| T5a: ESP32 protocol | T4 | Test retries, UART loss, an incompatible version, and stale replies. |
| T5b: Flipper interface | T5a | Show distinct states for stop, save in progress, success, and error. Leave the screen and return to see the current state. |
| T6: Up to three access points | T5b | Test zero, one, and three points, shared and distinct channels, loss or channel change, no traffic, and completion of all visits. |
| T8: Observation windows | T6 | Limit extensions, ignore repeated observations, serve other points fairly, and enforce the absolute deadline. |
| [T9: Hardware acceptance](t9.md) | T8 | Closed for the original beacon criteria. The report records the deferred checks and the later scan workflow evidence. |
| [T10: Capture modes](t10.md) | T9 | Closed. Add the **Capture mode** choice with Beacon and two reserved placeholders. Move Beacon behind a mode contract in the firmware and the app, with per-mode frame types and frame chains; see the [design](capture-modes-design.md). Reserve the protocol fields in documentation only. Any future mode stays within the four limits in [acceptable use](acceptable-use.md#capture-modes-in-scope). Acceptance: the requirement, the mockup with its own test, the design, Beacon unchanged in the existing host tests and on hardware, and the menu checks in the UI emulator. |
| [T11: EAPOL capture](t11.md) | T10 | Built on the host. The reserved Todo A placeholder is now an EAPOL mode that records the WPA 4-way handshake frames for the round's target; see the [spec](t11.md). The mode runs under the four limits in [acceptable use](acceptable-use.md#capture-modes-in-scope) and adds no transmission. The parser is clean-room. Protocol step R5 runs on the board and in the app, with the writer EAPOL gate, the journal fix with format v3, a 60 s budget per EAPOL point, and the receive-only source check. The version is 0.2. `make check`, `make asan`, `make tsan`, and `make embedded` pass within every memory limit. The WPA2-PSK bench is the open item: no device is available yet; see the [acceptance report](t11.md#acceptance-report). |
| [T12: Active EAPOL capture](t12.md) | T11 | The opt-in active mode: a targeted unicast deauthentication forces a client rejoin, then the mode captures the EAPOL handshake. The transmit path compiles only into a separate opt-in build; the default build stays provably receive-only. Built in steps A1 to A6, with the transmit audit; the host gates pass. The WPA2-PSK bench is the open item; see the [acceptance status](t12.md#acceptance-status). |
| [T13: Board flashing and CI](t13.md) | T12 | The Flipper flashes and tests the Devboard on its own, and a CI release bundles the `.fap` with the Devboard image. The Flipper enters the ESP32-S2 ROM bootloader over UART and writes the image from the Flipper card. **Test board** runs a full self-test; a microSD gate blocks capture without a card. The public release flashes the passive receive-only image; the active transmit image builds only with the opt-in `-DAIRROUNDS_ACTIVE=1` flag, off in public builds. Requirements only; see [T13](t13.md). |

## Bench checks and handoff

Before T9, record the Flipper and ESP32 versions, the approved access point IDs
and channels, and the card details. Record whether a client and an independent
receiver are available.

Test a normal passive write, no client or access point, shared and distinct
channels, and a control access point outside the list. Also test stop, loss of
UART, reboot, and an unavailable or full SD card. If an independent receiver
is available, use it to confirm that AirRounds does not transmit. Log missing
equipment and leave the corresponding checks open.

Start with T0, then T1 and T2 and the file check. Finish an end-to-end session
for one access point before adding more. Record each step's results. Keep the
previous image and a rollback procedure. The handoff includes a build manifest,
the `.bin` and `.fap`, host results, profile and log examples, and a separate bench
report. See [tool setup](development.md).
