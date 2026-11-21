# ESP32-S2 Wi-Fi Devboard

Read [acceptable use](../../docs/acceptable-use.md) before you build or flash
the firmware.

The storage writer writes blocks to a new file on a mounted microSD card.
The [toolchain example](../../tests/toolchain/esp32/airrounds_storage_smoke)
connects the writer to the Arduino SD library. The current radio workflow
saves one beacon per verified PCAP.

## Write blocks

Call `SdFile::openNew` with the full device path on the mounted card.
The call fails if the file already exists. Use `lastError()` for the error
code from the file system.

Use one producer to call `WriteQueue::enqueue` and one consumer to call
`drainOne`. The queue copies each block into fixed storage. A full queue
rejects the new block and increments `overflowCount()`. The block being
written still occupies its slot. Empty and oversized inputs return
`invalid` without changing the overflow count.

Call `drainOne` outside the receive callback. It writes at most one block
per call. Keep the file attached to the same queue while blocks are pending.

| Result | Meaning |
| --- | --- |
| `idle` | The queue had no block to write. |
| `complete` | The file API accepted all bytes in the front block. |
| `partial` | The file API accepted a prefix. The next call starts at the first unwritten byte. |
| `no_progress` | The file API accepted no bytes and gave no error code. The block stays queued. |
| `unavailable` | The file is closed. Open a new file before draining the queue. |
| `io_error` | The write reported a file-system error, with or without progress. |
| `invalid` | The write count was outside the requested range. |

After `io_error` or `invalid`, the queue keeps the front block and stops
writing. The caller can read the file error and end the session.

## Finish a PCAP

Create one [`PcapCapture`](storage/pcap_capture.h) per capture. Give each capture
its own path in the session directory. `open()` refuses to replace an existing
capture. Queue a classic PCAP 2.4 header and record bytes. Both byte orders and
both timestamp resolutions work. The file needs at least one record with
payload bytes.

Stop the producer before calling `finish()`. The writer drains the queue,
syncs the file, and closes it. It reopens the file to check the header, record
boundaries, byte count, and CRC-32. If a block was dropped, the capture stays
`incomplete`.

A successful check writes a small receipt. For `visit.pcap`, the writer
syncs, closes, and checks `visit.pcap.ok.part` before renaming it to
`visit.pcap.ok`. The receipt holds the PCAP byte count and CRC-32. A file error
returns `error` and leaves the data for diagnosis.

## Check after restart

Mount the card and call `recoverPcap(path)`.

| State | What AirRounds found |
| --- | --- |
| `saved` | The final receipt and PCAP agree. |
| `incomplete` | A capture has no final receipt, or the staged receipt is still present. |
| `error` | A file check failed, or the final receipt has no matching PCAP. |
| `missing` | The PCAP and both receipt files are absent. |

Recovery checks the files in place. Use a new capture path after an incomplete
visit. Keep the receipt with the PCAP when moving the files. This check covers
the file structure and bytes. Later stages check what the records contain and
write the session log.

## Run a passive visit

A T4 visit fixes one access point address (BSSID) and one 2.4 GHz channel
at startup. It rejects an empty list, multiple targets, an invalid BSSID,
or a channel outside 1–13.

The visit starts Wi-Fi in receive-only mode and keeps its settings in RAM.
It listens on the selected channel for up to 5 s. It accepts the first complete beacon
from the selected BSSID. The receiver checks the radio channel and any
advertised primary channel. The board stops the radio before it uses the
card.

The visit writes one raw 802.11 record with the original beacon bytes. The
timestamp records elapsed time since the visit started. The visit reports
`saved` after T2 reopens the closed PCAP and publishes its receipt. A quiet
visit reports `not_observed`. A card or file error reports `storage_error`.

See the [T4 checks](../../tests/README.md#one-passive-access-point) for
the host test and the bench commands.

## Scan for nearby networks

The board listens for beacons on its allowed 2.4 GHz channels, up to 180 ms
per channel. It retains up to 16 distinct BSSIDs and SSIDs in RAM, favoring stronger signals. Discovery
does not open microSD. It uses the same receive-only radio as a capture,
so scanning and capture take turns. See the [UART scan commands][uart].

## Run a round over UART

Build the `airrounds_uart` sketch with `make embedded`. The image is under
`artifacts/quality/esp32/airrounds_uart/`. It listens on the Devboard's UART
pins and writes captures to the microSD card.

The board copies a profile of up to five access points when it accepts a round. It visits BSSIDs
in order on the channels in the profile, up to the round deadline.
Points on the same channel have separate visits. The board keeps the first
valid beacon from each point for its PCAP.

A visit starts with up to 5 s of listening. A valid beacon is new if its
SSID bytes, beacon interval, and capability flags form a combination the
visit has never seen. The first valid beacon counts. Sequence numbers,
timestamps, and other fields do not count. The board remembers up to four
combinations per visit. Once that list is full, no further beacon can
extend the visit.

A new observation can move the receive cutoff to one second after the
callback sees the beacon. The cutoff stays within 7 s of the start of
listening. After each radio setup, the board divides the time left by
the number of points still to visit. The current point gets at most
that share. Slow setup or saves can shorten later visits.

The round deadline is 10 s per selected point from the start of the
round. At the deadline, the board stops accepting beacons and starts no
more visits. It closes and checks any capture already in hand.
A quiet point produces no file. A radio or SD error ends the round.
Earlier save results stay available in UART status until the next round
or a reboot. The PCAP files and receipts stay on the card.

See the [UART protocol][uart] for wiring, commands, and file names.

[uart]: ../../docs/uart.md

### On-card session log

Before listening, the firmware tries to write a session checkpoint to
`session-<boot>-<session>.log.open` on microSD. It records the build, Git
revision, UART version, passive mode, start time, time budget, and ordered
BSSID and channel pairs. If the board resets mid-round, the checkpoint can be
matched to any verified PCAPs by boot ID, session ID, and point order.

At the end of the round the firmware writes `session-<boot>-<session>.log`.
The final log also records the end time, overall result, and per-point
results and statistics. These include the listening interval, management
frames delivered to the receive callback, accepted matching beacons,
filtered frames, write-queue overflows, bytes accepted by storage, verified
PCAP size, and saved filename. Skipped points use `outcome=none` and zero
counts.

The writer syncs each staging file and reads it back byte for byte before
publishing it. A leftover `.log.open.part` or `.log.part` can help diagnose
an interrupted write. Once the verified `.log` is published, the writer
removes the checkpoint. If both files are on the card, use the final
`.log`. A checkpoint requires a writable card before listening begins.

The round stays `finalizing` until the final log attempt finishes. Query
`round_status_log` for the log status. Flipper shows `Session log failed`
if the board reports a journal failure. The PCAP result still reflects
only the capture.

`make embedded` sets the source build ID and Git revision. Set
`AIRROUNDS_REVISION` to a full Git SHA when building an exported tree.
