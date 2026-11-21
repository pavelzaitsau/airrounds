# AirRounds

AirRounds is a passive, receive-only Wi-Fi capture tool for Flipper Zero and the
ESP32-S2 Wi-Fi Devboard. It saves one beacon from each approved 2.4 GHz access
point it observes, one to five per round, to microSD. Each beacon goes in its
own PCAP file with a receipt, and a session log records the round. The source
has no transmit, deauthentication, decryption, or key-derivation path. No RF
measurement or automated gate confirms the absence of transmission yet; see
[known limits](docs/acceptable-use.md#known-limits).

AirRounds is a personal project for the author's own noncommercial security
research. The author runs it only on networks the author owns or has written
permission to test. The author does not offer it as a product or for use by
anyone else.

| Component | Language | Directory |
| --- | --- | --- |
| Devboard firmware | C++ | `firmware/esp32/` |
| Flipper app | C++ | `flipper/` |
| Mac file checker | Python | `tools/mac/` |
| Host tests and UI emulator | C++, Python | `tests/` |

Run `make check` for the host quality gate. `make embedded` builds and checks
the device images. See [quality checks](docs/quality.md) and
[architecture](docs/architecture.md).

## Disclaimer

To the maximum extent permitted by law, the author provides AirRounds "as is",
without any warranty or condition of any kind. The author and the contributors
accept no liability under any legal theory, including contract, tort, and
negligence. The exclusion covers any damage, loss, legal consequence, or other
harm from the software, its documentation, or any use of them.

Anyone who builds, modifies, flashes, runs, or distributes AirRounds does so at
their own risk. That person alone is responsible for every session they run.
That responsibility covers the choice of target, the legal basis for the test,
the handling of captured data, and compliance with the law. Nothing in this
repository authorizes anyone to test any network, and nothing in it is legal
advice. Many jurisdictions restrict the capture of wireless traffic, and capture
without authorization can be a crime.

## License

AirRounds is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Commercial use is not
permitted. The license permits more than the intended use;
[acceptable use](docs/acceptable-use.md) states the intended use and its limits.
The components that [NOTICE](NOTICE) lists keep their own licenses.
