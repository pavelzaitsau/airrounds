# AirRounds

AirRounds is a passive Wi-Fi diagnostic tool for Flipper Zero and the ESP32-S2
Wi-Fi Devboard. Scan for nearby networks and run a round on one to five approved
2.4 GHz access points. Use Flipper to select networks and view the results. ESP32 saves beacons to
microSD. Use a Mac to check the files after transfer.

| Directory | What it does |
| --- | --- |
| `firmware/esp32/` | Receives data, tracks the session, and writes to microSD |
| `flipper/` | Selects access points, controls the session, and shows results |
| `tools/mac/` | Checks the PCAP files and session log |
| `tests/` | Holds host tests and synthetic inputs |
| `docs/` | Describes the components, development steps, and acceptance checks |

T0 starts with the source revisions and build tools. See the
[development plan](docs/plan.md), [component map](docs/architecture.md), and
[tool setup](docs/development.md). See the [scan workflow and mockup](docs/ui-ux/scan-flow.md).

## Quality checks and project skills

Run `make check` for formatting, static analysis, host tests, and sanitizers.
Run `make embedded` with the T0 SDKs to build and analyze the ESP32 and Flipper
examples. See [quality checks](docs/quality.md) for setup, scope, and limitations.

Project skills live in [`.agents/skills/`](.agents/skills). They include the seven
Copydesk skills and the Trail of Bits `harness-writing`, `libfuzzer`, and
`sharp-edges` skills. Open a skill's `SKILL.md` for its workflow and requirements.
