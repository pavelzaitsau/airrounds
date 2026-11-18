# Flipper Zero app

T5b will add the AirRounds app to this directory. The ESP32 firmware already
provides the [UART commands and replies][uart]. The current bench uses the
Flipper USB-to-UART bridge and a Mac to send those commands.

The app will start and stop visits. It will show when reception stops, when
saving is in progress, and whether saving succeeded or failed. When the user
returns to the screen, the app will read the current result with `status`.
See the [development plan][plan] for the T5b checks.

[plan]: ../docs/plan.md#steps-and-acceptance-checks
[uart]: ../docs/uart.md
