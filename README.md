# AirRounds

AirRounds is a passive Wi-Fi diagnostic tool for Flipper Zero and the ESP32-S2
Wi-Fi Devboard. The first version covers two or three approved access points on
2.4 GHz. Flipper controls the session, ESP32 writes PCAP files to microSD, and a
Mac checks the files after transfer.

| Directory | What it does |
| --- | --- |
| `firmware/esp32/` | Receives data, tracks the session, and writes to microSD |
| `flipper/` | Selects access points, controls the session, and shows results |
| `tools/mac/` | Checks the PCAP files and session log |
| `tests/` | Holds host tests and synthetic inputs |
| `docs/` | Describes the components, development steps, and acceptance checks |

The first task is to record the source revisions and set up the build tools.
See the [component map](docs/architecture.md) and [development steps](docs/development.md).

## Project skills

The seven Copydesk skills live in [`.agents/skills/`](.agents/skills).
Open a skill's `SKILL.md` for its workflow and tool requirements.
