# AirRounds UART protocol v1

ESP32-S2 runs one passive round at a time. It listens for one beacon per
access point. The client controls the round over UART. Copy each PCAP and
its receipt from the SD card.

## Connect

Use UART0 at 115200 baud with 8 data bits, no parity, and 1 stop bit.
Use 3.3 V logic and a common ground. Connect ESP32-S2 GPIO 43 (TX) to
Flipper pin 14 (RX) and GPIO 44 (RX) to Flipper pin 13 (TX).

Send ASCII frames with one space between fields and a line feed at the end.
Requests begin with `@` and have up to 99 bytes between the marker and line
feed. A new `@` discards any partial request. The parser also discards
a partial request if 100 ms or more pass between byte reads. Send a whole
line in one write.

The board drops malformed headers and incomplete frames. For each valid
header, it sends one reply. Read the reply from `!` through the next line
feed. A new `!` replaces any partial reply. Boot text can appear before `!`
on the same line. Allow up to 191 bytes from the reply marker through the
line feed. Replies to the original commands still fit in 127 bytes. Drop a reply if it has missing, extra, or invalid fields.

## Request and reply fields

```text
@version boot request scope command [arguments]
!version boot request scope code session state result last_mutation features
```

| Field | Meaning |
| --- | --- |
| `version` | Two lowercase hex digits. Version 1 is `01`. |
| `boot` | Sixteen hex digits. The board creates this ID at boot. Use zero in a `hello` request. |
| `request` | Sixteen hex digits. The board echoes the request ID. |
| `scope` | Eight hex digits. The board echoes the session ID from the request. |
| `session` | Eight hex digits. The latest accepted session. Zero means idle after boot. |
| `code` | Result of the command. |
| `state`, `result` | Current session state and overall result. |
| `last_mutation` | Sixteen hex digits. The highest start or stop request ID processed in this boot. |
| `features` | Two hex digits. Bit 0 supports one passive visit. Bit 1 supports the round commands. |

Use lowercase hex throughout. In a `hello` reply, the board sends its own
version and boot ID. Other replies echo both fields from the request.
For `hello` and `status`, use a fresh request ID with its top bit
set. Use random IDs for these reads if the client does not keep a counter
across restarts. For `start` and `stop`, use IDs from
`0000000000000001` through `7fffffffffffffff`, in increasing order.

## Commands

| Command | Scope | Arguments and effect |
| --- | --- | --- |
| `hello` | Zero | Learn the boot ID, version, features, and current state. |
| `start` | Current session ID plus 1 | Send a unicast BSSID as 12 hex digits and a channel as two hex digits. Channels 1 through 13 use `01` through `0d`. |
| `stop` | Current nonzero session ID | Stop receiving, then finish saving any sample. A repeat has no further effect. |
| `status` | Zero or current session ID | Read the current state. Scope zero works before the first visit and after reconnect. |

The client starts with `hello`. Continue only after an `ok` reply for
version `01` with feature bit 0 set. Use the round commands if the reply
also sets bit 1. Treat an incompatible version or a
missing compatible reply as a failed connection.

A valid `start` uses the next session ID even if the radio fails. The board
keeps the latest session until it accepts the next one. A start for the
current session and the same target returns its state without starting
again. A start for that session with a different target returns `conflict`.
Session IDs and mutation request IDs do not wrap within a boot.

### Command codes

| Code | Meaning |
| --- | --- |
| `ok` | The command was accepted. Read `state` and `result` for the visit outcome. |
| `unsupported_version` | The request version is different from the board version. |
| `wrong_boot` | The request belongs to another boot. Send a fresh `hello`. |
| `bad_request` | The command, arguments, or request ID format is invalid. |
| `bad_target` | The address is zero or multicast, or the channel is outside 1 through 13. |
| `unknown_session` | The command's scope does not match its allowed session ID. |
| `busy` | The current visit is running or saving. |
| `conflict` | The same request ID changed content, or the current session changed target. |
| `stale_request` | A newer mutation request ID has already reached the board. |

### Visit status

| State | Result | Meaning |
| --- | --- | --- |
| `idle` | `none` | The board has not accepted a visit since boot. |
| `running` | `none` | The board is listening. The visit ends after one beacon or 5 s. |
| `finalizing` | `none` | Reception has stopped. The save result is pending. |
| `complete` or `stopped` | `saved` or `not_observed` | The visit ended on its own or by request. A saved PCAP has a verified receipt. A visit without a beacon produces no file. |
| `error` | `radio_error` or `storage_error` | The radio or storage operation failed. |

The board stops the radio before it starts SD work. Find the capture at
`visit-<boot>-<session>.pcap` on the card. Use the boot and session IDs from
the reply. The board reports `saved` after it checks the closed file and
publishes the receipt. If a stop ends a quiet visit, the reply can report
`stopped` and `not_observed` at once.

## Run more than one visit

Use these commands when the board advertises feature bit 1.

| Command | Scope | Arguments and effect |
| --- | --- | --- |
| `round` | Current session ID plus 1 | Send one to three BSSID and channel pairs in visit order. Use the same field widths as `start`. |
| `round_stop` | Current nonzero session ID | Stop reception and save any beacon already in hand. Finish a save in progress and skip later visits. |
| `round_status` | Zero or current session ID | Read the current round and its results. Use a read request ID, as for `status`. |

The board rejects an empty list and a repeated BSSID. A retry for the current
session uses the same pairs in the same order. The board visits each pair
once. If a point moves to a different channel, the board still listens on
the channel in the request. It reports `not_observed` if it hears no matching
beacon in 5 s, then moves to the next point. A radio or storage error ends the
round.

The round commands append `aps` to the reply, followed by one field per
access point in visit order. Each field has 15 lowercase hex digits. The
first 12 are the BSSID, the next two are the channel, and the last is the
visit result. An idle board appends only `aps`. For example,
`aps 020000000001062 0200000000020b1` reports a save on channel 6 and
no beacon on channel 11.

| Last digit | Visit result |
| --- | --- |
| `0` | No result yet. The first such point is active while the round runs or saves. After a stop or error, remaining points were skipped. |
| `1` | `not_observed` |
| `2` | `saved` |
| `3` | `radio_error` |
| `4` | `storage_error` |

A complete or stopped round reports an overall result of `saved` if at
least one point saved a beacon. Otherwise it reports `not_observed`.
Read the `aps` fields for each result. The original `hello`, `start`,
`stop`, and `status` replies use the original fields.

On the card, a multi-point round uses `visit-<boot>-<session>-<slot>.pcap`.
Slots start at 1 and follow the request order. Use the matching `.pcap.ok`
receipt for each file. A round with one point uses the original file name.

## Retry and reconnect

Send one mutation at a time. If its reply is missing, resend the same
request with the original ID and bytes. The board remembers the latest
mutation code and returns it with the current state. An earlier mutation
cannot run again. A failed command with a valid mutation header and verb
also consumes its request ID.

On reconnect, send a fresh `hello`. Read `last_mutation` before choosing the
next mutation ID. Keep track of the highest ID you sent during the same
board boot. Use an ID above both values for a new command. Send a new
`status` request to recover a lost result.
If UART drops while a round runs, the board finishes its scheduled visits.
After reboot, the board starts idle under a new boot ID.

For each reply, check the version, boot, request ID, and echoed scope before
using its state. `hello` is the only command that accepts a new boot ID.
Use the state from matching `ok` replies. On `wrong_boot`, send a fresh
`hello`. Accept one reply per request. After a retry, send a fresh
`status` to read the latest state. Drop late replies for other requests
or sessions.

When the app starts a round with multiple points, it checks the reply
against the requested list. Later replies for that round keep the list
and its finished results fixed.

## Example

This example starts after the board boots. Send `hello` first. Replace the
sample boot ID with the value in its reply. Use the BSSID and channel of
the approved access point. On a board that has already run a visit, use the
session and request IDs from the `hello` reply to choose the next IDs.

```text
@01 0000000000000000 8000000000000001 00000000 hello
@01 123456789abcdef0 0000000000000001 00000001 start 020000000001 06
@01 123456789abcdef0 0000000000000002 00000001 stop
@01 123456789abcdef0 8000000000000002 00000000 status
```

See the [UART checks](../tests/README.md#uart-protocol) for the bench steps
and results.
