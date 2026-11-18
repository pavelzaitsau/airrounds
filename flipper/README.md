# AirRounds for Flipper Zero

AirRounds runs a passive round on the ESP32-S2 Wi-Fi Devboard.
Have the BSSID and channel for one to three approved 2.4 GHz access points.

## Build and connect

Use the [pinned SDK][sdk] and run `make embedded` from the repository root.
The app is `artifacts/quality/flipper/airrounds.fap`. Copy it to
`apps/GPIO/` on the Flipper SD card. Open **Apps → GPIO → AirRounds**.

Load the [AirRounds UART firmware][uart] on the Devboard. Follow the same
guide for power, ground, and the UART pin connections. On Flipper, close
the USB-to-UART Bridge before opening AirRounds. Power the Devboard during
the whole visit.

## Start, stop, and check a round

1. Open **Start round**. Enter the first BSSID as 12 hex digits and select **Save**.
1. Press Left or Right to pick its channel.
1. To add another access point, press Down. Enter its BSSID and pick its channel.
1. Press OK when **Start** appears. The board visits the points in the order you entered them.
1. On **Status**, press OK when **Stop** appears to end the round.
1. Press Back to return to the menu. Open **Status** to read the results again.

Press Back on the channel screen to edit its BSSID. Press Back in the BSSID
editor to return to the previous screen. The app keeps your entries while
it is open. Use each BSSID once. With older AirRounds firmware, the app offers
one access point.

For a round with multiple points, **Status** shows one result per BSSID.
**Saved** means the board checked the file and published its receipt.
**No beacon** means the board heard no matching beacon during the visit.
**Skipped** means a stop or an error ended the round before that visit.

## Restore the status after a break

The board continues its round if the UART link drops or you leave the
screen. AirRounds checks the connection and reads the board's status when
the link comes back. It also requests fresh status when you open the screen.
If a start or stop reply is lost, use the recovered status to decide
whether to send another command. If **Start rejected** or **Stop rejected**
appears, press **Refresh** for the current result.

See the [T6 checks][checks] for host tests and bench steps.

[checks]: ../tests/README.md#passive-rounds-t6
[sdk]: ../docs/t0.md#flipper-zero
[uart]: ../docs/uart.md
