# AirRounds usability review

Review date 18 November 2026. This is a walkthrough of the sketches and planned
behavior. It draws on UART v1, the T5b bench notes, and the T6 plan.

## Findings and changes

| Priority | Finding | Change |
| --- | --- | --- |
| High | The review screen showed a name and channel. Two access points could have the same name and channel. | Show the full BSSID and channel for each point. Keep the session list fixed after Start. |
| High | `Saving` could mean the end of one visit or the end of the session. | Show `Saving point` during a visit and `Reception stopped` while finishing the session. |
| High | A missing Start or Stop reply left the operator unsure whether the request worked. | Show the pending action. Fetch board status after link recovery before allowing a new command. |
| Medium | A first-time user had no clear next action with an empty profile. | Lead with `Set up points` and offer `Add point` without a board connection. Use the hex keyboard for BSSID entry. |
| Medium | An operator returning during a session had to find `Status`. | Open confirmed active status on entry. Keep `Session status` first in the main menu after Back. |
| Medium | `Observing 2/3` looked like progress, even when points might be revisited. | Show the current point and channel. Show the saved count when outcomes are known. |
| Medium | A quiet visit and storage failure could be confused. A long filename could be cut off. | Show an outcome for each point. Show the file path on its own screen when the board confirms it. |
| Medium | The OK action could change while the button was held. | Bind the press to the action on screen. Require a new press if that action changes. |

## Usability by task

| Task | Result |
| --- | --- |
| Start a saved profile | Two short OK presses when `Start session` is selected. The second press follows the review screen. |
| Set up a first point | Setup is available offline. Entering 12 hex characters is the longest manual step. |
| Return to a running session | One OK from `Session status`. A fresh app entry can open it automatically once the board answers. |
| Stop a visit | One OK from `Observing`. The operator can see the request, the end of reception, and final saving. |
| Recover from link loss | The screen says the session may still run. After reconnection it shows the current board result. |
| Find an output | Select a saved point and use Right from its details to see the file path. |

Option A is approved with one saved profile and English labels. It favors
repeat sessions. The profile remains one item away for an operator who needs
to edit the list.

## Prototype design pass

1. **Review the profile.** Full addresses and channels fit in three rows.
   Start is the only OK action. Menus use an inverted row highlight.
2. **Observe and stop.** The display shows the current point and the Stop
   action. The simulator button names the next board event, such as
   **Acknowledge Stop** or **Finish saving**.
3. **Recover the link.** The display gives one next step, **Check cable**.
   After connecting, **Read current session** fetches the board's outcome.
   The display offers a session action only after the response.
4. **Inspect results.** The summary gives the saved count. The list uses
   plain outcome words. Right from a saved point opens its file path.

The LCD uses a two-color bitmap font with 8.4:1 text contrast. This avoids
soft edges at integer zoom. The page drops duplicate labels and uses a
shorter header to keep the controls closer to the display on small screens.
There are no new steps in the session flow.

## Checks

- The sketch renderer checked 56 text placements across 15 screens at
  128×64. It used the Helvetica Bold and HaxrCorp fonts selected by
  Flipper's Canvas implementation. The full BSSID fits with channels 1–13.
  [Canvas 1.5.1][canvas] · [u8g2 fonts][fonts]
- Chrome passed the pointer and keyboard flow before the visual pass. That
  flow included a lost Stop reply, offline saving, reconnect, and results.
- After the visual pass, 11 transition tests passed. A local rendering check
  verified the final control bindings and text bounds on nine LCD states.
  The in-app browser blocked the local file URL. The final visual pass did
  not get a new browser capture. [Prototype](prototype.html)
- The repository's host tests also passed with `make test`.
- Hardware still needs a readability check with the device backlight.
  The bench must confirm timing, board responses, and SD results.
  [Requirements](requirements.md) · [UART v1](../uart.md)

## Native app review

| Issue found | Fix and check |
| --- | --- |
| The hex keyboard calls the app while its view is locked. | Validate the address through the dispatcher queue. A new editor discards old queued events. The physical Flipper showed **Invalid BSSID** and accepted the corrected address. |
| An old board result could hide the firmware update message. | Give **Board status** its own route. The emulator checks an old board with a terminal result and a three-point profile. |
| **Last result** could show an empty view before the board replied. | Show **Checking status** while waiting, then show the result. Freeze the report when entering its details. The emulator checks a late reply. |
| A one-point T6 Stop reply could lose the point address. | Use round commands whenever the board advertises them. The UART test reopens the client during a one-point round and checks the Stop reply. |
| A card that returned after startup could leave an older profile in use. | Reread both profile records before saving. The emulator covers read failures, a returning card, and a short write. |

The final native FAP ran on a Flipper Zero. The test saved a synthetic point,
read the profile from the card, reopened the app, and saw the same BSSID and
channel. The test also opened the editor and backed out. The Devboard was
offline in that run. The test copy and its data were removed.

The headless emulator compiled and ran the native app source. The app sent
real protocol frames to the T6 board code with a fake receiver. The test
covered Start, a stale OK press, Stop during a save, UART loss before its
reply, recovery, and the per-point results. It does not model ARM execution,
electrical behavior, or radio timing.

The current main T6 bench report covers real three-point rounds started
from its Flipper app. It records shared channels, distinct channels, and a
missing middle point. The reworked interface has its own hardware results
below.

## Review on final T6

| Issue found | Fix and check |
| --- | --- |
| Flipper's expansion service could hold the UART port even with no other app open. | Pause the service while AirRounds owns the port and restore the user's settings on exit. The emulator reserves the port by default. The physical Flipper moved from **UART unavailable** to **Devboard offline** after this fix. |
| A profile read error could let a later save replace points from a card that had returned. | Compare the saved points with the profile the editor opened. The emulator restores the card just before a save and checks that all three points stay intact. |
| A rejected Start on an older board could show the new address on a different visit. | Show a local address for an old-board result only when that board accepted the matching Start. The emulator covers the rejected Start. |
| A lost reply or a board restart left the status unclear. | Show **Checking status** after reconnect while the app waits for the board. Offer **Last result** after a restart when the app has a confirmed report. The emulator checks the lost reply and the recovery. |

The rebuilt FAP ran on a physical Flipper Zero. Its buttons opened the
profile, rejected an all-zero BSSID, saved a valid address and channel, and
left the saved point intact when a draft was canceled. A relaunch loaded
the same point from the Flipper card. The test used its own app copy and
profile and removed both after the run.

A second run loaded a three-point test profile, canceled a removal, and
then confirmed removal of the middle point. The new record on the Flipper
card held the first and third points in the original order.

### Flipper and Devboard bench

On November 18, 2026, the rebuilt FAP ran on the physical Flipper and sent
commands to the ESP32-S2 Devboard over UART. A temporary Devboard build
used the final T6 round and storage code. Its USB port let the test pause
UART replies while the board kept running.

| Test | What we saw |
| --- | --- |
| Start and Stop | **Review session** showed three points. OK started the session. Stop ended it on the first point. The results showed the two remaining points as **Stopped**. |
| Lose and recover UART | Flipper showed **Link lost**. The Devboard kept the same session running. When replies returned, Flipper showed **Observing** again. All three quiet points finished as **Not seen**, with no files or receipts on the card. |
| Save from one point | AirRounds received one beacon on channel 11. Flipper showed **Saved**, and the point details opened **Board SD file** with the single-point filename. We read that file and its receipt from the card. The Mac checker confirmed the receipt and `tcpdump` showed one beacon from the expected access point on channel 11. |

We removed the temporary Flipper app and profile. We restored the original
4 MiB Devboard image, checked it against the fresh backup, and reset the
board.

[canvas]: https://github.com/flipperdevices/flipperzero-firmware/blob/1.5.1/applications/services/gui/canvas.c
[fonts]: https://github.com/olikraus/u8g2/tree/master/tools/font/build/single_font_files
