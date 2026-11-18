# ESP32-S2 Wi-Fi Devboard

The storage writer writes blocks to a new file on a mounted microSD card.
The [toolchain example](../../tests/toolchain/esp32/airrounds_storage_smoke)
connects the writer to the Arduino SD library.

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
