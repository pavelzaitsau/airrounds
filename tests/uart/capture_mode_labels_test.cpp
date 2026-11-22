// The checks below must run in every build, including one with NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../../flipper/network_list.h"
#include "../../flipper/ui_state.h"

namespace {

using airrounds::flipper::BoardModes;
using airrounds::flipper::CaptureModeInfo;
using airrounds::flipper::SuccessMeaning;
using airrounds::uart::Outcome;
using airrounds::uart::State;

bool same(const char* first, const char* second) {
    return std::strcmp(first, second) == 0;
}

// An absence mode as a future entry would declare it; the table has none yet.
constexpr CaptureModeInfo absenceMode{"Absence",
                                      "Points",
                                      "Absent",
                                      "Present",
                                      "Listening",
                                      "Saving",
                                      "Absent %u/%u",
                                      "Absent",
                                      "Present",
                                      "Absence confirmed",
                                      "Frames present",
                                      SuccessMeaning::absence_confirmed,
                                      0x05,
                                      false,
                                      true};

void testModeLookup() {
    using airrounds::flipper::modeOf;
    assert(&modeOf(0x00) == &airrounds::flipper::beaconMode());
    assert(same(modeOf(0x00).listTitle, "Beacons"));
    assert(same(modeOf(0x01).name, "EAPOL") && same(modeOf(0x01).listTitle, "Handshakes"));
    assert(same(modeOf(0x02).name, "Todo B"));
    // A mode the app does not know gets the neutral labels.
    const auto& unknown = modeOf(0x42);
    assert(&unknown == &airrounds::flipper::neutralMode);
    assert(same(unknown.listTitle, "Points") && same(unknown.rowSuccess, "Done") &&
           same(unknown.rowMiss, "No result") && same(unknown.counter, "Done %u/%u"));
}

void testAvailability() {
    using airrounds::flipper::captureModes;
    using airrounds::flipper::modeAvailable;
    BoardModes none;
    assert(modeAvailable(captureModes[0], none));
    assert(!modeAvailable(captureModes[1], none) && !modeAvailable(captureModes[2], none));

    BoardModes listed;
    listed.count = 2;
    listed.ids[0] = 0x00;
    listed.ids[1] = 0x01;
    // EAPOL is implemented and offered only when the board lists 01.
    assert(modeAvailable(captureModes[1], listed));
    // Todo B stays unavailable until the app implements it, even when the board lists it.
    BoardModes listsTodoB;
    listsTodoB.count = 1;
    listsTodoB.ids[0] = 0x02;
    assert(!modeAvailable(captureModes[2], listsTodoB));
    BoardModes absenceListed;
    absenceListed.count = 1;
    absenceListed.ids[0] = 0x05;
    assert(modeAvailable(absenceMode, absenceListed));
    assert(!modeAvailable(absenceMode, listed));
}

void testLabelSlots() {
    using airrounds::flipper::pointDetail;
    using airrounds::flipper::pointResult;
    const auto& beacon = airrounds::flipper::beaconMode();
    const auto& neutral = airrounds::flipper::neutralMode;

    // A frame-found mode: saved is a success, not_observed a miss.
    assert(same(pointResult(Outcome::saved, State::complete, beacon), "Captured"));
    assert(same(pointResult(Outcome::not_observed, State::complete, beacon), "Not seen"));
    assert(same(pointDetail(Outcome::saved, State::complete, beacon), "Captured to board SD"));
    assert(same(pointDetail(Outcome::not_observed, State::complete, beacon), "Point not seen"));

    // An absence mode: a saved frame disproves the absence.
    assert(same(pointResult(Outcome::saved, State::complete, absenceMode), "Present"));
    assert(same(pointDetail(Outcome::saved, State::complete, absenceMode), "Frames present"));
    assert(same(pointResult(Outcome::not_observed, State::complete, absenceMode), "Present"));

    // The neutral entry counts a saved frame as a success.
    assert(same(pointResult(Outcome::saved, State::complete, neutral), "Done"));
    assert(same(pointResult(Outcome::not_observed, State::complete, neutral), "No result"));

    // Errors and missing results keep their labels in every mode.
    for (const auto* mode : {&beacon, &neutral, &absenceMode}) {
        assert(same(pointResult(Outcome::radio_error, State::error, *mode), "Radio error"));
        assert(same(pointResult(Outcome::storage_error, State::error, *mode), "SD error"));
        assert(same(pointDetail(Outcome::storage_error, State::error, *mode), "SD write failed"));
        assert(same(pointResult(Outcome::none, State::stopped, *mode), "Stopped"));
    }
}

void testListRows() {
    using airrounds::flipper::networkRowStatus;
    const auto& beacon = airrounds::flipper::beaconMode();
    const auto& neutral = airrounds::flipper::neutralMode;
    // The Beacons list rows: the same strings as at T9 for Beacon.
    assert(same(networkRowStatus(Outcome::none, beacon), ""));
    assert(same(networkRowStatus(Outcome::saved, beacon), "Saved"));
    assert(same(networkRowStatus(Outcome::not_observed, beacon), "Not seen"));
    assert(same(networkRowStatus(Outcome::storage_error, beacon), "SD error"));
    assert(same(networkRowStatus(Outcome::radio_error, beacon), "Radio error"));
    // An absence mode: a saved frame disproves the absence.
    assert(same(networkRowStatus(Outcome::saved, absenceMode), "Present"));
    assert(same(networkRowStatus(Outcome::not_observed, absenceMode), "Present"));
    // The neutral entry counts a saved frame as a success.
    assert(same(networkRowStatus(Outcome::saved, neutral), "Done"));
    assert(same(networkRowStatus(Outcome::not_observed, neutral), "No result"));
}

void testClearOutcomes() {
    airrounds::uart::ScanList scan;
    const airrounds::session::Target first{{2, 0, 0, 0, 0, 1}, 6};
    const airrounds::session::Target second{{2, 0, 0, 0, 0, 2}, 11};
    const uint8_t name[] = {'L', 'a', 'b'};
    const bool firstSeen = scan.observe(first, -40, name, sizeof(name));
    const bool secondSeen = scan.observe(second, -50, name, sizeof(name));
    assert(firstSeen && secondSeen);

    airrounds::flipper::NetworkList networks;
    networks.replace(scan);
    const bool toggled = networks.toggle(1);
    assert(toggled);
    airrounds::uart::RoundStatus round;
    const bool added = round.profile.add(first);
    assert(added);
    round.results[0] = Outcome::saved;
    networks.record(round, State::complete);
    assert(networks.outcome(networks.find(first)) == Outcome::saved);

    // A mode change clears the results and keeps the list and the selection.
    networks.clearOutcomes();
    assert(networks.size() == 2);
    assert(networks.outcome(0) == Outcome::none && networks.outcome(1) == Outcome::none);
    assert(networks.selected(networks.find(second)));
}

// Result digit 5 (absence_confirmed): a Miss for a frame-found mode (Beacon, EAPOL), a Success for
// an absence mode and an unknown mode. The success counter follows the same mapping.
void testDigitFive() {
    using airrounds::flipper::networkRowStatus;
    using airrounds::flipper::pointDetail;
    using airrounds::flipper::pointResult;
    using airrounds::flipper::RunReport;
    using airrounds::flipper::savedPoints;
    const auto& beacon = airrounds::flipper::beaconMode();
    const auto& eapol = airrounds::flipper::captureModes[1];
    const auto& neutral = airrounds::flipper::neutralMode;
    assert(same(eapol.name, "EAPOL"));

    // EAPOL success labels in every slot.
    assert(same(pointResult(Outcome::saved, State::complete, eapol), "Captured"));
    assert(same(pointResult(Outcome::not_observed, State::complete, eapol), "Not seen"));
    assert(same(pointDetail(Outcome::saved, State::complete, eapol), "Handshake to board SD"));
    assert(same(networkRowStatus(Outcome::saved, eapol), "Saved"));
    assert(same(networkRowStatus(Outcome::not_observed, eapol), "Not seen"));
    assert(same(eapol.listTitle, "Handshakes") && same(eapol.counter, "Captured %u/%u"));

    assert(same(pointResult(Outcome::absence_confirmed, State::complete, beacon), "Not seen"));
    assert(same(pointResult(Outcome::absence_confirmed, State::complete, eapol), "Not seen"));
    assert(same(pointResult(Outcome::absence_confirmed, State::complete, absenceMode), "Absent"));
    assert(same(pointResult(Outcome::absence_confirmed, State::complete, neutral), "Done"));

    assert(same(networkRowStatus(Outcome::absence_confirmed, beacon), "Not seen"));
    assert(same(networkRowStatus(Outcome::absence_confirmed, absenceMode), "Absent"));
    assert(same(networkRowStatus(Outcome::absence_confirmed, neutral), "Done"));

    assert(same(pointDetail(Outcome::absence_confirmed, State::complete, eapol), "No handshake"));
    assert(same(pointDetail(Outcome::absence_confirmed, State::complete, absenceMode),
               "Absence confirmed"));

    // The counter counts a point as a success by the mode's meaning.
    RunReport report;
    report.state = State::complete;
    const airrounds::session::Target targets[3] = {
        {{2, 0, 0, 0, 0, 1}, 6}, {{2, 0, 0, 0, 0, 2}, 6}, {{2, 0, 0, 0, 0, 3}, 6}};
    for (const auto& target : targets) {
        const bool added = report.round.profile.add(target);
        assert(added);
    }
    report.round.results[0] = Outcome::saved;
    report.round.results[1] = Outcome::not_observed;
    report.round.results[2] = Outcome::absence_confirmed;
    assert(savedPoints(report, beacon) == 1);      // only saved
    assert(savedPoints(report, eapol) == 1);       // only saved
    assert(savedPoints(report, absenceMode) == 1); // only digit 5
    assert(savedPoints(report, neutral) == 2);     // saved and digit 5
}

// Each row of the lock table in docs/ui-ux/capture-modes.md, in its order of priority.
void testModeLocks() {
    using airrounds::flipper::captureModeLock;
    using airrounds::flipper::Link;
    using airrounds::flipper::ModeLockInput;
    using airrounds::flipper::Request;
    using airrounds::uart::ScanState;

    const ModeLockInput idle;
    assert(captureModeLock(idle) == nullptr);

    // Board state unknown: every unconfirmed state, even with a known round and scan.
    const auto busy = [] {
        ModeLockInput board;
        board.session = State::running;
        board.scan = ScanState::running;
        return board;
    };
    for (const auto link : {Link::connecting, Link::lost, Link::incompatible}) {
        auto board = busy();
        board.link = link;
        assert(same(captureModeLock(board), "Board state unknown"));
    }
    for (const auto pending : {Request::start, Request::stop}) {
        auto board = busy();
        board.pending = pending;
        assert(same(captureModeLock(board), "Board state unknown"));
    }
    auto unconfirmed = busy();
    unconfirmed.commandUnconfirmed = true;
    assert(same(captureModeLock(unconfirmed), "Board state unknown"));
    auto closed = busy();
    closed.uartOpen = false;
    assert(same(captureModeLock(closed), "Board state unknown"));

    // Session active: the board runs or saves a round, and wins over a scan.
    for (const auto session : {State::running, State::finalizing}) {
        ModeLockInput board;
        board.session = session;
        board.scan = ScanState::running;
        assert(same(captureModeLock(board), "Session active"));
    }

    // Scan active: a scan runs, or a scan or scan-cancel request waits for a reply.
    ModeLockInput scanning;
    scanning.scan = ScanState::running;
    assert(same(captureModeLock(scanning), "Scan active"));
    for (const auto pending : {Request::scan, Request::scan_stop}) {
        ModeLockInput board;
        board.pending = pending;
        assert(same(captureModeLock(board), "Scan active"));
    }

    // A status read, a finished round, or a finished scan does not lock.
    ModeLockInput reading;
    reading.pending = Request::status;
    reading.session = State::complete;
    reading.scan = ScanState::complete;
    assert(captureModeLock(reading) == nullptr);
}

} // namespace

int main() {
    testModeLookup();
    testAvailability();
    testLabelSlots();
    testListRows();
    testClearOutcomes();
    testDigitFive();
    testModeLocks();
    std::cout << "Capture mode labels: lookup, availability, label slots, list rows, result reset, "
                 "and locks passed\n";
    return EXIT_SUCCESS;
}
