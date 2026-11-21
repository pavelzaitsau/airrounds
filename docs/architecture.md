# AirRounds architecture

AirRounds runs only within the scope and limits in
[acceptable use](acceptable-use.md).

Flipper builds a `.fap` app. ESP32 builds a `.bin` firmware image. A single
repository lets us check compatibility across both components in one change.
Each component keeps its own build system.

| Feature | Location | Checks |
| --- | --- | --- |
| PCAP writes and file finalization | `firmware/esp32/` | Storage errors, short writes, and readability after close |
| Passive sessions for selected access points | `firmware/esp32/` | One or more access points, no observations, and a fixed session deadline |
| Capture modes | `firmware/esp32/`, `flipper/` | Beacon unchanged behind the mode contract, declared frame types only, and labels from the mode descriptor. See the [design](capture-modes-design.md). |
| UART control | `firmware/esp32/`, `flipper/` | Version compatibility, repeated requests, and lost links. Check that stop and save have distinct results. |
| Transfer check on a Mac | `tools/mac/` | Check the PCAP structure and receipt, and confirm a beacon from the requested BSSID. |
| Acceptance | `tests/`, `docs/` | Record host and hardware results separately. |

ESP32 owns the session and reports whether each file was saved. Flipper shows
the result from the board. The Mac checks the integrity of the transferred
files. The [UART protocol](uart.md) defines the board commands,
replies, and recovery rules.

Use the component directories to pin compatible source revisions and SDKs.
The [development plan](plan.md) lists the steps and when each is complete.
