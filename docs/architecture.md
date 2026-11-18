# AirRounds architecture

Flipper builds a `.fap` app. ESP32 builds a `.bin` firmware image. A single
repository lets us check compatibility across both components in one change.
Each component keeps its own build system.

| Feature | Location | Checks |
| --- | --- | --- |
| PCAP writes and file finalization | `firmware/esp32/` | Storage errors, short writes, and readability after close |
| Passive sessions for selected access points | `firmware/esp32/` | One or more access points, no observations, and a fixed session deadline |
| UART control | `firmware/esp32/`, `flipper/` | Version compatibility, repeated requests, and lost links. Check that stop and save have distinct results. |
| Transfer and review on a Mac | `tools/mac/` | Read the PCAP, check the records, and compare them with the log. |
| Acceptance | `tests/`, `docs/` | Record host and hardware results separately. |

ESP32 owns the session and reports whether each file was saved. Flipper shows
the result from the board. The Mac reads the transferred files and compares them
with the session log. Document the UART protocol and log format in `docs/`
when the first testable versions are ready.

Use the component directories to pin compatible source revisions and SDKs.
The [development plan](plan.md) lists the steps and when each is complete.
