# AirRounds acceptable use and licensing

AirRounds is a personal, defensive Wi-Fi security tool for noncommercial
research. Its author uses it to assess the security of networks the author owns
or has written permission to test. The author also measures the risk that
attacker-class tools pose to those networks. AirRounds is not offered for use by
anyone else. This document is the single source of truth for who may run
AirRounds, where, and under which limits. Every other document defers to it.

In this document, the author is the copyright holder that [NOTICE](../NOTICE)
names. The operator is whoever runs a session: the author, or anyone else
outside the intended use.

AirRounds is licensed under the PolyForm Noncommercial License 1.0.0. The full
license text is in [LICENSE](../LICENSE), and the third-party components and
their licenses are in [NOTICE](../NOTICE). The [Licensing](#licensing) section
states what the noncommercial restriction covers.

Read this before you build, flash, or run AirRounds. The operator carries full
responsibility for every session.

## Purpose

AirRounds listens passively around the access points the operator chose. The
round result on the Flipper and the session journal answer defensive questions
about the operator's own networks:

- Which frames do the operator's own access points expose to a passive listener.
- How much an attacker with similar hardware could collect without transmitting.
- Whether the operator's own access points behave as the operator expects.

AirRounds is a measurement tool for the operator's own perimeter. It is not a
product, a service, or a tool for use against networks the operator does not own
or control.

## Saved frames

The firmware writes the frames a round accepts to the microSD card of the
Devboard, as a PCAP file next to a session journal. This is how the device
works. It is not a permission to keep frames.

- This policy does not explicitly permit keeping frames for later analysis.
- This policy does not prohibit keeping frames temporarily for a limited use
  within one assessment of an own or permitted network.
- Delete the frames when that assessment ends. See
  [Data handling](#data-handling).

The project provides no tool or instruction for post-processing saved frames,
and does not encourage any. The Mac tool `tools/mac/check_pcap.py` checks a
transferred capture. It verifies the file structure and the receipt. With
`--bssid`, it confirms that the file holds a beacon from that access point and
reports its SSID. With `--eapol` and `--bssid`, it confirms that every record is
an EAPOL-Key frame for that access point, reports which handshake messages the
file holds per client, and reports whether one client completed a handshake. It
reads the 802.11 header, the EAPOL packet type, and the key-info flags; it reads
the client address only to group frames by client, and prints no client MAC
address. It reads no nonce, MIC, or key data. Decryption and key derivation stay
prohibited; see [Prohibited use](#prohibited-use).

## Capture modes in scope

The shipped capture modes are Beacon and EAPOL. The capture-mode contract lets
the author add more advanced modes later, each under the same four limits below.
See the [capture modes design](capture-modes-design.md#admission-checklist) for
the contract and the admission checklist a new mode must pass.

The EAPOL mode captures the EAPOL frames of the WPA 4-way handshake. Handshake
capture shows what an attacker could collect toward an offline password-cracking
attempt, so the operator can judge that risk on their own networks.

Handshake and EAPOL capture are in scope only under all four limits below. The
same limits bind every passive mode. The active mode transmits and is governed by
the [active operation](#active-operation) section, which adds its own limits.

| Limit | Rule |
| --- | --- |
| Own or permitted networks | The mode captures only the round's approved access points, one to five BSSIDs, which the operator owns or has written permission to test. |
| Research use | The capture is a test-only artefact for a security assessment. |
| Receive-only | In the passive build the radio stays in `WIFI_MODE_NULL`, and the firmware has no transmit call. The active mode is the single exception; see [active operation](#active-operation). |
| No decryption | A mode decrypts nothing and derives no key, PMK, PTK, PMKID, or passphrase. It saves only the raw frame bytes it receives. |

A passive handshake mode captures a handshake only when a client joins on its
own. It never forces a reconnect, because the passive build never transmits. The
active mode forces the reconnect; see [active operation](#active-operation). No
mode cracks a password, and the project provides nothing that does.

Raw handshake frames contain key-derived values: EAPOL message 1 can carry the
PMKID, and messages 2 to 4 carry the MIC. Either value allows an offline guess of
the passphrase. Treat a handshake capture as being as sensitive as the passphrase
itself.

A handshake capture on an own or permitted network can record a client's MAC
address and EAPOL frames. That client traffic is in scope because it belongs to
the authorized target network. Client MAC addresses never reach the Flipper
screen. Targeting a third party, or collecting their credentials, traffic, or
location, stays prohibited.

## Intended operator

The author is the intended operator. The author runs AirRounds for personal,
noncommercial research only, on a network the author owns or holds written,
time-bounded permission from its owner to test. Anyone else who runs AirRounds
does so outside its intended use. The same rules then bind them. They carry full
responsibility for every session. This policy does not provide for use by an
organization, including a noncommercial one. A paid assessment, a client
engagement, or any other commercial use is prohibited by this policy, and for an
individual it is also outside the license; see [Licensing](#licensing).

The repository grants no permission to any third party to run AirRounds against
any network. Possession of the source or a built image is not authorization.

This policy states how AirRounds may be used. It does not add conditions to the
license. The law and the network owner's permission bind the operator
independently of the license.

## Authorized perimeter

AirRounds runs only inside a restricted, authorized perimeter: networks the
operator owns, or networks whose owner has given the operator written permission
to test, for the duration of that permission.

| Target | Allowed |
| --- | --- |
| A network the operator owns | Yes |
| A network whose owner gave written, time-bounded permission | Yes, within that scope |
| Any other network | No |

The operator confirms every approved access point is in scope before every
round. During a Beacon capture, the firmware drops frames from any BSSID other
than the access point it is visiting. Every new mode must do the same before it
is admitted. That filter is a scope aid, not a legal boundary. The operator
chooses the access points, so the operator owns the decision that they are in
scope.

### Scan

A scan helps the operator find their own access points. During a bounded scan,
the radio receives every management frame in range, including frames from
networks and devices the operator does not own. The firmware keeps only beacons
for the scan list and discards every other frame. The Flipper shows up to 16
networks by name.

- A scan writes no capture. Nothing from a scan reaches the Devboard microSD
  card, and the Flipper app keeps the scan list in memory only.
- A scan is a selection step only. Use it to find and select an own or permitted
  access point, and for nothing else.
- Do not select, record, or note a network from the scan list that is outside
  the authorized perimeter.

## Prohibited use

The following are prohibited without exception. They hold even where the code or
the hardware would technically allow them.

- Running AirRounds against a network the operator does not own or is not
  permitted in writing to test. This is the strictest rule, and nothing
  overrides it.
- Transmitting any frame, except the targeted unicast deauthentication frame the
  active mode sends in the separate opt-in build under
  [active operation](#active-operation). That frame hits only the targeted client.
  A beacon frame, a probe frame, and a broadcast deauthentication frame stay
  prohibited in every build. The passive build adds no transmit path.
- Decrypting captured traffic, or deriving a key, PMK, PTK, PMKID, or passphrase
  from a capture. This includes an offline guess of the passphrase.
- Capturing to collect another person's credentials, traffic content, or
  location, or to track a device or a person.
- Moving a capture to a device or service the operator does not control. See
  [Data handling](#data-handling).
- Any commercial use, including paid security assessments.

## Technical safeguards

Each safeguard below backs a line in the policy. Each claim names the source
that proves it, so a reviewer can check the claim against the code. These
safeguards describe the default passive build. The active build's transmit
confinement has its own safeguards in [active operation](#active-operation).

| Safeguard | Claim | Evidence |
| --- | --- | --- |
| Receive-only radio | The radio starts in `WIFI_MODE_NULL` with a promiscuous receive callback. The source adds no transmit path. | `PassiveVisit::openRadio` in `firmware/esp32/airrounds_uart/passive_visit.h`; [T9 security assurance gap](t9.md#security-assurance-gap) |
| No transmit call | The firmware source holds no frame-transmit call such as `esp_wifi_80211_tx`. | A search of `firmware/esp32/` finds none; [capture modes design, Invariants](capture-modes-design.md#invariants) |
| Build gate | A host test fails the build when `firmware/esp32/**` contains a transmit call or a non-`WIFI_MODE_NULL` radio mode, through an allowlist of the radio calls the firmware uses. | `tests/mac/test_receive_only.py` |
| Declared frame types only | The receive filter and the PCAP writer pass only the frame types a mode declares. | `PassiveVisit::receiveFilter` in `passive_visit.h`; `writePcapRecords` in `firmware/esp32/storage/pcap_record.h` |
| EAPOL writer gate | A second, independent check keyed off the declared data type requires every data record to carry the EAPOL ethertype, so a parser defect in the mode cannot save another data frame. | `containsEapolEthertype` in `firmware/esp32/storage/pcap_record.h` |
| Visited access point only | Beacon and EAPOL drop every frame whose BSSID is not the access point they are visiting. | `PassiveSession::receive` in `firmware/esp32/session/passive_session.h`; `EapolMode::onFrame` in `firmware/esp32/session/eapol_mode.h`; [admission checklist](capture-modes-design.md#admission-checklist) for new modes |
| Scan writes no capture | A scan keeps beacons for the scan list and never mounts the microSD card. | `PassiveVisit::startScan`, `pollScan`, and `finishScan` in `passive_visit.h` |

## Known limits

The no-transmission property rests on the source and a source-level build gate
today, not on radio measurement. State the limits plainly, and do not overclaim.

- The independent RF measurement that would confirm radio silence on the bench is
  deferred. Until it runs, the no-transmission property rests on the source and
  the build gate, not on a measurement. See
  [T9 security assurance gap](t9.md#security-assurance-gap).
- The build gate reads the source, not the built image. It fails on an unreviewed
  radio call, but a transmit path through a call outside its allowlist namespaces
  would need a reviewer to catch it. See `tests/mac/test_receive_only.py`.
- The access point filter is a scope aid, not an enforced legal boundary. The
  operator selects the access points, so the operator is accountable for the
  choice.
- A capture holds the raw frames it accepts. A beacon capture holds broadcast
  beacon data. A handshake capture also holds client MAC addresses and
  key-derived values that allow an offline guess of the passphrase. Treat every
  capture as sensitive. See [Data handling](#data-handling).

## Active operation

The active mode is the one part of AirRounds that transmits. It sends a targeted
deauthentication frame to force a client off an approved access point, so the
client rejoins and produces a WPA handshake the mode captures. It realises the
Todo B capture-mode slot. Its requirements are in [T12](t12.md).

The active mode exists only in a separate, opt-in transmit build. The author
builds it deliberately and runs it only within this section. The shipped default
build has no active mode and no transmit path, and the receive-only safeguards
above describe that default build.

The passive four limits still bind the active mode, except that it transmits
under the limits below. The passive capture of the handshake, the no-decryption
rule, and the own-or-permitted-network rule all still hold. The mode captures the
handshake with the same target-access-point filter as passive EAPOL. Only the
transmit side is new.

| Limit | Rule |
| --- | --- |
| Approved target only | The mode transmits only to a client on one of the round's approved access points, which the operator owns or has written permission to test for active transmission. |
| Unicast only | The mode sends a deauthentication frame to one client address. It never broadcasts a deauthentication frame, and it sends no other frame type. |
| Bounded transmit | The mode sends a small burst, and at most two bursts per visit, under a hard per-round transmit cap. It never streams continuously. |
| Auto-stop | The mode stops transmitting once it captures a handshake, within one poll cycle. A kill switch stops it at any time. |
| Audit | The journal records the transmit per point: the frame count, the reason, and the target client on a `tx` line, with the target access point, the channel, and the listen window on the point line. |

### Operator attestation

Before an active round, the operator confirms all of the following. The operator
owns the decision and the responsibility.

- The operator owns the target network, or holds written, time-bounded permission
  that names active transmission, not testing alone.
- The target client is not a safety-critical or mission-critical device. A
  deauthentication burst disrupts a device that depends on the connection, such as
  a camera, a voice phone, or a telemetry sensor.
- The operator accepts the legal risk below.

### Legal and regulatory

Deauthentication is an active interference with a radio link, not a passive
capture. It carries a heavier legal exposure than the rest of AirRounds.

- The FCC treats Wi-Fi deauthentication as prohibited jamming under 47 U.S.C.
  section 333. The enforcement record is strongest for deauthenticating devices on
  a network the operator does not own. The operator carries this risk on every
  active round.
- The law on radio interference binds the operator independently of this policy
  and of the license. When the law and this policy disagree, the stricter rule
  wins.
- This document is not legal advice. For an active test, confirm the legal basis
  before the round.

### Transmit confinement

The active build cannot prove the absence of a transmit capability, because it
has one. It proves instead that the transmit path is confined. State this limit
plainly, and do not claim the passive build's stronger property for the active
build. Each claim names the source that proves it, as the passive safeguards do.

| Safeguard | Claim | Evidence |
| --- | --- | --- |
| Separate build | The transmit path compiles only into the opt-in active build; the default build excludes it. | `tools/quality/embedded.py` builds `firmware/active/airrounds_active` behind `AIRROUNDS_ACTIVE`; the default build links no `firmware/active` source. |
| Confined transmit | The active build holds its one transmit call in one reviewed file; a gate variant proves no other file transmits. | `DeauthTransmitter::send` in `firmware/active/deauth_tx.h`; `tests/mac/test_active_transmit.py` |
| No stray include | No `firmware/esp32` file includes a `firmware/active` header, so the default image cannot pull in the transmit path. | `tests/mac/test_receive_only.py` |
| One frame type | The mode transmits only a unicast deauthentication frame; it builds no other frame. | `buildDeauthFrame` in `firmware/active/deauth_frame.h`; `ActiveEapolMode::poll` in `firmware/active/active_eapol_mode.h` |
| Bounded by construction | The burst size, the per-visit burst count, and the per-round cap are compile-time constants, not operator settings. | `ActiveEapolMode` constants; `transmitCap` in `firmware/esp32/uart/passive_round.h` |

These safeguards rest on the source and the source-level gates, not on an RF
measurement. The [open hardware bench](t12.md#hardware-bench) confirms the
transmit on the Devboard, including that the station-mode radio emits nothing but
the deauthentication burst.

### Reuse

The active mode is clean-room original work. pwnagotchi and esp32-marauder
implement the same technique under GPL-3.0, which the project cannot copy; see
[adding third-party code](#adding-third-party-code). A deauthentication frame is a
fixed structure defined by the IEEE 802.11 standard, so the mode is written from
the standard, not from a third-party implementation.

## Data handling

A capture can identify devices and, through them, people. Handle it as sensitive
data.

- Keep captures only on the Devboard microSD card and on devices the operator
  controls, such as the operator's Mac. Do not upload a capture to a service or
  share it with anyone else.
- Keep captures only until the assessment they belong to ends, then delete them.
- Do not use a capture to identify, track, or profile a device or a person.

## Operator responsibility

The operator carries full responsibility for every session: the choice of
target, the legal basis for the test, the handling of the captured data, and
compliance with the law that applies to them. The
[disclaimer](../README.md#disclaimer) in the README applies to AirRounds and
every use of it.

Follow all applicable laws on interception, privacy, and radio emission.

When the law and this policy disagree, the stricter rule wins.

## Licensing

AirRounds is released under the PolyForm Noncommercial License 1.0.0. The
license lets you use, study, modify, and share it for any noncommercial purpose,
and it also permits use by the noncommercial organizations it lists, such as
charities and educational institutions. This policy is narrower: it provides for
the author's own research only. Commercial use is not permitted, except for the
components that [NOTICE](../NOTICE) lists under their own terms.

This document is not legal advice. For a commercially important decision, have a
lawyer confirm it.

| File | Owns |
| --- | --- |
| [LICENSE](../LICENSE) | The full PolyForm Noncommercial License 1.0.0 text and the required notice |
| [NOTICE](../NOTICE) | Every third-party or separately licensed component, its source, its license, and the full license text or the path of a tracked copy |
| [CONTRIBUTING](../CONTRIBUTING.md) | The license under which the project accepts contributions |
| This document | The use policy and the limits of the noncommercial restriction |

Keep the required notice with every copy you distribute:

```text
Required Notice: Copyright 2026 Pavel Zaitsau
```

### What the noncommercial restriction covers

The PolyForm Noncommercial restriction applies to the original AirRounds code.
It does not override the license of a component listed in [NOTICE](../NOTICE).
Each such component keeps its own license.

The PolyForm license does not stop the copyright holder from offering AirRounds
under other terms later, such as a commercial or an open-source license. To keep
that option, external contributions come in under MIT-0; see
[CONTRIBUTING](../CONTRIBUTING.md).

PolyForm Noncommercial is not an OSI-approved open-source license. It carries a
field-of-use restriction, so AirRounds does not qualify as open source, and some
directories and package indexes reject it.

### Distribution

The project distributes source code only. It publishes no built Flipper app
(`.fap`) and no built firmware image (`.bin`). The author builds both for
personal use.

A build links platform code under other licenses: the Flipper Zero firmware SDK
(GPL-3.0), Arduino-ESP32 (LGPL-2.1), and ESP-IDF (Apache-2.0). The source
repository does not contain that code. Do not publish a built image. Publishing
one needs a separate license review first, because the image combines AirRounds
code with that platform code.

### Adding third-party code

The rules in this section apply to code built into the firmware, the Flipper
app, the Mac tools, or the tests. The development skills under `.agents/skills/`
are separate tooling with their own licenses, listed in [NOTICE](../NOTICE), and
are never combined with AirRounds code.

AirRounds can take third-party code under a permissive license, such as MIT,
BSD, or Apache-2.0. A permissive license allows redistribution inside a project
with more restrictive terms, so the noncommercial restriction stays valid for the
AirRounds code around it.

Do not add code under GPL or another strong copyleft license. GPL-3.0 section 10
forbids any further restriction on a derivative work. GPL code makes the
noncommercial restriction impossible for the whole program it is combined into.

When you add permissive third-party code, do all of the following in the same
change:

1. Add an entry to [NOTICE](../NOTICE) with the component, its source, its
   license, and the full license text or the path of a tracked copy.
1. Keep the upstream copyright line and license text in the copied files.
   Removing them breaks the upstream license condition.
1. Mark each copied file with an SPDX identifier, as the next section shows.
1. For Apache-2.0 code, mark every file you change as changed, and copy the
   attributions from the upstream `NOTICE` file, if it has one, into
   [NOTICE](../NOTICE).

A file that mixes upstream code with original AirRounds code is a mixed-license
file. The upstream lines keep their license. Your new lines are under PolyForm
Noncommercial. Marking such a file with the upstream license alone licenses your
additions under that license, which defeats the noncommercial restriction.

### SPDX identifiers

Add an SPDX identifier to every new source file. Existing source files carry no
identifier yet; the root [LICENSE](../LICENSE) covers them.

| File | Identifier |
| --- | --- |
| Original AirRounds code | `SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0` |
| Unmodified third-party code | The upstream identifier, for example `SPDX-License-Identifier: MIT`, with the upstream copyright line |
| Third-party code with AirRounds changes | Both identifiers, for example `SPDX-License-Identifier: MIT AND PolyForm-Noncommercial-1.0.0`, with the upstream copyright line and a header that says which lines are under which license |
