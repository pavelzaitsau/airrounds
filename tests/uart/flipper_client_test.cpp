// The checks below must run in every build, including one with NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../../flipper/ui_state.h"
#include "uart_protocol.h"

namespace {
using airrounds::flipper::Link;
using airrounds::flipper::Request;
using airrounds::flipper::SessionClient;
using namespace airrounds::uart;

constexpr airrounds::session::Target target{{2, 0, 0, 0, 0, 1}, 6};
constexpr uint64_t boot = 0x123456789abcdef0ULL;

class Visit final : public VisitBackend {
public:
    uint64_t nowUs() const override { return 0; }
    VisitStart start(const airrounds::session::Target& selected, const VisitBudget&) override {
        assert(airrounds::session::validTarget(selected));
        ++starts;
        done = false;
        outcome = Outcome::none;
        return VisitStart::listening;
    }
    bool observationDone() const override { return done; }
    bool stopReception() override {
        ++stops;
        return true;
    }
    void beginSave(const CaptureId&) override {}
    Outcome saveResult() const override { return outcome; }
    bool supportsJournal() const override { return journalSupported; }
    JournalState journalState() const override { return journal; }
    bool journalSupported = false;
    JournalState journal = JournalState::none;

    unsigned starts = 0;
    unsigned stops = 0;
    bool done = false;
    Outcome outcome = Outcome::none;
};

class TestScan final : public ScanBackend {
public:
    TestScan() {
        const char* names[]{"Studio", "", "Loft"};
        for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
            const airrounds::session::Target entry{{2, 0, 0, 0, 1, static_cast<uint8_t>(index + 1)},
                                                   static_cast<uint8_t>(index + 1)};
            const bool added = list.add(entry, reinterpret_cast<const uint8_t*>(names[index]),
                                        strlen(names[index]));
            assert(added);
        }
    }
    void startScan() override {
        state = ScanState::running;
        ++starts;
    }
    void pollScan() override {}
    void stopScan() override {
        state = cleanupFails ? ScanState::error : ScanState::idle;
        ++stops;
    }
    ScanState scanState() const override { return state; }
    const ScanList& scanList() const override { return list; }
    // The board reads the state through the backend, which static analysis cannot follow.
    void set(ScanState next) { state = next; }

    ScanState state = ScanState::idle;
    bool cleanupFails = false;
    ScanList list;
    unsigned starts = 0;
    unsigned stops = 0;
};

// Replace one part of a reply line. The part must be there, or the check proves nothing.
std::string tamper(std::string line, const std::string& from, const std::string& to) {
    const auto at = line.find(from);
    if (at == std::string::npos) {
        std::cerr << "No [" << from << "] in " << line;
        std::abort();
    }
    return line.replace(at, from.size(), to);
}

class Loopback {
public:
    explicit Loopback(ScanBackend* scan = nullptr)
        : client(write, this, 0x42), board(visit, boot, scan), scanning(scan != nullptr) {}

    // The board that answers. A test can swap in a restarted board.
    std::string deliver() { return deliverTo(target != nullptr ? *target : board); }

    std::string deliverTo(Protocol& receiver) {
        std::string reply;
        for (char byte : outbound) {
            char line[Protocol::replyCapacity];
            if (receiver.receive({static_cast<uint8_t>(byte), now}, line)) {
                reply += line;
            }
        }
        outbound.clear();
        return reply;
    }

    void receive(const std::string& reply) {
        for (char byte : reply) {
            client.receive({static_cast<uint8_t>(byte), now});
        }
    }

    void handshake(bool rounds = true) {
        client.poll(now);
        assert(outbound.find(" hello\n") != std::string::npos);
        auto hello = deliver();
        if (!rounds) {
            hello.replace(hello.size() - 3, 2, "01");
        }
        receive("ROM: reset" + hello);
        assert(client.link() != Link::ready);
        client.poll(now);
        const char* request = scanning && rounds                 ? " round_status_detail\n"
                              : visit.journalSupported && rounds ? " round_status_log\n"
                              : rounds                           ? " round_status\n"
                                                                 : " status\n";
        assert(outbound.find(request) != std::string::npos);
        receive(deliver());
        assert(client.link() == Link::ready);
    }

    static void write(void* context, const char* bytes, size_t size) {
        auto& link = *static_cast<Loopback*>(context);
        assert(link.outbound.empty());
        link.outbound.assign(bytes, size);
    }

    Visit visit;
    SessionClient client;
    Protocol board;
    Protocol* target = nullptr;
    bool scanning;
    uint32_t now = 0;
    std::string outbound;
};

void stopAndSave() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    assert(link.client.state() == State::running);
    assert(link.client.stop(link.client.visitId(), link.now));
    link.receive(link.deliver());
    assert(link.client.state() == State::finalizing);
    assert(link.client.outcome() == Outcome::none);
    assert(!link.client.stop(link.client.visitId(), link.now));
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    link.now += 500;
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.state() == State::stopped);
    assert(link.client.outcome() == Outcome::saved);
    assert(link.visit.starts == 1 && link.visit.stops == 1);
}

void lostStartAndResume() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    const auto lateStart = link.deliver();
    link.now += 1001;
    link.client.poll(link.now);
    assert(link.client.link() == Link::lost);
    assert(link.outbound.empty());
    link.visit.done = true;
    link.visit.outcome = Outcome::not_observed;
    link.board.poll();
    link.now += 1000;
    link.client.poll(link.now);
    link.receive(lateStart);
    assert(link.client.link() == Link::lost);
    link.receive(link.deliver());
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.link() == Link::ready);
    assert(link.client.state() == State::complete);
    assert(link.client.outcome() == Outcome::not_observed);
    assert(link.visit.starts == 1);
}

void newCommandAfterDroppedWrite() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    const auto dropped = link.outbound;
    link.outbound.clear();
    link.now += 950;
    link.client.poll(link.now);
    link.now += 500;
    link.handshake();
    assert(link.visit.starts == 0);
    const bool started2 = link.client.start(target, link.now);
    assert(started2);
    assert(link.outbound != dropped);
    assert(link.outbound.find(" 0000000000000002 00000001 round ") != std::string::npos);
    link.receive(link.deliver());
    assert(link.visit.starts == 1);
}

void stopKeepsTheSelectedVisit() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    const auto selected = link.client.visitId();
    link.visit.done = true;
    link.visit.outcome = Outcome::not_observed;
    link.board.poll();

    // The board starts another visit while the earlier Stop press is held.
    link.outbound = "@01 123456789abcdef0 0000000000000002 00000002 start 020000000001 06\n";
    assert(link.deliver().find("00000002 running none") != std::string::npos);
    link.client.refresh(link.now);
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.canStop());
    assert(!link.client.stop(selected, link.now));
    assert(!link.client.stop({boot + 1, 2}, link.now));
    assert(link.outbound.empty());
    assert(link.client.stop(link.client.visitId(), link.now));
    link.receive(link.deliver());
    assert(link.visit.stops == 2);
}

void commandRejectionStaysVisible() {
    Loopback link;
    link.handshake();
    link.outbound = "@01 123456789abcdef0 0000000000000001 00000001 start 020000000001 06\n";
    assert(link.deliver().find("00000001 running none") != std::string::npos);
    const airrounds::session::Target changed{{2, 0, 0, 0, 0, 2}, 6};
    const bool started = link.client.start(changed, link.now);
    assert(started);
    const auto rejected = link.deliver();
    assert(rejected.find(" conflict ") != std::string::npos);
    link.receive(rejected);
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.state() == State::running);
    assert(link.client.rejected() == Request::start);
    link.client.refresh(link.now);
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.rejected() == Request::none);
    assert(link.visit.starts == 1);
}

void freshStatusAfterReturn() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    link.now += 500;
    link.client.poll(link.now);
    const auto oldStatus = link.deliver();
    link.visit.done = true;
    link.visit.outcome = Outcome::storage_error;
    link.board.poll();
    link.client.refresh(link.now);
    link.client.poll(link.now);
    link.receive(oldStatus);
    assert(link.client.link() != Link::ready);
    link.receive(link.deliver());
    assert(link.client.link() == Link::ready);
    assert(link.client.state() == State::error);
    assert(link.client.outcome() == Outcome::storage_error);

    // Reopen the app with a fresh nonce while the same board stays powered.
    link.client = SessionClient(Loopback::write, &link, 0x90);
    link.handshake();
    assert(link.client.state() == State::error);
    assert(link.client.outcome() == Outcome::storage_error);
}

void noMutationWithoutCompatibleStatus() {
    Loopback link;
    link.client.poll(link.now);
    auto hello = link.deliver();
    hello.replace(1, 2, "02");
    link.receive(hello);
    assert(link.client.link() == Link::incompatible);
    const bool started = link.client.start(target, link.now);
    assert(!started);
    assert(!link.client.stop(link.client.visitId(), link.now));
    link.now += 400;
    link.client.poll(link.now);
    hello = link.deliver();
    hello.replace(hello.size() - 3, 2, "00");
    link.receive(hello);
    assert(link.client.link() == Link::incompatible);
    assert(link.outbound.empty());
}

void loseStopReplyDuringSave() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    assert(link.client.stop(link.client.visitId(), link.now));
    const auto lateStop = link.deliver();
    link.now += 950;
    link.receive(lateStop);
    assert(link.client.link() == Link::lost);
    assert(!link.client.stop(link.client.visitId(), link.now));
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    link.now += 500;
    link.handshake();
    assert(link.client.state() == State::stopped);
    assert(link.client.outcome() == Outcome::saved);
    assert(link.visit.stops == 1);
}
void roundStopWhileSavingAndReconnect() {
    Loopback link;
    link.handshake();
    airrounds::session::Profile profile;
    const bool invalidStarted = link.client.start(profile, link.now);
    assert(!invalidStarted);
    const bool valid = profile.add(target) && profile.add({{2, 0, 0, 0, 0, 2}, 6}) &&
                       profile.add({{2, 0, 0, 0, 0, 3}, 11});
    assert(valid);
    const bool started = link.client.start(profile, link.now);
    assert(started);
    const auto start = link.deliver();
    link.receive(start);
    assert(link.client.round().profile.matches(profile));
    assert(link.client.round().next() == 0);

    link.visit.done = true;
    link.board.poll();
    link.now += 500;
    link.client.poll(link.now);
    link.receive(start);
    assert(link.client.state() == State::running);
    const auto status = link.deliver();
    auto inconsistent = status;
    inconsistent[inconsistent.size() - 2] = '2';
    link.receive(inconsistent);
    assert(link.client.state() == State::running);
    link.receive(status);
    assert(link.client.state() == State::finalizing);
    assert(link.client.stop(link.client.visitId(), link.now));
    const auto lostStop = link.deliver();
    assert(lostStop.find("finalizing none") != std::string::npos);
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    assert(link.visit.starts == 1 && link.visit.stops == 1);

    link.client = SessionClient(Loopback::write, &link, 0x90);
    link.handshake();
    assert(link.client.state() == State::stopped);
    assert(link.client.round().profile.matches(profile));
    airrounds::flipper::RunReport report;
    report.id = link.client.visitId();
    report.state = link.client.state();
    report.outcome = link.client.outcome();
    report.round = link.client.round();
    assert(airrounds::flipper::savedPoints(report) == 1);
    const auto last = airrounds::flipper::pointOutcome(report, 2);
    assert(std::string(airrounds::flipper::pointResult(last, report.state)) == "Stopped");
}

void singlePointKeepsItsAddressAfterReopenAndStop() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    assert(link.outbound.find(" round ") != std::string::npos);
    link.receive(link.deliver());
    link.client = SessionClient(Loopback::write, &link, 0x90);
    link.handshake();
    assert(link.client.round().profile.size() == 1);
    assert(link.client.stop(link.client.visitId(), link.now));
    assert(link.outbound.find(" round_stop\n") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.round().profile.size() == 1);
    assert(airrounds::session::sameAddress(link.client.round().profile[0], target));
}

airrounds::session::Profile twoPointProfile() {
    airrounds::session::Profile profile;
    const bool valid = profile.add(target) && profile.add({{2, 0, 0, 0, 0, 2}, 11});
    assert(valid);
    return profile;
}

void roundRepliesKeepTheRequestedProfile() {
    using airrounds::flipper::Request;
    Loopback link;
    link.handshake();
    const auto profile = twoPointProfile();
    const bool started = link.client.start(profile, link.now);
    assert(started);
    const auto start = link.deliver();
    auto changed = start;
    changed.replace(changed.find("0200000000020b0"), 15, "0200000000040b0");
    link.receive(changed);
    assert(link.client.pending() == Request::start);
    link.receive(start);
    assert(link.client.round().profile.matches(profile));

    link.now += 500;
    link.client.poll(link.now);
    const auto status = link.deliver();
    changed = status;
    changed.replace(changed.find("0200000000020b0"), 15, "020000000002010");
    link.receive(changed);
    assert(link.client.pending() == Request::status);
    assert(link.client.round().profile.matches(profile));
    link.receive(status);
    assert(link.client.pending() == Request::none);
}

void roundRepliesKeepFinishedResults() {
    using airrounds::flipper::Request;
    Loopback link;
    link.handshake();
    const bool started = link.client.start(twoPointProfile(), link.now);
    assert(started);
    link.receive(link.deliver());
    link.visit.done = true;
    link.visit.outcome = Outcome::saved;
    link.board.poll();

    link.now += 500;
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.round().results[0] == Outcome::saved);
    link.now += 500;
    link.client.poll(link.now);
    const auto status = link.deliver();
    auto changed = status;
    changed.replace(changed.find("020000000001062"), 15, "020000000001061");
    link.receive(changed);
    assert(link.client.pending() == Request::status);
    assert(link.client.round().results[0] == Outcome::saved);
    link.receive(status);

    link.visit.outcome = Outcome::not_observed;
    assert(link.client.stop(link.client.visitId(), link.now));
    link.receive(link.deliver());
    assert(link.client.state() == State::stopped);
    link.now += 500;
    link.client.poll(link.now);
    const auto stopped = link.deliver();
    auto wrongState = stopped;
    wrongState.replace(wrongState.find(" stopped saved "), 15, " complete saved ");
    link.receive(wrongState);
    assert(link.client.pending() == Request::status);
    assert(link.client.state() == State::stopped);
    link.receive(stopped);
    assert(link.client.pending() == Request::none);
}

void nextRoundCanUseAnotherProfile() {
    Loopback link;
    link.handshake();
    auto profile = twoPointProfile();
    const bool started = link.client.start(profile, link.now);
    assert(started);
    link.receive(link.deliver());

    link.visit.outcome = Outcome::not_observed;
    assert(link.client.stop(link.client.visitId(), link.now));
    link.receive(link.deliver());
    assert(link.client.state() == State::stopped);

    profile.removeLast();
    const bool valid = profile.add({{2, 0, 0, 0, 0, 3}, 1});
    assert(valid);
    const bool restarted = link.client.start(profile, link.now);
    assert(restarted);
    link.receive(link.deliver());
    assert(link.client.state() == State::running);
    assert(link.client.visitId().matches({boot, 2}));
    assert(link.client.round().profile.matches(profile));
}

void oldFirmwareKeepsSingleVisit() {
    Loopback link;
    link.handshake(false);
    assert(!link.client.supportsRounds());
    airrounds::session::Profile profile;
    const bool valid = profile.add(target) && profile.add({{2, 0, 0, 0, 0, 2}, 11});
    assert(valid);
    const bool invalidStarted = link.client.start(profile, link.now);
    assert(!invalidStarted);
    assert(link.outbound.empty());
    const bool started = link.client.start(target, link.now);
    assert(started);
    assert(link.outbound.find(" start ") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.state() == State::running);
}
void journalFailureKeepsTheCaptureResult() {
    Loopback link;
    link.visit.journalSupported = true;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    link.visit.done = true;
    link.visit.outcome = Outcome::saved;
    link.visit.journal = JournalState::failed;
    link.board.poll();
    link.now += 500;
    link.client.poll(link.now);
    const auto status = link.deliver();
    assert(status.find(" journal failed aps ") != std::string::npos);
    link.receive(status);
    assert(link.client.state() == State::complete && link.client.outcome() == Outcome::saved);
    assert(link.client.journalState() == JournalState::failed);

    link.outbound = "@01 123456789abcdef0 80000000000001aa 00000000 round_status\n";
    const auto legacy = link.deliver();
    assert(legacy.find(" journal ") == std::string::npos);
    Reply parsed;
    const bool decoded = readReply(legacy.c_str(), parsed);
    assert(decoded && parsed.outcome == Outcome::saved && !parsed.hasJournal);
}

void wrongBootDropsTheLink() {
    Loopback link;
    link.handshake();
    // The board restarts while Flipper still holds the old boot ID.
    Visit restartedVisit;
    Protocol restarted(restartedVisit, boot + 1);
    const bool started = link.client.start(target, link.now);
    assert(started);
    const auto reply = link.deliverTo(restarted);
    assert(reply.find(" wrong_boot ") != std::string::npos);
    link.receive(reply);
    assert(link.client.link() == Link::lost);
    assert(link.client.pending() == Request::none);
    assert(link.client.rejected() == Request::none);
    assert(restartedVisit.starts == 0 && link.visit.starts == 0);
    // Flipper says hello again after the reconnect delay, not at once.
    link.client.poll(link.now + 299);
    assert(link.outbound.empty());
    link.client.poll(link.now + 300);
    assert(link.outbound.find(" hello\n") != std::string::npos);
}

void lastSessionIdBlocksStart() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    link.visit.done = true;
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    link.now += 350;
    link.client.poll(link.now);
    const auto status = link.deliver();
    assert(status.find(" 00000001 complete saved ") != std::string::npos);
    link.receive(status);
    assert(link.client.state() == State::complete && link.client.canStart());

    // A board at the last session ID cannot number another session.
    link.now += 350;
    link.client.poll(link.now);
    link.receive(tamper(link.deliver(), " 00000001 complete", " ffffffff complete"));
    assert(link.client.visitId().matches({boot, UINT32_MAX}));
    assert(link.client.link() == Link::ready && !link.client.canStart());
    const bool started2 = link.client.start(target, link.now);
    assert(!started2);
    assert(link.outbound.empty());
}

// Poll until the client sends a request, then return the board's reply.
std::string exchange(Loopback& link) {
    for (unsigned turn = 0; turn < 10 && link.outbound.empty(); ++turn) {
        link.now += 50;
        link.client.poll(link.now);
    }
    assert(!link.outbound.empty());
    return link.deliver();
}

// Start a scan and check what Flipper refuses while it runs. Returns the scan ID.
std::string beginScan(Loopback& link, const TestScan& scanner) {
    assert(link.client.supportsScan() && link.client.supportsRoundDetail());
    assert(link.client.scanState() == ScanState::idle);
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    assert(link.outbound.find(" 00000000 scan\n") != std::string::npos);
    const auto scanId = link.outbound.substr(21, 16);
    assert(link.client.scanState() == ScanState::running);
    const bool startedDuringScan = link.client.start(target, link.now);
    assert(!link.client.canStart() && !startedDuringScan);
    const bool scannedAgain = link.client.scan(link.now);
    assert(!scannedAgain);
    link.receive(link.deliver());
    assert(scanner.starts == 1 && link.client.pending() == Request::none);
    return scanId;
}

// The board has no rows while it listens. Flipper asks again after the poll interval.
void readFirstRow(Loopback& link, TestScan& scanner, const std::string& scanId) {
    link.client.poll(link.now);
    assert(link.outbound.find(" scan_read " + scanId + " 00\n") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.scanState() == ScanState::running);
    link.client.poll(link.now + 349);
    assert(link.outbound.empty());
    link.now += 350;
    link.client.poll(link.now);
    assert(link.outbound.find(" scan_read " + scanId + " 00\n") != std::string::npos);
    scanner.set(ScanState::complete);
    link.receive(link.deliver());
    assert(link.client.scanResults().size() == 1);
    assert(link.client.scanState() == ScanState::running);
}

// Later rows follow at once.
void readLaterRows(Loopback& link, const std::string& scanId) {
    link.client.poll(link.now);
    assert(link.outbound.find(" scan_read " + scanId + " 01\n") != std::string::npos);
    link.receive(link.deliver());
    link.client.poll(link.now);
    assert(link.outbound.find(" scan_read " + scanId + " 02\n") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.scanState() == ScanState::complete);
}

void rowsMatchTheBoard(const SessionClient& client, const TestScan& scanner) {
    const auto& rows = client.scanResults();
    assert(rows.size() == 3);
    for (size_t index = 0; index < rows.size(); ++index) {
        assert(airrounds::session::sameAddress(rows[index].target, scanner.list[index].target));
        assert(rows[index].target.channel == index + 1);
        assert(rows[index].nameSize == scanner.list[index].nameSize);
        assert(memcmp(rows[index].name, scanner.list[index].name, rows[index].nameSize) == 0);
    }
}

void scanReadsEveryRow() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const auto scanId = beginScan(link, scanner);
    readFirstRow(link, scanner, scanId);
    readLaterRows(link, scanId);
    rowsMatchTheBoard(link.client, scanner);
    assert(link.client.canStart(5) && !link.client.canStart(6));
    link.client.poll(link.now);
    assert(link.outbound.find(" round_status_detail\n") != std::string::npos);
    link.receive(link.deliver());

    // A new scan clears the earlier rows.
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    assert(link.client.scanResults().size() == 0);
    link.receive(link.deliver());
    assert(scanner.starts == 2);
}

void emptyScanCompletes() {
    TestScan scanner;
    scanner.list.clear();
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    scanner.state = ScanState::complete;
    link.receive(exchange(link));
    assert(link.client.scanState() == ScanState::complete);
    assert(link.client.scanResults().size() == 0);
}

// Run a scan to its first row and return the reply that carries it.
std::string firstScanRow(Loopback& link, TestScan& scanner) {
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    scanner.state = ScanState::complete;
    return exchange(link);
}

void scanRowsMustArriveInOrder() {
    {
        TestScan scanner;
        Loopback link(&scanner);
        const auto row = firstScanRow(link, scanner);
        link.receive(tamper(row, " complete 03 00 ", " complete 03 01 "));
        assert(link.client.scanState() == ScanState::error);
        assert(link.client.scanResults().size() == 0);
        assert(link.client.pending() == Request::none);
        assert(link.client.canStart());
        link.client.poll(link.now);
        assert(link.outbound.find(" round_status_detail\n") != std::string::npos);
    }
    {
        // The row count must not change during one scan.
        TestScan scanner;
        Loopback link(&scanner);
        link.receive(firstScanRow(link, scanner));
        assert(link.client.scanResults().size() == 1);
        link.receive(tamper(exchange(link), " complete 03 01 ", " complete 04 01 "));
        assert(link.client.scanState() == ScanState::error);
    }
    {
        // A board that falls back to running after it reported rows has lost them.
        TestScan scanner;
        Loopback link(&scanner);
        link.receive(firstScanRow(link, scanner));
        scanner.state = ScanState::running;
        link.receive(exchange(link));
        assert(link.client.scanState() == ScanState::error);
    }
    {
        TestScan scanner;
        Loopback link(&scanner);
        link.handshake();
        const bool scanned = link.client.scan(link.now);
        assert(scanned);
        link.receive(link.deliver());
        scanner.state = ScanState::idle;
        link.receive(exchange(link));
        assert(link.client.scanState() == ScanState::error);
    }
}

void scanRepliesMustMatchTheRequest() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    const auto accepted = link.deliver();
    // A read reply cannot confirm the scan command.
    link.receive(tamper(accepted, " 1b\n", " 1b scan running 00 00\n"));
    assert(link.client.pending() == Request::scan);
    link.receive(tamper(accepted, " 1b\n", " 1b aps\n"));
    assert(link.client.pending() == Request::scan);
    link.receive(accepted);
    assert(link.client.pending() == Request::none);
    assert(link.client.scanState() == ScanState::running);

    link.client.poll(link.now);
    const auto row = link.deliver();
    assert(row.find(" scan running 00 00\n") != std::string::npos);
    link.receive(tamper(row, " scan running 00 00\n", "\n"));
    assert(link.client.pending() == Request::scan_read);
    link.receive(row);
    assert(link.client.pending() == Request::none);
}

void scanRejectionIsAnError() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    link.receive(link.deliver());
    assert(link.client.state() == State::running);
    const bool scanned = link.client.scan(link.now);
    assert(!scanned);
    link.visit.done = true;
    link.board.poll();
    link.receive(exchange(link));
    assert(link.client.state() == State::finalizing);
    const bool scanned2 = link.client.scan(link.now);
    assert(!scanned2);
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    link.receive(exchange(link));
    assert(link.client.state() == State::complete);

    // Another client starts a session after this read. The rejection ends the scan.
    link.outbound = "@01 123456789abcdef0 0000000000000005 00000002 round 020000000001 06\n";
    assert(link.deliver().find(" ok ") != std::string::npos);
    const bool scanned3 = link.client.scan(link.now);
    assert(scanned3);
    const auto rejected = link.deliver();
    assert(rejected.find(" stale_request ") != std::string::npos);
    link.receive(rejected);
    assert(link.client.scanState() == ScanState::error);
    assert(link.client.pending() == Request::none);
    assert(scanner.starts == 0);
}

void noScanWithoutBoardSupport() {
    Loopback link;
    link.handshake();
    assert(!link.client.supportsScan() && !link.client.supportsRoundDetail());
    const bool scanned = link.client.scan(link.now);
    assert(!scanned);
    assert(link.outbound.empty());
    assert(link.client.scanState() == ScanState::idle);
    assert(!link.client.cancelScan(link.now));
    assert(link.client.canStart(3) && !link.client.canStart(4));

    TestScan scanner;
    Loopback unready(&scanner);
    const bool scanned2 = unready.client.scan(unready.now);
    assert(!scanned2);
    assert(unready.outbound.empty());
}

void cancelBeforeScanReply() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    assert(!link.client.cancelScan(link.now));
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    const auto accepted = link.deliver();
    // Stop must wait for the scan ID that the reply confirms.
    assert(link.client.cancelScan(link.now));
    assert(link.outbound.empty());
    assert(link.client.cancelScan(link.now));
    link.receive(accepted);
    assert(link.client.scanState() == ScanState::running);
    link.client.poll(link.now);
    assert(link.outbound.find(" scan_stop ") != std::string::npos);
    assert(link.client.pending() == Request::scan_stop);
    // Another cancel while Stop is on the wire sends nothing new.
    const auto stop = link.outbound;
    assert(link.client.cancelScan(link.now));
    assert(link.outbound == stop);
    link.receive(link.deliver());
    assert(scanner.stops == 1 && scanner.state == ScanState::idle);
    assert(link.client.scanState() == ScanState::idle);
    assert(link.client.pending() == Request::none);
    link.client.poll(link.now);
    assert(link.outbound.find(" round_status_detail\n") != std::string::npos);
}

void cancelDuringScanRead() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    const auto scanId = link.outbound.substr(21, 16);
    link.receive(link.deliver());
    scanner.state = ScanState::complete;
    link.client.poll(link.now);
    assert(link.client.pending() == Request::scan_read);
    const auto row = link.deliver();
    // Stop replaces the read at once. The late row no longer counts.
    assert(link.client.cancelScan(link.now));
    assert(link.outbound.find(" scan_stop " + scanId + "\n") != std::string::npos);
    link.receive(row);
    assert(link.client.pending() == Request::scan_stop);
    assert(link.client.scanResults().size() == 0);
    link.receive(link.deliver());
    assert(link.client.scanState() == ScanState::idle);
    assert(link.client.scanResults().size() == 0);
}

void cancelAfterScanReply() {
    TestScan scanner;
    scanner.cleanupFails = true;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    const auto scanId = link.outbound.substr(21, 16);
    link.receive(link.deliver());
    assert(link.client.cancelScan(link.now));
    assert(link.outbound.find(" 00000000 scan_stop " + scanId + "\n") != std::string::npos);
    const auto failed = link.deliver();
    assert(failed.find(" radio_error ") != std::string::npos);
    link.receive(failed);
    assert(link.client.scanState() == ScanState::error);
    assert(!link.client.cancelScan(link.now));
    assert(link.client.canStart());
}

void lostLinkEndsTheScan() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    link.client.poll(link.now);
    link.deliver();
    link.now += 900;
    link.client.poll(link.now);
    assert(link.client.link() == Link::lost);
    assert(link.client.scanState() == ScanState::error);
    assert(!link.client.cancelScan(link.now));
}

// Poll until the client sends a status read, then return the board's reply.
std::string statusReply(Loopback& link) {
    link.now += 350;
    link.client.poll(link.now);
    assert(link.outbound.find(" 8") != std::string::npos);
    return link.deliver();
}

// Reopen the app on the same board. Its first status reply is tampered, so no earlier round can
// mask the check. The real reply then completes the handshake.
void reopenIgnores(Loopback& link, const std::string& from, const std::string& to) {
    link.client = SessionClient(Loopback::write, &link, 0x90);
    link.client.poll(link.now);
    link.receive(link.deliver());
    link.client.poll(link.now);
    const auto reply = link.deliver();
    link.receive(tamper(reply, from, to));
    assert(link.client.pending() == Request::status && link.client.link() != Link::ready);
    link.receive(reply);
    assert(link.client.link() == Link::ready);
}

// A tampered status reply must leave the request open. The real reply then completes it.
void ignoresTamperedStatus(Loopback& link, const std::string& from, const std::string& to) {
    const auto reply = statusReply(link);
    link.receive(tamper(reply, from, to));
    assert(link.client.pending() == Request::status);
    link.receive(reply);
    assert(link.client.pending() == Request::none);
}

void secondScanStartsOver() {
    TestScan scanner;
    Loopback link(&scanner);
    const auto row = firstScanRow(link, scanner);
    link.receive(row);
    link.receive(exchange(link));
    link.receive(exchange(link));
    assert(link.client.scanState() == ScanState::complete && link.client.scanResults().size() == 3);
    link.client.poll(link.now);
    link.receive(link.deliver());

    // The next scan finds two rows. It must not expect the earlier count.
    scanner.list.clear();
    for (uint8_t index = 1; index <= 2; ++index) {
        const airrounds::session::Target entry{{2, 0, 0, 0, 2, index}, 6};
        const bool added = scanner.list.add(entry, nullptr, 0);
        assert(added);
    }
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    scanner.set(ScanState::running);
    link.receive(exchange(link));
    assert(link.client.scanState() == ScanState::running);
    scanner.set(ScanState::complete);
    link.receive(exchange(link));
    link.receive(exchange(link));
    assert(link.client.scanState() == ScanState::complete && link.client.scanResults().size() == 2);
}

void scanRowsNeedDistinctAddresses() {
    TestScan scanner;
    Loopback link(&scanner);
    link.receive(firstScanRow(link, scanner));
    link.receive(tamper(exchange(link), " 020000000102 02 ", " 020000000101 02 "));
    assert(link.client.scanState() == ScanState::error);
}

void scanStopUsesAnIdAboveOtherClients() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    // Another client sends a mutation during the scan.
    link.outbound = "@01 123456789abcdef0 0000000000000050 00000009 stop\n";
    assert(link.deliver().find(" unknown_session ") != std::string::npos);
    link.receive(exchange(link));
    assert(link.client.cancelScan(link.now));
    assert(link.outbound.find(" 0000000000000051 00000000 scan_stop ") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.scanState() == ScanState::idle);
}

void failedScanDropsTheCancel() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    // Another client starts a round. Flipper has not read it yet, so the board refuses the scan.
    link.outbound = "@01 123456789abcdef0 0000000000000050 00000001 round 020000000009 06\n";
    assert(link.deliver().find(" ok ") != std::string::npos);
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    assert(link.client.cancelScan(link.now));
    const auto refused = link.deliver();
    assert(refused.find(" ok ") == std::string::npos);
    link.receive(refused);
    assert(link.client.scanState() == ScanState::error);
    link.client.poll(link.now);
    assert(link.outbound.find(" round_status_detail\n") != std::string::npos);
}

void finishedScanSurvivesLinkLoss() {
    TestScan scanner;
    Loopback link(&scanner);
    scanner.list.clear();
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.receive(link.deliver());
    scanner.state = ScanState::complete;
    link.receive(exchange(link));
    assert(link.client.scanState() == ScanState::complete);
    link.client.transportLost(link.now);
    assert(link.client.scanState() == ScanState::complete);
}

void cancelDoesNotOutliveTheLink() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool scanned = link.client.scan(link.now);
    assert(scanned);
    link.outbound.clear();
    assert(link.client.cancelScan(link.now));
    link.client.transportLost(link.now);
    assert(link.client.scanState() == ScanState::error);
    // After the reconnect, Flipper reads the status. It sends no Stop for the lost scan.
    link.now += 300;
    link.handshake();
    assert(scanner.stops == 0);
}

void refreshKeepsTheCommandOnTheWire() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    const auto reply = link.deliver();
    link.client.refresh(link.now);
    assert(link.client.pending() == Request::start);
    assert(link.client.link() == Link::connecting);
    link.receive(reply);
    assert(link.client.pending() == Request::none && link.client.state() == State::running);
    // The command reply does not confirm the link. Flipper reads the status at once.
    assert(link.client.link() == Link::connecting);
    link.client.poll(link.now);
    assert(link.outbound.find(" round_status\n") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.link() == Link::ready);
}

void requestTimeout() {
    {
        Loopback link;
        link.handshake();
        const bool started = link.client.start(target, link.now);
        assert(started);
        const auto reply = link.deliver();
        link.client.poll(link.now + 899);
        assert(link.client.link() == Link::ready && link.client.pending() == Request::start);
        assert(link.outbound.empty());
        // A reply after 900 ms is too late.
        link.now += 900;
        link.receive(reply);
        assert(link.client.link() == Link::lost && link.client.state() == State::idle);
    }
    {
        Loopback link;
        link.handshake();
        const bool started2 = link.client.start(target, link.now);
        assert(started2);
        link.client.poll(link.now + 900);
        assert(link.client.link() == Link::lost);
    }
}

void damagedLinesAreDropped() {
    Loopback link;
    link.handshake();
    // A line longer than any reply does not spill into the next one. The buffer holds 239 bytes
    // with the line feed, so check the lengths around that limit as well.
    std::string reply;
    for (const size_t length : {239U, 240U, 241U, 600U}) {
        link.receive("!" + std::string(length - 2, 'a') + "\n");
        reply = statusReply(link);
        link.receive(reply);
        assert(link.client.pending() == Request::none);
    }
    // A control byte inside a reply drops the whole line.
    reply = statusReply(link);
    link.receive("!\x01" + reply.substr(1));
    assert(link.client.pending() == Request::status);
    std::string damaged = reply;
    damaged.insert(30, 1, '\x01');
    link.receive(damaged);
    assert(link.client.pending() == Request::status);
    link.receive(reply);
    assert(link.client.pending() == Request::none);
}

// Flipper must not use a mutation ID that reaches the read bit.
void mutationIdsRunOut() {
    const char* const last[]{"7ffffffffffffffe", "7fffffffffffffff"};
    for (unsigned used = 0; used < 2; ++used) {
        Loopback link;
        link.client.poll(link.now);
        link.receive(tamper(link.deliver(), " idle none 0000000000000000 ",
                            std::string(" idle none ") + last[used] + " "));
        link.client.poll(link.now);
        link.receive(link.deliver());
        assert(link.client.link() == Link::ready);
        assert(link.client.canStart() == (used == 0));
        if (used == 0) {
            const bool started = link.client.start(target, link.now);
            assert(started);
            assert(link.outbound.find(" 7fffffffffffffff 00000001 round ") != std::string::npos);
            link.receive(link.deliver());
            assert(link.client.state() == State::running);
            assert(!link.client.stop(link.client.visitId(), link.now));
        } else {
            const bool started2 = link.client.start(target, link.now);
            assert(!started2);
        }
    }
}

void missingFeatureIsIncompatible() {
    Loopback link;
    link.client.poll(link.now);
    link.receive(tamper(link.deliver(), " 03\n", " 02\n"));
    assert(link.client.link() == Link::incompatible);
    assert(link.client.pending() == Request::none);
}

void restartedBoardStartsNewIds() {
    Loopback link;
    link.handshake();
    link.outbound = "@01 123456789abcdef0 0000000000000001 00000001 start 020000000001 06\n";
    link.deliver();
    const airrounds::session::Target changed{{2, 0, 0, 0, 0, 2}, 6};
    const bool started = link.client.start(changed, link.now);
    assert(started);
    link.receive(link.deliver());
    assert(link.client.rejected() == Request::start);
    assert(link.client.link() == Link::connecting);

    // The board restarts. The old rejection and mutation IDs belong to the old boot.
    Visit restartedVisit;
    Protocol restarted(restartedVisit, boot + 1);
    link.target = &restarted;
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.link() == Link::lost);
    link.now += 300;
    link.handshake();
    assert(link.client.rejected() == Request::none);
    const bool started2 = link.client.start(target, link.now);
    assert(started2);
    assert(link.outbound.find(" 0000000000000001 00000001 round ") != std::string::npos);
    link.receive(link.deliver());
    assert(link.client.state() == State::running && restartedVisit.starts == 1);
}

void newCommandClearsTheRejection() {
    Loopback link;
    link.handshake();
    link.outbound = "@01 123456789abcdef0 0000000000000001 00000001 start 020000000001 06\n";
    link.deliver();
    const airrounds::session::Target changed{{2, 0, 0, 0, 0, 2}, 6};
    const bool started = link.client.start(changed, link.now);
    assert(started);
    link.receive(link.deliver());
    assert(link.client.rejected() == Request::start);
    link.receive(statusReply(link));
    assert(link.client.rejected() == Request::start && link.client.link() == Link::ready);
    assert(link.client.stop(link.client.visitId(), link.now));
    assert(link.client.rejected() == Request::none);
}

void statusErrorDropsTheLink() {
    Loopback link;
    link.handshake();
    link.receive(tamper(statusReply(link), " ok ", " bad_request "));
    assert(link.client.link() == Link::lost);
    assert(link.client.rejected() == Request::none);
}

void startReplyMustNameTheCommand() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(target, link.now);
    assert(started);
    const auto reply = link.deliver();
    link.receive(tamper(reply, " 0000000000000001 03 ", " 0000000000000002 03 "));
    assert(link.client.pending() == Request::start);
    link.receive(tamper(reply, " ok 00000001 ", " ok 00000002 "));
    assert(link.client.pending() == Request::start);
    link.receive(reply);
    assert(link.client.state() == State::running);
}

void inconsistentRoundsAreIgnored() {
    {
        Loopback link;
        link.handshake();
        const bool started = link.client.start(twoPointProfile(), link.now);
        assert(started);
        link.receive(link.deliver());
        // A session must be idle exactly when its ID is zero.
        ignoresTamperedStatus(link, " 00000001 running ", " 00000000 running ");
        // A running round has a point left.
        auto reply = statusReply(link);
        link.receive(tamper(tamper(reply, "020000000001060", "020000000001061"), "0200000000020b0",
                            "0200000000020b1"));
        assert(link.client.pending() == Request::status);
        link.receive(reply);
        // Before an error, a point can only be saved or missed.
        link.visit.done = true;
        link.visit.outcome = Outcome::saved;
        link.board.poll();
        ignoresTamperedStatus(link, "020000000001062", "020000000001063");
        assert(link.client.round().results[0] == Outcome::saved);
        link.visit.done = true;
        link.visit.outcome = Outcome::not_observed;
        link.board.poll();
        link.receive(statusReply(link));
        assert(link.client.state() == State::complete);
        // A finished round keeps its state and every result.
        ignoresTamperedStatus(link, " complete saved ", " stopped saved ");
        ignoresTamperedStatus(link, "0200000000020b1", "0200000000020b2");
        assert(link.client.state() == State::complete);
        assert(link.client.round().results[1] == Outcome::not_observed);
    }
    {
        Loopback link;
        link.handshake();
        const bool started2 = link.client.start(twoPointProfile(), link.now);
        assert(started2);
        link.receive(link.deliver());
        link.visit.done = true;
        link.visit.outcome = Outcome::storage_error;
        link.board.poll();
        // The failed point carries the error. An error needs a finished point.
        reopenIgnores(link, "020000000001064", "020000000001063");
        reopenIgnores(link, "020000000001064", "020000000001060");
        assert(link.client.state() == State::error);
    }
    {
        Loopback link;
        link.handshake();
        const bool started3 = link.client.start(twoPointProfile(), link.now);
        assert(started3);
        link.receive(link.deliver());
        link.visit.outcome = Outcome::not_observed;
        assert(link.client.stop(link.client.visitId(), link.now));
        link.receive(link.deliver());
        assert(link.client.state() == State::stopped);
        // A stopped round needs a finished point.
        reopenIgnores(link, "020000000001061", "020000000001060");
    }
}

void interruptionNeedsAMissedPoint() {
    TestScan scanner;
    Loopback link(&scanner);
    link.handshake();
    const bool started = link.client.start(twoPointProfile(), link.now);
    assert(started);
    link.receive(link.deliver());
    // Stop cuts the first visit short.
    link.visit.outcome = Outcome::not_observed;
    assert(link.client.stop(link.client.visitId(), link.now));
    link.receive(link.deliver());
    const auto reply = statusReply(link);
    assert(reply.find(" interrupted 1 ") != std::string::npos);
    link.receive(reply);
    assert(link.client.round().interruption == VisitInterruption::yes);
    // An interruption needs a missed point before it.
    reopenIgnores(link, "020000000001061", "020000000001060");
    assert(link.client.round().interruption == VisitInterruption::yes);
}

void savingPointCanStop() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(twoPointProfile(), link.now);
    assert(started);
    link.receive(link.deliver());
    link.visit.done = true;
    link.board.poll();
    link.receive(statusReply(link));
    assert(link.client.state() == State::finalizing && link.client.round().next() == 0);
    assert(link.client.canStop());
}

void restartedBoardReusesTheSessionNumber() {
    Loopback link;
    link.handshake();
    const bool started = link.client.start(twoPointProfile(), link.now);
    assert(started);
    link.receive(link.deliver());
    // The board restarts, and another client starts session 1 there with another point.
    Visit restartedVisit;
    Protocol restarted(restartedVisit, boot + 1);
    link.outbound = "@01 123456789abcdef1 0000000000000001 00000001 round 020000000009 06\n";
    assert(link.deliverTo(restarted).find(" ok ") != std::string::npos);
    link.target = &restarted;
    link.client.poll(link.now + 350);
    link.receive(link.deliver());
    assert(link.client.link() == Link::lost);
    link.now += 650;
    link.handshake();
    assert(link.client.visitId().matches({boot + 1, 1}));
    assert(link.client.round().profile.size() == 1);
}

void replyDetailsMustMatchTheRead() {
    {
        Loopback link;
        link.handshake();
        const bool started = link.client.start(target, link.now);
        assert(started);
        link.receive(link.deliver());
        ignoresTamperedStatus(link, " 03 aps 020000000001060\n", " 03\n");
    }
    {
        Loopback link;
        link.visit.journalSupported = true;
        link.handshake();
        const bool started2 = link.client.start(target, link.now);
        assert(started2);
        link.receive(link.deliver());
        ignoresTamperedStatus(link, " journal none aps ", " aps ");
    }
    {
        TestScan scanner;
        Loopback link(&scanner);
        link.handshake();
        ignoresTamperedStatus(link, " interrupted 0 aps", " aps");
    }
}

} // namespace

int main() try {
    stopAndSave();
    lostStartAndResume();
    newCommandAfterDroppedWrite();
    stopKeepsTheSelectedVisit();
    commandRejectionStaysVisible();
    freshStatusAfterReturn();
    noMutationWithoutCompatibleStatus();
    loseStopReplyDuringSave();
    roundStopWhileSavingAndReconnect();
    roundRepliesKeepTheRequestedProfile();
    roundRepliesKeepFinishedResults();
    nextRoundCanUseAnotherProfile();
    singlePointKeepsItsAddressAfterReopenAndStop();
    oldFirmwareKeepsSingleVisit();
    journalFailureKeepsTheCaptureResult();
    wrongBootDropsTheLink();
    lastSessionIdBlocksStart();
    scanReadsEveryRow();
    emptyScanCompletes();
    scanRowsMustArriveInOrder();
    scanRepliesMustMatchTheRequest();
    scanRejectionIsAnError();
    noScanWithoutBoardSupport();
    cancelBeforeScanReply();
    cancelDuringScanRead();
    cancelAfterScanReply();
    lostLinkEndsTheScan();
    secondScanStartsOver();
    scanRowsNeedDistinctAddresses();
    scanStopUsesAnIdAboveOtherClients();
    failedScanDropsTheCancel();
    finishedScanSurvivesLinkLoss();
    cancelDoesNotOutliveTheLink();
    refreshKeepsTheCommandOnTheWire();
    requestTimeout();
    damagedLinesAreDropped();
    mutationIdsRunOut();
    missingFeatureIsIncompatible();
    restartedBoardStartsNewIds();
    newCommandClearsTheRejection();
    statusErrorDropsTheLink();
    startReplyMustNameTheCommand();
    inconsistentRoundsAreIgnored();
    interruptionNeedsAMissedPoint();
    savingPointCanStop();
    restartedBoardReusesTheSessionNumber();
    replyDetailsMustMatchTheRead();
    std::cout << "Flipper client checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
