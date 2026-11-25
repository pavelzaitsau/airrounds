# T12 architecture and code plan

This document plans the firmware, protocol, and tool changes for the
[active EAPOL mode](capture-modes-design.md#active-eapol-mode), and the order to
build them. It writes no code. The requirements are in [T12](t12.md), and the
mode behaviour is in the [capture modes design](capture-modes-design.md).

The guiding constraint is confinement. The default build stays provably
receive-only: `tests/mac/test_receive_only.py` passes against it unchanged. The
transmit path exists only in the opt-in active build, in one reviewed file outside
the default gate's scan root.

## Build layout

The active mode is a separate build, selected by a compile-time define. The two
builds share every file except the transmit path. A5 updates
[uart.md](uart.md#capture-modes) to document wire ID `0x02`, the `modes` reply byte
layout (a count byte then the ID list, so `03 00 01 02` in the active build), and
`round_status_mode` carrying `0x02`.

| Build | Define | Compiles | Gate |
| --- | --- | --- | --- |
| Default | none | `firmware/esp32/` | `test_receive_only.py`: no transmit call, `WIFI_MODE_NULL` only |
| Active | `AIRROUNDS_ACTIVE=1` | `firmware/esp32/` plus `firmware/active/` | A new active gate variant: exactly one `esp_wifi_80211_tx` call, confined to `firmware/active/` |

- The `esp_wifi_80211_tx` call site lives in `firmware/active/deauth_tx.h`, outside
  the `firmware/esp32/` tree the default gate scans. The default build never
  compiles it, and the default gate never sees it, so the default build stays
  provably receive-only.
- `make embedded` builds and gates both images every run. The default invocation
  stays as it is; an added active invocation passes `--library firmware/active` and
  the `AIRROUNDS_ACTIVE=1` define. Both the default gate and the active gate run each
  time, so the active build is always exercised.
- The active gate variant scans `firmware/active/`. It asserts exactly one file
  holds exactly one `esp_wifi_80211_tx` call, no other transmit token
  (`esp_now_send`, `WiFi.*`, another `esp_wifi_*_tx`), and `WIFI_MODE_NULL` only,
  except the single mode-change token that step A2 may allow. It reuses the
  allowlist approach of `test_receive_only.py`.
- The separation is directory-based only. A shared `firmware/esp32/` file must
  never wrap `esp_wifi_80211_tx` in `#ifdef AIRROUNDS_ACTIVE`, and must never name
  the token, even in a comment. The default gate reads raw text, including comments
  and code the default build does not compile, so any mention there fails it.
- The active gate also asserts that no `firmware/esp32/` file includes a
  `firmware/active/` header. The token grep cannot see an include, so an accidental
  include would otherwise link the transmit path into the default image.
- The default build's assurance is the conjunction of three checks: the default
  gate finds no transmit token under `firmware/esp32/`, the active gate confines the
  one transmit call to `firmware/active/`, and no `firmware/esp32/` file includes
  `firmware/active/`.

## Radio mode

The active build opens the radio in `WIFI_MODE_STA`, not `WIFI_MODE_NULL`, so the
station interface exists for `esp_wifi_80211_tx`. The station-mode token lives only
in `firmware/active/`: the active sketch calls `PassiveVisit::setRadioMode(WIFI_MODE_STA)`,
and the active gate's mode allowlist admits it. The default build never leaves
`WIFI_MODE_NULL`.

The station radio stays up for the whole active visit, not only around the one
transmit call, so the capture runs in station promiscuous mode. The active build
calls no `esp_wifi_connect` and no `esp_wifi_scan_start`, so the station stack has
no reason to transmit on its own. The [open hardware bench](t12.md#hardware-bench)
confirms two facts before shipping: `esp_wifi_80211_tx` transmits in station
promiscuous mode, and the station radio emits nothing but the deauthentication
burst (no probe, authentication, or association frames).

## The raw-frame sanity check

A deauthentication frame spoofs its source address as the access point BSSID. The
ESP-IDF rejects such a frame: `esp_wifi_80211_tx` calls
`ieee80211_raw_frame_sanity_check`, which fails any frame whose source is not the
interface MAC, so the call returns an error and nothing leaves the radio.

The active build overrides the check with its own
`ieee80211_raw_frame_sanity_check` that returns zero, in `firmware/active/deauth_tx.h`.
The symbol exists only in the opt-in build, so the default build is unaffected.

The ESP-IDF defines the same symbol, non-weak, in `libnet80211.a`. A plain
override fails the link with a multiple-definition error. The active build passes
`-Wl,-zmuldefs` to the linker, so the override wins and the archive copy is
dropped. `tools/quality/embedded.py` sets the flag for the active build only, so
the default image never sees it.

The bench confirmed the path end to end: the transmit returned `ESP_OK`, the
deauthentication forced a client rejoin, the mode captured the EAPOL handshake,
and the journal recorded the three transmitted frames.

## Contract change: R6

The [capture-mode contract](capture-modes-design.md#the-capture-mode-contract)
gains step R6. The transmit primitive lives on the context, not on the base
contract, so the base contract stays transmit-free.

- `CaptureMode` gains `virtual void poll(uint64_t nowUs, ModeContext& ctx)`, with a
  default empty body. Passive modes do not override it. The active mode drives its
  burst from `poll`, never from `onFrame`, because `onFrame` runs in the receive
  callback and must not block or transmit.
- `ModeContext` gains `bool transmit(const uint8_t* frame, size_t len)`, plus the
  reserved `tune(channel)` and `note(key, value)`. In the default build, `transmit`
  delegates to a null transmitter and returns false, and no radio transmit call
  compiles. The active build injects a transmitter from `firmware/active/` that
  calls `esp_wifi_80211_tx`.
- `PassiveVisit` calls `poll` from the UART loop between frames, and holds the
  transmit budget. `transmit` checks and decrements it, so the per-round cap holds
  even if the mode has a defect. `PassiveVisit` is a process-lived singleton, so the
  budget does not reset by object lifetime. `PassiveRound` re-seeds it at every round
  start, through a `seedTransmitBudget(cap)` call on round-begin; the seed runs once
  per round and re-runs each round. `reset()`, which clears a visit, never touches
  the budget, so two visits in one round share one 30-frame budget and two rounds
  each get a fresh 30.

## Protocol

The active mode adds one wire ID. The [UART protocol](uart.md#capture-modes)
carries it under the existing feature bit 5.

| Item | Rule |
| --- | --- |
| Wire ID | `CaptureModeId::active = 2`, `0x02` on the wire. `uart_reply.h` adds the matching `activeModeId` constant and the `static_assert`. |
| `modes` reply | The active build lists `03 00 01 02`. The default build lists `02 00 01` and never lists `0x02`. |
| Budget | `PassiveRound::visitBudgetUs` returns 60 s for the active mode, as for EAPOL. |
| `round_status_mode` | Carries `0x02` for an active round, like any other mode. |
| Cross-build pairing | The app always carries the mode-2 descriptor and offers it only when the board lists `0x02`. An older app without the descriptor falls to `modeOf`'s neutral labels for `0x02`. |

The reply fits the 240-byte buffer: the `modes` reply gains one byte, and
`round_status_mode` already counts the mode byte.

## Flipper descriptor

`flipper/capture_mode.h` gains one `CaptureModeInfo` for the active mode.

- The menu label is **Deauth**, which names the transmit behaviour at the point of
  selection. It is six characters, like the other mode names, so it fits the row.
- `implemented` is true and `implied` is false, so the screen offers it only on a
  board that lists `0x02`, which only the active build does. A default-build board
  shows the slot as unavailable.
- The result labels reuse the EAPOL set: success is a frame found, and the detail
  reports a handshake saved.
- There is no on-device attestation screen. The operator attestation is a policy
  step in [active operation](acceptable-use.md#active-operation), not a Flipper
  prompt.

## Journal and the Mac check

The journal records the transmit audit, and a Mac check verifies it.

- `session_journal.h` adds a transmit audit record per visit in the active build:
  the reason code, the frame count, the target BSSID, the target client MAC address,
  and a timestamp per transmitted frame. The active build's journal is format v4; the
  default build stays v3. v4 is additive, a superset of v3, so no existing reader
  needs a migration, and only the new Mac verifier reads the audit.
- The capture PCAP is unchanged: it holds only the EAPOL-Key frames, which
  `check_pcap.py --eapol` already verifies.
- A Mac check verifies the audit: a `check_pcap.py` journal mode, or a separate
  script, confirms the audit names a unicast target, stays within the burst and the
  per-round cap, and matches the capture's target BSSID. The audit is the only
  record of what the mode transmitted.

## Code steps

Build in this order. Each step lands with its host tests and keeps every memory
gate green.

| Step | Depends on | Content |
| --- | --- | --- |
| A1 | None | Contract R6: `poll` on `CaptureMode` (default empty), `ModeContext` with a null-transmitter `transmit`, `tune`, `note`. `PassiveVisit` calls `poll`; `PassiveRound` re-seeds the transmit budget each round. The shared tree gains no transmit call or token. Tests: `poll` runs, passive modes unaffected, default `transmit` returns false, two visits share one budget while two rounds each reset, and `test_receive_only.py` and the full passive suite stay green after the shared-tree change. |
| A2 | None | Radio mode: the active build opens `WIFI_MODE_STA`, confined to `firmware/active/`; see [radio mode](#radio-mode). The [hardware bench](t12.md#hardware-bench) confirms the station transmit and that the station stack emits nothing else. |
| A3 | A1, A2 | Build and gates: `firmware/active/deauth_tx.h` with the deauthentication frame builder and the one `esp_wifi_80211_tx` call. The `AIRROUNDS_ACTIVE` build in `embedded.py`. The active gate variant `tests/mac/test_active_transmit.py`. The default gate stays green. |
| A4 | A1, A3 | Active mode logic: `ActiveEapolMode` reuses the EAPOL capture, adds target discovery from an uplink `addr2` with the group, BSSID, and zero rejections, the burst state machine in `poll` (up to two bursts, 5 s apart), capture-before-transmit, auto-stop, and a `done()` override that finishes the visit on a crackable set (message 1 with a PMKID, or messages 1 and 2 from one client). Tests: target selection, burst bounds, round cap, auto-stop, early finish, and the deauthentication frame bytes, written clean-room from the IEEE 802.11 standard. |
| A5 | A3, A4 | Protocol and descriptor: wire ID `0x02`, the `modes` reply (`03 00 01 02` in the active build), the budget, `round_status_mode`, the `uart.md` wire-format update, the Flipper descriptor and labels, and the cross-build pairing. Tests: labels, availability, and the `modes` parse. |
| A6 | A4 | Journal audit: the active build emits format v4 with the appended audit record; the default build stays v3. The Mac audit verifier. Tests: the audit fields, the verifier, and the default journal unchanged at v3. |
| A7 | A1 to A6 | Acceptance: `make check`, `make asan`, `make tsan`, and `make embedded` for both builds; the default gate passes; the active gate passes; a WPA2-PSK bench run on an own network with a measured transmit; the audit verified. The [T12 acceptance report](t12.md#acceptance-criteria). |

## Memory

The active mode reuses the EAPOL mode's four 512-byte frame buffers and adds a
small transmit state: the target client address, the burst counter, and the burst
timing. The ESP32 global RAM budget is 65,536 bytes, of which the firmware uses 54,256
today. The active transmit path is a separate image, so it adds nothing to the
default image. A1 adds a `poll` vtable slot and a transmitter pointer to the shared
tree, which grows the default image by a few bytes. `make embedded` is the memory
gate for both builds.

## Risks

- The active build runs the radio in `WIFI_MODE_STA` for the whole visit, so the
  station stack must not transmit on its own. The build calls no `esp_wifi_connect`
  or `esp_wifi_scan_start`; the [hardware bench](t12.md#hardware-bench)
  confirms no unsolicited emission.
- The `poll` cadence must meet the 100 ms burst spacing without blocking. The UART
  loop drives `poll`; if its period exceeds 100 ms, the burst uses a non-blocking
  timer inside the mode, still bounded by the cap.
- 802.11w Protected Management Frames defeats deauthentication. A modern target may
  produce no handshake. That outcome is `not_observed`, not an error, and the bench
  report states it.
- The active gate variant is a new source-level check. Like the default gate, it
  rests on source review, not on an RF measurement; see
  [known limits](acceptable-use.md#known-limits).

## References

- [T12 requirements](t12.md)
- [Capture modes design](capture-modes-design.md): the contract, the admission
  checklist, and the active mode entry.
- [Acceptable use: active operation](acceptable-use.md#active-operation)
- [UART protocol](uart.md#capture-modes)
