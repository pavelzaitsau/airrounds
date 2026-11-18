# AirRounds for Flipper Zero

AirRounds controls one passive visit on the ESP32-S2 Wi-Fi Devboard.
Use an approved 2.4 GHz access point. Have its BSSID and channel ready.

## Build and connect

Use the [pinned SDK][sdk] and run `make embedded` from the repository root.
The app is `artifacts/quality/flipper/airrounds.fap`. Copy it to
`apps/GPIO/` on the Flipper SD card. Open **Apps → GPIO → AirRounds**.

Load the [AirRounds UART firmware][uart] on the Devboard. Follow the same
guide for power, ground, and the UART pin connections. On Flipper, close
the USB-to-UART Bridge before opening AirRounds. Power the Devboard during
the whole visit.

## Start, stop, and check a visit

1. Open **Start visit**. Enter the BSSID as 12 hex digits and select **Save**.
1. Use Left and Right to pick a channel. Press OK when **Start** appears.
1. On **Status**, press OK when **Stop** appears to end reception.
1. Press Back to return to the menu. Open **Status** to read the result again.

AirRounds keeps the last BSSID you saved and the channel while the app is
open. Press Back in the BSSID editor to discard your edits.
**Status** shows **Reception stopped** and **Saving...** while the board
finishes a sample. It shows **Saved** only after the board confirms the
file. A quiet visit shows **No beacon**. The error screen says whether
the radio or SD save failed.

## Restore the status after a break

The board continues its visit if the UART link drops or you leave the
screen. AirRounds checks the connection and reads the board's status when
the link comes back. It also requests fresh status when you open the screen.
If a start or stop reply is lost, use the recovered status to decide
whether to send another command. If **Start rejected** or **Stop rejected**
appears, press **Refresh** for the current result.

See the [T5b checks][checks] for host tests and bench steps.

[checks]: ../tests/README.md#flipper-app-t5b
[sdk]: ../docs/t0.md#flipper-zero
[uart]: ../docs/uart.md
