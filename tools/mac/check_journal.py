#!/usr/bin/env python3
"""Verify the transmit audit in an AirRounds session journal.

The active build records every deauthentication burst in the session journal (format v4). This
checks that the audit describes only the bounded, targeted transmit the policy permits: a unicast
client, within the per-round cap, with a reason code. It reads the journal only; it never transmits
or changes anything. See docs/t12-architecture.md and docs/acceptable-use.md, "Active operation".

A passive journal (format v3) holds no transmit audit; without --require-audit this check passes it,
because a passive capture transmitted nothing to audit.
"""

import argparse
import re
import sys

# The transmit caps, kept in step with the firmware: a visit sends at most two bursts of three
# frames (ActiveEapolMode::burstFrames * maxBursts), and a round at most five points' worth
# (transmitCap in firmware/esp32/uart/passive_round.h). The audit counts frames the radio accepted
# for transmission (a successful esp_wifi_80211_tx), not RF-confirmed emission.
PER_VISIT_CAP = 6
ROUND_CAP = 30

TX_LINE = re.compile(r"^tx point=(\d+) frames=(\d+) reason=(\d+) client=([0-9a-f]{12})$")
BROADCAST = "ffffffffffff"
ZERO = "000000000000"


def audit_problems(path):
    """Return (saw_audit, [problem, ...]) for the journal at path."""
    problems = []
    total = 0
    saw_audit = False
    with open(path, encoding="ascii", errors="replace") as handle:
        for number, raw in enumerate(handle, 1):
            stripped = raw.rstrip("\n")
            match = TX_LINE.match(stripped)
            if match is None:
                # A tx-prefixed line that fails the strict format is a corrupt audit, not noise.
                if stripped.startswith("tx "):
                    problems.append(f"line {number}: malformed transmit audit line")
                continue
            saw_audit = True
            frames = int(match.group(2))
            client = match.group(4)
            total += frames
            if frames > PER_VISIT_CAP:
                problems.append(
                    f"line {number}: {frames} frames exceeds the per-visit cap of {PER_VISIT_CAP}")
            if frames == 0:
                continue
            if int(client[0:2], 16) & 1:
                problems.append(f"line {number}: client {client} is a group address")
            if client == ZERO:
                problems.append(f"line {number}: {frames} frames transmitted with a zero client")
            if client == BROADCAST:
                problems.append(f"line {number}: broadcast client; a broadcast deauth is prohibited")
    if total > ROUND_CAP:
        problems.append(f"transmitted {total} frames, over the per-round cap of {ROUND_CAP}")
    return saw_audit, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal")
    parser.add_argument("--require-audit", action="store_true",
                        help="fail when the journal has no transmit audit (an active-mode journal)")
    arguments = parser.parse_args()
    saw_audit, problems = audit_problems(arguments.journal)
    if arguments.require_audit and not saw_audit:
        problems.append("no transmit audit found; this is not an active-mode journal")
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        raise SystemExit(1)
    print(f"transmit audit ok: {arguments.journal}")


if __name__ == "__main__":
    main()
