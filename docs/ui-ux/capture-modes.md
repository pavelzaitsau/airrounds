# Capture modes on Flipper

AirRounds captures one kind of Wi-Fi frame today: the beacon. This page is the
requirement for phase T10. It adds a **Capture mode** choice to the Flipper app
so the operator picks what a round captures, and it reserves two future modes
without defining them. The phase also moves Beacon behind a capture-mode
contract in the firmware and the app, with no change to its behaviour.

AirRounds stays receive-only. A capture mode never transmits. The phase does not
add handshake capture, deauthentication, injection, or traffic decryption.
Deauthentication, injection, and decryption are outside AirRounds. A later mode
may add handshake capture under the four limits in [acceptable use][aup-modes].
See [Security and permitted use](#security-and-permitted-use).

The [capture modes design](../capture-modes-design.md) describes the
refactoring, the mode contract, and the admission checklist for a new mode.

[Scan and capture workflow][scan-flow] · [UART protocol][uart] ·
[Project plan][plan]

## Approved design

The operator keeps the scan workflow and gets a **Capture mode** item in the
main menu. The current mode is **Beacon**. Two placeholders, **Todo A** and
**Todo B**, hold the place of future modes and show **Soon**. The operator
cannot select a placeholder. A round runs in one mode from start to finish.

A capture mode owns its labels and the meaning of its result. A future mode
can define success as a frame found or as an absence confirmed without a
change to the round, the radio, the shared PCAP writer, or the journal. A
mode also declares the frame types it needs: management, control, or data
frames. It can save one frame or a chain of related frames from one visit.

## Scope

| In scope | Out of scope |
| --- | --- |
| The **Capture mode** menu item and screen | The protocol commands for modes, which `docs/uart.md` only reserves |
| Mode labels, availability, and result meaning | The algorithm, name, or file format of Todo A and Todo B |
| A reserved capability bit and commands in `docs/uart.md` | A mode that persists across app restarts |
| The mockup `scan-prototype.html`, its logic module, and its test | The legacy manual profile menu, which has no scan support |
| The refactoring that puts Beacon behind a mode interface, and the **Capture mode** screen in the app | Changes to the Mac check `tools/mac/check_pcap.py` |
| | Any transmission or decryption |
| | Handshake capture, which a later mode may add |

## Main menu

The scan menu gains one item. The order is **Scan area**, **Capture mode**,
**Flash board hardware**, **Test board**, **About**.

- The screen shows four rows and the menu has five items, so the menu
  scrolls. Focus drives the scroll. Arrows show rows above or below, as in
  the **Beacons** list.
- **Capture mode** appears only for a board that supports scanning. The
  legacy menu does not show it and always captures beacons.
- During a session the first row still reads **Session status**.

## Capture mode screen

The screen has the heading **Capture mode** and three rows: **Beacon**,
**Todo A**, and **Todo B**.

- A `*` marker stands before the current mode, wherever the focus is.
- An unavailable mode shows **Soon** at the right of its row.
- On entry, focus is on the current mode. Up and Down always move the focus,
  also while the screen is locked.
- Back returns to the menu with no change.

The first row of this table that applies sets the hint and the effect of OK:

| Condition | Hint | OK |
| --- | --- | --- |
| The board state is not confirmed | **Board state unknown** | Changes nothing |
| The board runs or saves a round | **Session active** | Changes nothing |
| A scan runs, or a scan or scan-cancel request waits for a reply | **Scan active** | Changes nothing |
| Focus on an unavailable mode | **Not available yet** | Changes nothing |
| Focus on an available mode | None | Selects the mode and returns to the menu with focus on **Capture mode** |

The board state is confirmed when the link is ready, no Start or Stop
request waits for a reply, and no command is unconfirmed. Every
other state is unconfirmed, whatever the screen title: **Link lost**,
**Devboard offline**, **Checking status**, and **Update Devboard** all lock
the choice. An unconfirmed state wins over the last known session state, so
**Link lost** during a round shows **Board state unknown**. A round can still
run on the board in that state. **Devboard restarted** is a confirmed, idle
board and does not lock the choice.

A lock hint shows when the screen opens and stays while the lock holds. Up and
Down do not clear it. **Not available yet** clears at the next key press.

## Mode behaviour

- The default mode is **Beacon**. A choice lasts until the operator leaves
  the app. The next launch starts on **Beacon**.
- The local choice sets the mode of the next Start only. The round keeps that
  mode from start to finish.
- The labels and the result meaning of a running or finished round come from
  the mode that the board reports. An app reopened during a Todo A round shows
  Todo A labels, not Beacon labels. The app treats a board without the
  capture-mode capability as Beacon. If the board reports a mode that the app
  does not know, the screens use the neutral labels in the table below.
- **Scan area** always listens for beacons, whatever the mode.
- A change of mode clears the per-point results. The network list and the
  point selection stay.
- A mode is available when the app implements it and the board lists it in
  its reply to the `modes` read command. Beacon is available on every board,
  including a board without the capture-mode capability. Until a `modes`
  reply arrives, or when the read fails, no other mode can be selected or
  started.
- The choice returns to Beacon in two cases: a successful `modes` reply no
  longer lists the selected mode, for example after a board restart, or the
  board loses the capture-mode capability, as described below. A reconnect
  that still waits for the `modes` reply keeps the choice. The return to
  Beacon is a change of mode and clears the per-point results.
- While the board has not confirmed the selected mode, the **Beacons** list
  hides **Start** and shows **Mode pending** at the right of the heading. The
  selection marks stay visible. The heading shows the first text that
  applies: **Maximum 5**, **Log failed**, **Board offline**, **Mode
  pending**, then the selection count.
- The app reads `modes` again with every status poll until a read succeeds,
  and only while the board advertises the capture-mode capability. A board
  that comes back without the capability, for example after older firmware
  is flashed, gets no `modes` read. The choice returns to Beacon at once and
  the per-point results clear. The board would reject the unknown command,
  and the app would treat the rejection as a lost link.

## Labels and the meaning of a result

- The mode supplies every label of the scan path: the list heading
  **Beacons**, the row results **Saved** and **Not seen**, the progress rows
  **Listening** and **Saving**, the progress counter **Captured 1/3**, and the
  result labels **Captured**, **Point not seen**, and **Captured to board SD**.
  **Waiting for a beacon** belongs to the legacy menu, which always uses
  Beacon, and is not a mode label.
- The mode defines success: a frame found, as Beacon does, or an absence
  confirmed. For a mode of the second kind, a confirmed absence is a success
  and does not show as **Not seen**.
- The progress counter counts the points that succeeded in the mode's sense.
  The app reads the round's overall result only for Beacon. For any other
  mode it derives the result from the per-point results.
- A confirmed absence needs evidence that the Mac can check, as a saved
  beacon has its PCAP and receipt. A point without a beacon produces no file
  today, so the form of that evidence is an open question.

| Label slot | Beacon | Neutral, for an unknown mode |
| --- | --- | --- |
| List heading | **Beacons** | **Points** |
| Row success | **Saved** | **Done** |
| Row miss | **Not seen** | **No result** |
| Progress rows | **Listening**, **Saving** | **Listening**, **Saving** |
| Progress counter | **Captured 1/3** | **Done 1/3** |
| Result success | **Captured** | **Done** |
| Result miss | **Not seen** | **No result** |
| Detail success | **Captured to board SD** | **Done** |
| Detail miss | **Point not seen** | **No result** |

Each point result maps to a label slot by the mode's meaning of success:

| Point result | Frame found mode, as Beacon | Absence mode | Unknown mode |
| --- | --- | --- | --- |
| `saved` | Success | Miss: the absence was disproved | Success |
| A confirmed absence | Miss | Success | Success |
| `not_observed` | Miss | Miss | Miss |

No result yet, `radio_error`, and `storage_error` keep their current labels
in every mode.

Each row of the **Capture mode** screen starts at x = 5. The right text ends
at x = 121. The app clips the left text to a width of `112 − width of Soon`
pixels of `FontSecondary`, which leaves a 4-pixel gap before **Soon**.
Without right text, the limit is 118 pixels. The marker, its space, and the
mode name must fit in that width. Take the glyph widths from
the `u8g2_font_haxrcorp4089_tr` font in the Flipper 1.5.1 firmware source.
The mockup font and the emulator test font have other widths. Confirm the
widths on a device screenshot when the screen is built. The
[Status labels][scan-flow-labels] section lists the widths of the current
labels.

## Security and permitted use

[Acceptable use][aup-modes] owns the rules for every capture mode, Beacon
included. This page does not repeat them, so an update to the policy needs no
update here.

For phase T10, the refactoring keeps what the
[security assurance gap][t9-gap] relies on. Beacon declares management frames
only. A new mode passes the
[admission checklist](../capture-modes-design.md#admission-checklist) before
it is built.

## Non-functional requirements

- The passive guarantee of the current release holds. The board transmits
  nothing. A future mode exchanges data only with Flipper over UART and with
  the microSD journal, never over the air.
- A board without the capture-mode capability works as it does now, in
  Beacon mode. The existing UART replies keep their formats, because the
  current Flipper rejects a reply with an unknown field. A new read command
  carries the mode. An older client that reads a round of another mode sees
  a confirmed absence as `not_observed`. A client that reads the new command
  takes per-point results of such a round from that command only, because the
  Start and Stop replies use the older encoding.
- Moving Beacon behind the mode interface does not change its behaviour. The
  5-second and 7-second windows, the 10-second budget per point, and the PCAP
  and journal formats stay unchanged in T10. Firmware v0.1 later moved the
  journal to format v2. The existing host tests pass unchanged until the
  menu itself changes.

## Open questions

- The names **Todo A** and **Todo B** are placeholders. A real mode replaces
  them.
- The evidence for a confirmed absence is undefined. The design document
  defines it before the first mode of that kind is built.

[scan-flow]: scan-flow.md
[scan-flow-labels]: scan-flow.md#status-labels
[uart]: ../uart.md
[plan]: ../plan.md
[t9-gap]: ../t9.md#security-assurance-gap
[aup-modes]: ../acceptable-use.md#capture-modes-in-scope
