// This test compiles the active-build journal path by defining AIRROUNDS_ACTIVE before the include.
// The default-build journal (format v3, radio=passive, no tx audit) is covered by
// tests/uart/session_journal_test.cpp, which the gate compiles without the define.
#define AIRROUNDS_ACTIVE 1

// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "session_journal.h"

using namespace airrounds::uart;

int main() {
    SessionReport report;
    report.id = {0x1234, 7};
    report.state = State::complete;
    report.outcome = Outcome::saved;
    report.status.mode = activeModeId;
    const bool added = report.status.profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);
    report.status.results[0] = Outcome::saved;
    report.budget = {100, 60000000};
    report.endedUs = 5000000;

    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    stats[0].listenStartedUs = 100;
    stats[0].listenEndedUs = 5000000;
    stats[0].txFrames = 3;
    stats[0].txReason = 7;
    const uint8_t client[6] = {0x12, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(stats[0].txClient, client, sizeof(client));

    char text[2304];
    const size_t size =
        detail::formatSessionJournal(text, sizeof(text), report, stats, {"build-id", "revision"});
    assert(size > 0);
    const std::string out(text, size);

    // The active build marks the journal v4, records the radio as active, and names the mode.
    assert(out.find("format=airrounds-session-v4\n") != std::string::npos);
    assert(out.find(" radio=active mode=active\n") != std::string::npos);
    // The transmit audit names the frame count, the reason, and the target client.
    assert(out.find("tx point=1 frames=3 reason=7 client=122233445566\n") != std::string::npos);

    // A point that transmitted nothing records a zero audit, never a stale client.
    report.id.session = 8;
    report.status.results[0] = Outcome::not_observed;
    VisitStatistics quiet[airrounds::session::Profile::capacity]{};
    const size_t quietSize =
        detail::formatSessionJournal(text, sizeof(text), report, quiet, {"build-id", "revision"});
    assert(quietSize > 0);
    const std::string quietOut(text, quietSize);
    assert(quietOut.find("tx point=1 frames=0 reason=0 client=000000000000\n") != std::string::npos);

    // A full five-point round fits the 2304-byte journal buffer with a tx line per point.
    SessionReport five;
    five.id = {0x1234, 9};
    five.state = State::complete;
    five.outcome = Outcome::saved;
    five.status.mode = activeModeId;
    for (uint8_t point = 1; point <= 5; ++point) {
        assert(five.status.profile.add({{2, 0, 0, 0, 0, point}, 6}));
        five.status.results[point - 1] = Outcome::saved;
    }
    five.budget = {100, 300000000};
    five.endedUs = 300000000;
    VisitStatistics busy[airrounds::session::Profile::capacity]{};
    for (size_t point = 0; point < 5; ++point) {
        busy[point].txFrames = 6;
        busy[point].txReason = 7;
        memcpy(busy[point].txClient, client, sizeof(client));
    }
    const size_t fiveSize =
        detail::formatSessionJournal(text, sizeof(text), five, busy, {"build-id", "revision"});
    assert(fiveSize > 0 && fiveSize < sizeof(text));
    const std::string fiveOut(text, fiveSize);
    for (int point = 1; point <= 5; ++point) {
        const std::string line =
            "tx point=" + std::to_string(point) + " frames=6 reason=7 client=122233445566\n";
        assert(fiveOut.find(line) != std::string::npos);
    }

    std::cout << "active journal tests passed\n";
    return 0;
}
