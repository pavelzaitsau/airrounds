# AirRounds for Flipper Zero

AirRounds controls passive Wi-Fi sessions on the ESP32-S2 Wi-Fi Devboard.
Prepare the BSSID and 2.4 GHz channel of each approved access point.

## Build and connect

Run `make embedded` from the repository root with the [pinned SDK][sdk].
Copy `artifacts/quality/flipper/airrounds.fap` to `apps/GPIO/` on the Flipper
SD card and open **Apps → GPIO → AirRounds**.

Use the [AirRounds firmware and wiring guide][board] for the Devboard.
Close the Flipper USB-to-UART Bridge before opening AirRounds. Keep
the Devboard powered during a session.

One point also works on the older AirRounds board firmware. Two or three
points require board firmware that supports AirRounds rounds. **Board status**
explains when the firmware needs an update.

## Save a profile

1. Open **Set up points** or **Access points**.
1. Choose **Add point** and enter the six BSSID bytes with the hex keyboard.
1. Select **Save** on the keyboard. Use Left or Right to choose the channel.
1. Press OK on **Save**. Add up to three points.

Open a point to edit or remove it. Back from the channel screen returns
to BSSID entry. Back from BSSID entry discards the draft. The app saves
one profile on the Flipper card and loads it when the app opens again.
If the card cannot confirm a write, the app shows **Save not confirmed**.
If the saved profile changed while the app was open, reopen AirRounds to
load it before making edits.

## Run a session

1. Select **Start session**. Check every BSSID and channel on **Review session**.
1. Press OK on **Start**. **Observing** shows the active point and channel.
1. Press OK on **Stop** to stop the session. **Stopping...** waits for the
   board. **Reception stopped** means reception ended and the board is
   still finishing the file. **Saving point** means later visits can follow.
1. Open **Results** to see each point. Select a **Saved** point, then press
   Right for its file name. Read the lines as one name at the root of the
   Devboard card.

**Saved** means the board confirmed the closed file and its receipt.
**Not seen** means the board found no matching beacon. **Stopped** means
the stop left an unfinished point with no result. A card error or radio
error has its own label.

## Recover a session

Back leaves the status screen. Use **Session status** to return.
AirRounds opens a confirmed active session when the app starts.

If UART drops, the board may finish the session on its own. Reconnect the
cable to read the board's current state. AirRounds waits for that state
before offering a new Start or Stop. **Command unconfirmed** means the app
is still checking what happened. **Last result** shows the latest confirmed
result available to the app, including after a board restart.

[Design and prototype][design] · [UART protocol][uart]

## Test without a device

Run `python3 tests/flipper/run_ui_emulator.py` from the repository root.
The headless emulator runs the native app source with simulated Flipper
services. It uses the AirRounds board protocol and scheduler with a fake
receiver. The test covers setup and saving, profile reload, three-point
review, Stop, lost UART, results, and older board firmware. It does not run
the ARM package or open a USB port.

[sdk]: ../docs/t0.md#flipper-zero
[board]: ../firmware/esp32/README.md
[design]: ../docs/ui-ux/requirements.md
[uart]: ../docs/uart.md
