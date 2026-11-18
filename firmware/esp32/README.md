# ESP32-S2 Wi-Fi Devboard

The storage writer saves blocks to a new file on a mounted microSD card.
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
writing. The caller can read the file error and end the session. T2 checks
the file after close before reporting it as saved.
