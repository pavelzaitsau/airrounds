# AirRounds on Flipper Zero

AirRounds runs passive Wi-Fi diagnostics on one saved profile with up to three
approved 2.4 GHz access points. Flipper starts and stops the session. The
ESP32-S2 Devboard runs it and saves files to its own microSD. The operator checks
those files on a Mac after the session. One point is useful for bench tests.
[Project plan][plan] · [Architecture][architecture]

## Approved design

The operator chose **option A**. Start from the main menu, keep one saved
profile, and use short English labels. Show the full BSSID and channel for every
point before starting. The profile uses fixed labels such as `Point 1`.
Each session keeps its own copy of the point list.

[Open the clickable prototype](prototype.html) · [View the sketches](sketches.png)
· [Read the design review](review.md)

The [native Flipper app](../../flipper/README.md) implements this flow. Its
result labels use the outcomes that the Devboard sends. The file details
use the UART protocol's real file name instead of the demo path.

## Try the prototype

Open `prototype.html` in Chrome. The page uses local scripts and starts with a
demo profile if the browser has no saved profile.

- Use the on-screen Flipper buttons or your keyboard. Arrow keys move.
  Enter is OK. Esc is Back.
- Start with OK → OK. Select **Acknowledge Start** under **Board event**.
  Press OK to request Stop. Select **Acknowledge Stop**, then **Finish saving**.
  Press OK again to open **Results**.
- In a new session, disconnect UART while listening or after requesting Stop.
  Step through board events while disconnected. Reconnect UART and choose
  **Read current session**. Flipper shows the board's answer.
- Use **Empty profile** to try adding a point. The demo accepts synthetic
  examples such as `02:00:00:00:00:01` on channel 6. Use the
  hexadecimal on-screen keys, then **Next** and **Save**.
- **Reset demo** restores the sample profile and resets the simulated board.

The simulator steps through events on demand. Its board state lives in the
open tab. The saved profile uses browser storage. The browser prototype
draws a two-color 128×64 display with a bitmap font. The prototype covers the
main session and UART recovery flow.
Board restart, card error, and firmware mismatch remain bench checks.
Run the transition checks from the repository root.

```bash
node --test docs/ui-ux/prototype.test.cjs
```

## What the current project supports

| Source | What we know |
| --- | --- |
| AirRounds T5b | The current Flipper app has `Status` and `Start visit`. It uses a keyboard for BSSID entry and a separate channel screen. The app keeps the last address and channel in memory. |
| UART v1 | The board reports one point and one latest visit. Reception and file saving have separate states. The board can finish a visit after losing UART. [UART v1][uart] |
| T5b bench | Flipper firmware 1.5.1 screenshots show link loss, recovery, `Stopped / No beacon`, and `Complete / Saved`. These checks cover one point. |
| T6 plan | Flipper stores the profile. The board runs the passive session and reports a result for each point. [Project plan][plan] |

The research used the original UART commit `cc6d2d8` and the T5b app and bench
work. The native port uses the T5b commit `bfb39a7` and the current T6 board
code as its base. The project plan defines this passive release.

## Flipper display and controls

| Platform detail | Design rule |
| --- | --- |
| Monochrome LCD, 128×64. Direction pad with OK and a separate Back button. [Hardware][display] · [Controls][controls] | Use clear text and an inverted highlight. Keep the main flow on short button presses. |
| A full-screen `Submenu` shows three rows below a heading. [Submenu][submenu] | Keep three main menu items. |
| Full-screen mode uses all 64 rows. Windowed mode reserves 13 rows for the system bar. [GUI layout][gui-layout] | Use full-screen mode. Show the Devboard link inside the app. |
| Firmware provides `FontPrimary`, `FontSecondary`, and button hints. [Canvas][canvas] · [GUI elements][elements] | Use short labels, test their pixel width, and leave space for the OK hint. |
| `ViewDispatcher` switches views. `SceneManager` can manage larger navigation flows. [GUI guide][gui] · [ViewDispatcher][dispatcher] | Reuse the menu and input modules. Use a separate view for live status. Decide on `SceneManager` when implementing T6. |
| uFBT builds FAP files with a pinned SDK. [uFBT][ufbt] · [FAP manifest][fam] | Build against the project SDK, f7 1.5.1 with API 88.2. [Project SDK][sdk] |

## Main menu and profile

- The first item is `Session status` while a known session is active, saving,
  or waiting for link recovery. Otherwise it is `Set up points` for an empty
  profile, `Start session` for a ready board and a saved profile, or
  `Board status` when the board blocks the start.
- `Access points` opens the saved points. Choose `Add point` when space is
  available. OK on a point shows its address and opens `Edit` or `Remove`.
  Confirm removal. After removing the last point, `Add point` is available.
- `Last result` opens the latest confirmed result available to the app. It
  shows `No result available` when there is none. Results use the point list
  from their own session.
- On app entry, a board-confirmed active session opens once. After Back from
  status, the first menu item can reopen it. Link recovery leaves an editor
  or result screen in place.
- Use a single saved profile. Reject a fourth point, a duplicate BSSID, and an
  invalid address or channel. A separate channel screen lets the operator
  confirm the address and channel before saving. Back returns to the previous
  step. Leaving address entry discards the draft.
- Use the built-in `ByteInput` as the firmware starting point. Its keys cover
  `0–9` and `A–F` and it shows all six bytes. Changing one digit should keep
  the other digits. If saving fails, keep the earlier profile and offer a
  retry or Back. [ByteInput 1.5.1][byte-input]

## Check and start a session

`Review session` shows up to three rows. Each row shows the point number, full
BSSID, and channel. The board must confirm compatibility and availability
before Flipper offers `Start`. The start request uses the list shown on that
screen.

If the link drops or the board restarts, check availability again.
`Update Devboard` means the board confirmed incompatibility. If the board
does not answer, show `Devboard offline` and `Check cable`. Show card readiness
only when the board confirms it. Once Start is sent, show `Starting...` and
wait for the board before offering a new command.

## Live session

| Condition | Display | OK |
| --- | --- | --- |
| Waiting for the Start reply | `Starting...` / `Waiting for board` | — |
| Board confirms the current point | `Observing` / `Point 2 / Ch 6` | `Stop` |
| Board is saving one point and the session may continue | `Saving point` / `Point 2` | `Stop` if the board supports it during point saving |
| Waiting for the Stop reply | `Stopping...` / `Waiting for board` | — |
| Board stopped receiving and is finishing the session | `Reception stopped` / `Saving to board SD` | — |
| Reply lost and status is being read | `Checking status` / `Command unconfirmed` | `Refresh` after automatic recovery ends, if available |
| Lost UART during an active session | `Link lost` / `Session may run` / `Check cable` | — |
| Link restored, board state still unknown | `Checking status` / `Waiting for board` | — |
| Board boot ID changed during a known session | `Devboard restarted` / `Check board SD` | `Results` if confirmed results are available |
| Terminal board state | `Session ended`, `Session stopped`, `SD write failed`, or `Radio failed` | `Results` if results are available |

Stop ends the session after the board safely finishes any current write.
Back leaves the status screen. Bind OK to the action and session displayed
when the button goes down. If the action changes before release, require a
fresh press. When UART returns, read the current board state before enabling
another Start or Stop. [UART v1][uart] · [Project plan][plan]

## Results and files

Show the session outcome separately from the number of saved points.
`Session ended` with `1 of 3 saved` is a valid result.

| Point outcome | List | Details |
| --- | --- | --- |
| `saved` | `Saved` | `Saved to board SD`. The board confirmed the file. |
| `not_observed` | `Not seen` | `Point not seen` during the visit. |
| `no_usable_sample` | `No sample` | `No usable sample`. The board observed the point and found no usable sample. |
| `stopped` | `Stopped` | `Stopped before result`. |
| `storage_error` | `SD error` | `SD write failed`. |
| `incomplete` | `Incomplete` | `Incomplete file`. Exclude it from the saved count. |
| Missing status | `Unknown` | `Result unavailable`. |

OK opens the selected point. Show the full BSSID and channel. For a saved
point, Right opens the file path when the board confirms it. Use numbered
pages for a path that is too long for one screen. If the path is unknown,
show `File path unavailable` and `Check board SD`. The Mac checks files
independently after the session. [Project plan][plan]

## T6 board data and bench checks

The Flipper app needs T6 support from the board. It needs the board boot ID
and session ID, the session's full point list, the current point, and a result
for each point. It also needs a final state and a way to distinguish point
saving from session saving. File links require a confirmed path or a stable
naming rule. Show a countdown or card readiness only with confirmed data.
[UART v1][uart] · [Project plan][plan]

On the device, check first-time entry, profile persistence, duplicate addresses,
three points, and Back from each screen. On the bench, check a lost Start reply,
a lost Stop reply, UART loss during saving, a board restart, and a card error.
Compare the on-screen result with the files the board reports.

[plan]: ../plan.md
[architecture]: ../architecture.md
[uart]: ../uart.md
[sdk]: ../t0.md#flipper-zero
[display]: https://docs.flipper.net/zero/development/hardware/tech-specs
[controls]: https://docs.flipper.net/zero/basics/control
[gui]: https://developer.flipper.net/flipperzero/doxygen/app_dev_gui.html
[dispatcher]: https://developer.flipper.net/flipperzero/doxygen/view__dispatcher_8h.html
[submenu]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/modules/submenu.c
[byte-input]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/modules/byte_input.c
[gui-layout]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/gui_i.h
[canvas]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/canvas.c
[elements]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/elements.h
[ufbt]: https://github.com/flipperdevices/flipperzero-ufbt
[fam]: https://github.com/flipperdevices/flipperzero-firmware/blob/dev/documentation/AppManifests.md
