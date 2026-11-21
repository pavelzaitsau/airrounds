// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>

#include "firmware_version.h"
#include "uart_protocol.h"

namespace {

// Show control bytes so a reply that differs only in its line feed is visible.
std::string visible(const std::string& text) {
    std::string shown;
    for (const char byte : text) {
        if (byte == '\n') {
            shown += "\\n";
        } else if (static_cast<unsigned char>(byte) < ' ' ||
                   static_cast<unsigned char>(byte) > '~') {
            shown += '?';
        } else {
            shown += byte;
        }
    }
    return '"' + shown + '"';
}

void fail(int line, const char* check, const std::string& expected, const std::string& actual) {
    std::cerr << "protocol_test.cpp:" << line << ": " << check
              << " failed\n  expected: " << visible(expected) << "\n  actual:   " << visible(actual)
              << '\n';
    std::abort();
}

void expectReply(const std::string& actual, const std::string& expected, int line) {
    if (actual != expected) {
        fail(line, "reply", expected, actual);
    }
}

void expectContains(const std::string& actual, const std::string& part, int line) {
    if (actual.find(part) == std::string::npos) {
        fail(line, "reply contains", "... " + part + " ...", actual);
    }
}

// A rejected or plain reply ends after the feature bits, with no scan or round fields.
void expectPlain(const std::string& actual, const char* code, int line) {
    const std::string suffix = " 1b\n";
    if (actual.find(std::string(" ") + code + " ") != 46 || actual.size() < suffix.size() ||
        actual.compare(actual.size() - suffix.size(), suffix.size(), suffix) != 0) {
        fail(line, "plain reply", std::string("... ") + code + " ... 1b\\n", actual);
    }
}

#define EXPECT_REPLY(actual, expected) expectReply((actual), (expected), __LINE__)
#define EXPECT_CONTAINS(actual, part) expectContains((actual), (part), __LINE__)
#define EXPECT_PLAIN(actual, code) expectPlain((actual), (code), __LINE__)

using airrounds::session::Target;
using namespace airrounds::uart;

constexpr uint64_t boot = 0x123456789abcdef0ULL;

class Visit final : public VisitBackend {
public:
    uint64_t nowUs() const override { return clockUs; }
    void beginRound(const SessionReport&) override { checkpointPending = holdCheckpoint; }
    VisitStart start(const Target&, const VisitBudget&) override {
        assert(!receiving);
        ++starts;
        done = false;
        result = Outcome::none;
        receiving = radioStarts;
        return radioStarts ? VisitStart::listening : VisitStart::radio_error;
    }
    bool observationDone() const override { return done; }
    bool stopReception() override {
        assert(receiving);
        ++stops;
        receiving = false;
        return radioStops;
    }
    void beginSave(const CaptureId& id) override {
        assert(!receiving);
        saveId = id;
        ++saves;
    }
    Outcome saveResult() const override { return result; }
    bool journalPending() const override { return checkpointPending; }
    bool supportsJournal() const override { return journal; }
    JournalState journalState() const override { return journalStatus; }

    uint64_t clockUs = 0;
    unsigned starts = 0;
    unsigned stops = 0;
    unsigned saves = 0;
    bool done = false;
    bool receiving = false;
    bool radioStarts = true;
    bool radioStops = true;
    // A held checkpoint keeps the next round in its preparation phase.
    bool holdCheckpoint = false;
    bool checkpointPending = false;
    Outcome result = Outcome::none;
    CaptureId saveId{};
    bool journal = false;
    JournalState journalStatus = JournalState::none;
};

class Wire {
public:
    explicit Wire(uint64_t bootId = boot, ScanBackend* scan = nullptr)
        : protocol(visit, bootId, scan) {}

    std::string send(const std::string& bytes) {
        std::string reply;
        for (char byte : bytes) {
            char output[Protocol::replyCapacity];
            if (protocol.receive({static_cast<uint8_t>(byte), now}, output)) {
                reply += output;
            }
        }
        return reply;
    }

    std::string command(const char* tail, const char* version = "01") {
        return send(std::string("@") + version + " 123456789abcdef0 " + tail + "\n");
    }

    Visit visit;
    Protocol protocol;
    uint32_t now = 0;
};

void negotiationAndValidation() {
    Wire wire;
    EXPECT_REPLY(
        wire.send("@01 0000000000000000 8000000000000011 00000000 hello\n"),
        "!01 123456789abcdef0 8000000000000011 00000000 ok 00000000 idle none 0000000000000000 "
        "03\n");
    EXPECT_REPLY(
        wire.command("0000000000000012 00000001 start 020000000001 06", "02"),
        "!02 123456789abcdef0 0000000000000012 00000001 unsupported_version 00000000 idle none "
        "0000000000000000 03\n");
    EXPECT_REPLY(
        wire.send("@01 0000000000000000 0000000000000013 00000001 start 020000000001 06\n"),
        "!01 0000000000000000 0000000000000013 00000001 wrong_boot 00000000 idle none "
        "0000000000000000 03\n");
    EXPECT_CONTAINS(wire.command("0000000000000014 00000001 start 010000000001 06"), "bad_target");
    EXPECT_CONTAINS(wire.command("0000000000000015 00000001 start 020000000001 0e"), "bad_target");
    EXPECT_CONTAINS(wire.command("0000000000000016 00000001 start"), "bad_request");
    EXPECT_CONTAINS(wire.command("0000000000000017 00000002 start 020000000001 06"),
                    "unknown_session");
    EXPECT_CONTAINS(wire.command("8000000000000018 00000001 start 020000000001 06"), "bad_request");
    EXPECT_CONTAINS(wire.send("@02 0000000000000000 8000000000000019 00000000 hello\n"),
                    "unsupported_version");
    assert(wire.visit.starts == 0);
}

void retriesAndSave() {
    Wire wire;
    const char* start = "0000000000000020 00000001 start 020000000001 06";
    const char* stop = "0000000000000025 00000001 stop";
    const std::string running = wire.command(start);
    EXPECT_REPLY(running, "!01 123456789abcdef0 0000000000000020 00000001 ok 00000001 running none "
                          "0000000000000020 03\n");
    EXPECT_REPLY(wire.command(start), running);
    EXPECT_CONTAINS(wire.command(stop, "02"), "unsupported_version");
    assert(wire.visit.stops == 0);
    EXPECT_CONTAINS(wire.command("0000000000000022 00000001 start 020000000002 06"), "conflict");
    EXPECT_CONTAINS(wire.command("0000000000000023 00000002 start 020000000001 06"), "busy");
    const std::string stopped = wire.command(stop);
    EXPECT_REPLY(stopped,
                 "!01 123456789abcdef0 0000000000000025 00000001 ok 00000001 finalizing none "
                 "0000000000000025 03\n");
    EXPECT_REPLY(wire.command(stop), stopped);
    EXPECT_CONTAINS(wire.command("8000000000000024 00000000 status"), "00000001 finalizing none");
    wire.protocol.poll();
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command(stop), "00000001 stopped saved");
    EXPECT_CONTAINS(wire.command("0000000000000023 00000002 start 020000000001 06"),
                    "stale_request");
    EXPECT_CONTAINS(wire.command(start), "00000001 stopped saved");
    assert(wire.visit.starts == 1);
    assert(wire.visit.stops == 1);
    assert(wire.visit.saves == 1);
    assert(wire.visit.saveId.visit.matches({boot, 1}));
    assert(wire.visit.saveId.slot == 0);
}

void quietStopCompletesInReply() {
    Wire wire;
    wire.command("0000000000000001 00000001 start 020000000001 06");
    wire.visit.result = Outcome::not_observed;
    const auto reply = wire.command("0000000000000002 00000001 stop");
    EXPECT_CONTAINS(reply, "00000001 stopped not_observed");
    assert(wire.visit.stops == 1);
}

void interruptedStream() {
    Wire wire;
    const char* start = "0000000000000030 00000001 start 020000000001 06";
    wire.now = UINT32_MAX - 50U;
    EXPECT_REPLY(wire.send("@01 123456789abcdef0 0000000000000030 00000001 start "), "");
    wire.now += 101U;
    EXPECT_REPLY(wire.send("020000000001 06\n"), "");
    assert(wire.visit.starts == 0);
    EXPECT_REPLY(wire.send("@01 123456789abcdef0 "), "");
    EXPECT_CONTAINS(wire.command(start), "00000001 running none");
    wire.visit.done = true;
    wire.protocol.poll();
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000031 00000000 status"),
                    "00000001 complete not_observed");
    EXPECT_CONTAINS(wire.command(start), "00000001 complete not_observed");
    assert(wire.visit.starts == 1);
    assert(wire.visit.stops == 1);
    EXPECT_CONTAINS(wire.command("8000000000000032 00000000 status"), "complete");
}

void replyFraming() {
    Wire wire;
    const auto reply = wire.send("@01 0000000000000000 800000000000002a 00000000 hello\n");
    const ReplyKey expected{0, 0x800000000000002aULL, 0};
    const auto header = reply.substr(0, reply.find(" ok"));
    assert(matchesReply(reply.c_str(), expected));
    assert(!matchesReply(header.c_str(), expected));
    assert(!matchesReply(reply.substr(0, reply.size() - 1).c_str(), expected));
    assert(!matchesReply((header + " ok 00000000 idle none 0000000000000000\n").c_str(), expected));
    assert(!matchesReply((header + " bogus 00000000 idle none 0000000000000000 01\n").c_str(),
                         expected));
    assert(!matchesReply((header + " ok 00000000 unknown none 0000000000000000 01\n").c_str(),
                         expected));
    assert(!matchesReply((header + " ok 00000000 idle nonsense 0000000000000000 01\n").c_str(),
                         expected));
    assert(
        !matchesReply((header + " ok 00000000 idle none 8000000000000000 01\n").c_str(), expected));
    assert(
        !matchesReply((header + " ok 00000000 idle none 0000000000000000 0z\n").c_str(), expected));
    assert(matchesReply(("rst:0x1 (POWERO" + reply).c_str(), expected));
    assert(matchesReply((header + reply).c_str(), expected));
}

void staleTraffic() {
    Wire wire;
    const std::string old = wire.command("0000000000000040 00000001 start 020000000001 06");
    const std::string oldStatus = wire.command("8000000000000041 00000000 status");
    wire.command("0000000000000042 00000001 stop");
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    const std::string current = wire.command("0000000000000043 00000002 start 020000000001 06");
    const ReplyKey expected{boot, 0x43, 2};
    assert(matchesReply(current.c_str(), expected));
    assert(!matchesReply(old.c_str(), expected));
    assert(!matchesReply(oldStatus.c_str(), {boot, 0x8000000000000044ULL, 0}));
    assert(!matchesReply(current.c_str(), {boot, 0x43, 1}));
    assert(!matchesReply(current.c_str(), {boot + 1, 0x43, 2}));
    const std::string hello = wire.send("@01 0000000000000000 8000000000000046 00000000 hello\n");
    assert(matchesReply(hello.c_str(), {0, 0x8000000000000046ULL, 0}));
    assert(!matchesReply(hello.c_str(), {0, 0x8000000000000047ULL, 0}));
    assert(!matchesReply("READY T4", expected));
    assert(!matchesReply("!02 123456789abcdef0 0000000000000043 00000002 ok", expected));
    EXPECT_CONTAINS(wire.command("0000000000000042 00000001 stop"), "stale_request");
    EXPECT_CONTAINS(wire.command("0000000000000040 00000001 start 020000000001 06"),
                    "stale_request");
    EXPECT_CONTAINS(wire.command("0000000000000045 00000001 stop"), "unknown_session");
    assert(wire.visit.starts == 2);
    assert(wire.visit.stops == 1);
    Wire rebooted(boot + 1);
    const std::string wrongBoot =
        rebooted.command("0000000000000040 00000001 start 020000000001 06");
    EXPECT_CONTAINS(wrongBoot, "wrong_boot");
    assert(matchesReply(wrongBoot.c_str(), {boot, 0x40, 1}));
    assert(!matchesReply(wrongBoot.c_str(), {boot + 1, 0x40, 1}));
    assert(rebooted.visit.starts == 0);
    const std::string wrongVersion = wire.command("0000000000000046 00000002 stop", "02");
    EXPECT_CONTAINS(wrongVersion, "unsupported_version");
    assert(!matchesReply(wrongVersion.c_str(), {boot, 0x46, 2}));
}

void replayAfterStateChange() {
    Wire wire;
    const char* oldStop = "0000000000000060 00000001 stop";
    EXPECT_CONTAINS(wire.command(oldStop), "unknown_session");
    EXPECT_CONTAINS(wire.command("0000000000000060 00000001 start 020000000001 06"), "conflict");
    wire.command("0000000000000061 00000001 start 020000000001 06");
    const char* busyStart = "0000000000000062 00000002 start 020000000001 06";
    EXPECT_CONTAINS(wire.command(busyStart), "busy");
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command(busyStart), "busy");
    EXPECT_CONTAINS(wire.command("0000000000000063 00000002 start 020000000001 06"), "running");
    EXPECT_CONTAINS(wire.command(oldStop), "stale_request");
    assert(wire.visit.starts == 2);
    assert(wire.visit.stops == 1);
}

void roundRejectsInvalidProfiles() {
    Wire wire;
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round"), "bad_request");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000001 round 020000000001 06 020000000001 0b"),
                    "bad_target");
    EXPECT_CONTAINS(wire.command("0000000000000003 00000001 round 020000000001 06 020000000002 0e"),
                    "bad_target");
    EXPECT_CONTAINS(wire.command("0000000000000004 00000001 round 020000000001 06 020000000002 0b "
                                 "020000000003 01 020000000004 0d"),
                    "bad_request");
    assert(wire.visit.starts == 0);
}

class TestScan final : public ScanBackend {
public:
    void startScan() override {
        state = ScanState::running;
        ++starts;
    }
    void pollScan() override { ++polls; }
    void stopScan() override { state = cleanupFails ? ScanState::error : ScanState::idle; }
    ScanState scanState() const override { return state; }
    const ScanList& scanList() const override { return list; }

    ScanState state = ScanState::idle;
    bool cleanupFails = false;
    ScanList list;
    unsigned starts = 0;
    unsigned polls = 0;
};

void scanContract() {
    TestScan scan;
    Wire wire(boot, &scan);
    const char* start = "0000000000000001 00000000 scan";
    EXPECT_CONTAINS(wire.command(start), " ok ");
    EXPECT_CONTAINS(wire.command(start), " ok ");
    assert(scan.starts == 1);
    EXPECT_CONTAINS(wire.command("0000000000000002 00000001 start 020000000001 01"), " busy ");
    Target entry{{}, 6};
    airrounds::session::setAddress(entry, 0x020000000005);
    const uint8_t name[]{'S', 't', 'u', 'd', 'i', 'o'};
    const bool added = scan.list.add(entry, name, sizeof(name));
    const bool addedAgain = scan.list.add(entry, name, sizeof(name));
    assert(added);
    assert(!addedAgain);
    scan.state = ScanState::complete;
    const auto row = wire.command("8000000000000003 00000000 scan_read 0000000000000001 00");
    Reply reply;
    const bool decoded = readReply(row.c_str(), reply);
    assert(decoded);
    assert(reply.hasScan);
    assert(reply.scan.count == 1);
    assert(reply.scan.entry.nameSize == sizeof(name));
    assert(memcmp(reply.scan.entry.name, name, sizeof(name)) == 0);
    EXPECT_CONTAINS(wire.command("8000000000000004 00000000 scan_read 0000000000000001 01"),
                    "bad_request");
    EXPECT_CONTAINS(wire.command("8000000000000005 00000000 scan_read 0000000000000002 00"),
                    "unknown_session");
    EXPECT_CONTAINS(wire.command("0000000000000003 00000000 scan"), " ok ");
    EXPECT_CONTAINS(wire.command("0000000000000004 00000000 scan_stop 0000000000000003"), " ok ");
    assert(scan.state == ScanState::idle);
}

void scanCancelReportsCleanupError() {
    TestScan scan;
    scan.cleanupFails = true;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000000 scan"), " ok ");
    const char* stop = "0000000000000002 00000000 scan_stop 0000000000000001";
    const auto failed = wire.command(stop);
    Reply reply;
    const bool decoded = readReply(failed.c_str(), reply);
    assert(decoded);
    assert(reply.code == Code::radio_error);
    EXPECT_REPLY(wire.command(stop), failed);
    assert(scan.state == ScanState::error);
}

void roundDetailsDistinguishAnInterruption() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01"), " ok ");
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    Reply reply;
    auto line = wire.command("0000000000000002 00000001 round_stop");
    bool decoded = readReply(line.c_str(), reply);
    assert(decoded);
    assert(reply.state == State::stopped);
    assert(reply.round.interruption == VisitInterruption::unknown);
    line = wire.command("8000000000000003 00000001 round_status_detail");
    decoded = readReply(line.c_str(), reply);
    assert(decoded);
    assert(reply.round.interruption == VisitInterruption::no);
    assert(reply.round.results[0] == Outcome::not_observed);

    EXPECT_CONTAINS(wire.command("0000000000000003 00000002 round 020000000001 01"), " ok ");
    wire.visit.result = Outcome::not_observed;
    wire.command("0000000000000004 00000002 round_stop");
    line = wire.command("8000000000000004 00000002 round_status_detail");
    decoded = readReply(line.c_str(), reply);
    assert(decoded);
    assert(reply.round.interruption == VisitInterruption::yes);
    assert(reply.state == State::stopped);
    assert(reply.round.results[0] == Outcome::not_observed);
    line = wire.command("8000000000000005 00000002 round_status");
    decoded = readReply(line.c_str(), reply);
    assert(decoded);
    assert(reply.round.interruption == VisitInterruption::unknown);

    // A stop during a save does not cut a visit short, even right after an interrupted round.
    EXPECT_CONTAINS(wire.command("0000000000000005 00000003 round 020000000001 01 020000000002 06"),
                    " ok ");
    wire.visit.done = true;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("0000000000000006 00000003 round_stop"), " finalizing ");
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_REPLY(wire.command("8000000000000006 00000003 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000006 00000003 ok 00000003 stopped not_observed "
                 "0000000000000006 1b interrupted 0 aps 020000000001011 020000000002060\n");
}

void aSavedRoundDoesNotCarryOver() {
    Wire wire;
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 start 020000000001 06"), " running ");
    wire.visit.done = true;
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000002 00000001 status"), " complete saved ");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000002 start 020000000001 06"), " running ");
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000003 00000002 status"), " complete not_observed ");
}

void stopAfterTheDeadlineIsNotAnInterruption() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01"),
                    "running none");
    // Stop arrives after the round deadline but before a poll ends the visit.
    wire.visit.clockUs = 10000000;
    wire.visit.result = Outcome::not_observed;
    EXPECT_REPLY(wire.command("0000000000000002 00000001 round_stop"),
                 "!01 123456789abcdef0 0000000000000002 00000001 ok 00000001 stopped not_observed "
                 "0000000000000002 1b aps 020000000001011\n");
    EXPECT_REPLY(wire.command("8000000000000003 00000001 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000003 00000001 ok 00000001 stopped not_observed "
                 "0000000000000002 1b interrupted 0 aps 020000000001011\n");
    assert(wire.visit.starts == 1);
    assert(wire.visit.stops == 1);
    assert(wire.visit.saves == 1);
}

void stopWithASavedPointIsNotAnInterruption() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01 020000000002 06 "
                                 "020000000003 0b"),
                    "running none");
    wire.visit.done = true;
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    assert(wire.visit.starts == 2);
    assert(wire.visit.saveId.slot == 1);
    // Stop cuts the second visit short, but the visit still saved a beacon.
    wire.visit.result = Outcome::saved;
    EXPECT_REPLY(wire.command("0000000000000002 00000001 round_stop"),
                 "!01 123456789abcdef0 0000000000000002 00000001 ok 00000001 stopped saved "
                 "0000000000000002 1b aps 020000000001012 020000000002062 0200000000030b0\n");
    EXPECT_REPLY(wire.command("8000000000000003 00000001 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000003 00000001 ok 00000001 stopped saved "
                 "0000000000000002 1b interrupted 0 aps 020000000001012 020000000002062 "
                 "0200000000030b0\n");
    wire.protocol.poll();
    assert(wire.visit.starts == 2);
    assert(wire.visit.stops == 2);
    assert(wire.visit.saves == 2);
    assert(wire.visit.saveId.slot == 2);
}

void stopDuringPreparationIsAnInterruption() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01"),
                    "running none");
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000002 00000001 round_status"),
                    "complete not_observed");

    // The finished visit still reports done while the next checkpoint is written.
    wire.visit.holdCheckpoint = true;
    EXPECT_REPLY(wire.command("0000000000000003 00000002 round 020000000002 06 020000000003 0b"),
                 "!01 123456789abcdef0 0000000000000003 00000002 ok 00000002 running none "
                 "0000000000000003 1b aps 020000000002060 0200000000030b0\n");
    assert(wire.visit.starts == 1);
    EXPECT_REPLY(wire.command("0000000000000004 00000002 round_stop"),
                 "!01 123456789abcdef0 0000000000000004 00000002 ok 00000002 running none "
                 "0000000000000004 1b aps 020000000002060 0200000000030b0\n");
    wire.protocol.poll();
    assert(wire.visit.starts == 1);
    wire.visit.checkpointPending = false;
    wire.protocol.poll();
    EXPECT_REPLY(wire.command("8000000000000005 00000002 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000005 00000002 ok 00000002 stopped not_observed "
                 "0000000000000004 1b interrupted 1 aps 020000000002061 0200000000030b0\n");
    assert(wire.visit.starts == 1);
    assert(wire.visit.stops == 1);
    assert(wire.visit.saves == 1);
}

void scanKeepsStrongerRows() {
    ScanList list;
    for (size_t index = 0; index < ScanList::capacity; ++index) {
        const Target target{{2, 0, 0, 0, 0, static_cast<uint8_t>(index + 1)}, 1};
        const bool added =
            list.observe(target, static_cast<int8_t>(-90 + static_cast<int>(index)), nullptr, 0);
        assert(added);
    }
    const Target weak{{2, 0, 0, 0, 0, 17}, 11};
    const Target strong{{2, 0, 0, 0, 0, 18}, 11};
    const bool keptWeak = list.observe(weak, -100, nullptr, 0);
    const bool keptStrong = list.observe(strong, -35, nullptr, 0);
    assert(!keptWeak);
    assert(keptStrong);
    assert(list.size() == ScanList::capacity);
    assert(airrounds::session::sameAddress(list[0].target, strong));
}

void fivePointContract() {
    TestScan scan;
    Wire wire(boot, &scan);
    const auto started =
        wire.command("0000000000000001 00000001 round 020000000001 01 "
                     "020000000002 06 020000000003 0b 020000000004 01 020000000005 06");
    EXPECT_CONTAINS(started, "020000000005060");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000000 scan"), "busy");
    for (unsigned slot = 1; slot <= 5; ++slot) {
        wire.visit.done = true;
        wire.protocol.poll();
        assert(wire.visit.saveId.slot == slot);
        wire.visit.result = slot == 2 ? Outcome::not_observed : Outcome::saved;
        wire.protocol.poll();
    }
    const auto finished = wire.command("8000000000000003 00000001 round_status");
    Reply reply;
    const bool decoded = readReply(finished.c_str(), reply);
    assert(decoded);
    assert(reply.state == State::complete);
    assert(reply.round.profile.size() == 5);
    assert(reply.round.results[4] == Outcome::saved);
    assert(reply.round.next() == 5);
    assert(reply.round.results[1] == Outcome::not_observed);
}

void roundCompletesEverySlot() {
    Wire wire;
    const char* start =
        "0000000000000001 00000001 round 020000000001 06 020000000002 06 020000000003 0b";
    const char* status = "8000000000000002 00000000 round_status";
    EXPECT_CONTAINS(wire.command(start), "ok 00000001 running none");
    EXPECT_CONTAINS(wire.command(start), "ok 00000001 running none");
    assert(wire.visit.starts == 1);
    wire.visit.done = true;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command(status), "finalizing none");
    assert(wire.visit.starts == 1);
    assert(wire.visit.saveId.slot == 1);
    EXPECT_CONTAINS(wire.command("0000000000000003 00000001 round 020000000002 06 020000000001 06 "
                                 "020000000003 0b"),
                    "conflict");
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    assert(wire.visit.starts == 2);
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    assert(wire.visit.starts == 3);
    wire.visit.done = true;
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command(status), "complete saved");
    assert(wire.visit.starts == 3);
    assert(wire.visit.stops == 3);
    assert(wire.visit.saves == 3);
    assert(wire.visit.saveId.visit.matches({boot, 1}));
    assert(wire.visit.saveId.slot == 3);
    Reply parsed;
    const auto completed = wire.command(status);
    const bool valid = readReply(completed.c_str(), parsed);
    assert(valid);
    assert(parsed.hasRound);
    assert(parsed.round.profile.size() == 3);
    assert(parsed.round.next() == 3);
    assert(parsed.round.results[0] == Outcome::saved);
    assert(parsed.round.results[1] == Outcome::not_observed);
    assert(parsed.round.results[2] == Outcome::saved);
    const auto legacy = wire.command("8000000000000004 00000000 status");
    const bool legacyValid = readReply(legacy.c_str(), parsed);
    assert(legacyValid);
    assert(!parsed.hasRound);
    auto invalid = completed;
    invalid[invalid.size() - 2] = 'f';
    const bool invalidAccepted = readReply(invalid.c_str(), parsed);
    assert(!invalidAccepted);
    auto surplus = completed;
    surplus.insert(surplus.size() - 1, " 020000000004061");
    const bool surplusAccepted = readReply(surplus.c_str(), parsed);
    assert(!surplusAccepted);
}

void roundKeepsItsActiveVisit() {
    Visit visit;
    PassiveRound round(visit);
    airrounds::session::Profile profile;
    const bool emptyAccepted = round.start({boot, 1}, profile);
    assert(!emptyAccepted);
    assert(round.state() == State::idle);
    assert(visit.starts == 0);

    const bool valid = profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(valid);
    const bool started = round.start({boot, 1}, profile);
    const bool startedWhileRunning = round.start({boot, 2}, profile);
    assert(started);
    assert(!startedWhileRunning);
    assert(round.state() == State::running);
    assert(visit.starts == 1);

    visit.done = true;
    round.poll();
    assert(round.state() == State::finalizing);
    assert(visit.saveId.visit.matches({boot, 1}));
    const bool startedWhileSaving = round.start({boot, 2}, profile);
    assert(!startedWhileSaving);
    assert(round.state() == State::finalizing);
    assert(visit.starts == 1);

    visit.result = Outcome::saved;
    round.poll();
    assert(round.state() == State::complete);
    const bool restarted = round.start({boot, 2}, profile);
    assert(restarted);
    assert(round.state() == State::running);
    assert(visit.starts == 2);
}

void roundStopsOnStorageError() {
    Wire wire;
    const auto start = wire.command(
        "0000000000000001 00000001 round 020000000001 01 020000000002 06 020000000003 0b");
    EXPECT_CONTAINS(start, "running none");
    wire.visit.done = true;
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    wire.visit.done = true;
    wire.visit.result = Outcome::storage_error;
    wire.protocol.poll();
    Reply reply;
    const auto status = wire.command("8000000000000002 00000001 round_status");
    const bool valid = readReply(status.c_str(), reply);
    assert(valid);
    assert(reply.state == State::error);
    assert(reply.outcome == Outcome::storage_error);
    assert(reply.round.results[0] == Outcome::saved);
    assert(reply.round.results[1] == Outcome::storage_error);
    assert(reply.round.results[2] == Outcome::none);
    assert(wire.visit.starts == 2);
    assert(wire.visit.saves == 2);
}

void failures() {
    Wire wire;
    wire.visit.radioStarts = false;
    const char* start = "0000000000000050 00000001 start 020000000001 06";
    EXPECT_CONTAINS(wire.command(start), "00000001 error radio_error");
    EXPECT_CONTAINS(wire.command(start), "00000001 error radio_error");
    assert(wire.visit.starts == 1);
    wire.visit.radioStarts = true;
    wire.command("0000000000000051 00000002 start 020000000001 06");
    wire.command("0000000000000052 00000002 stop");
    wire.visit.result = Outcome::storage_error;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000053 00000002 status"),
                    "00000002 error storage_error");
    wire.command("0000000000000054 00000003 start 020000000001 06");
    wire.visit.radioStops = false;
    EXPECT_CONTAINS(wire.command("0000000000000055 00000003 stop"), "00000003 error radio_error");
    assert(wire.visit.saves == 1);
}

std::string idleReply(const char* request, const char* code) {
    return std::string("!01 123456789abcdef0 ") + request + " 00000000 " + code +
           " 00000000 idle none 0000000000000000 03\n";
}

void framingDropsBadFrames() {
    Wire wire;
    const std::string status = "@01 123456789abcdef0 8000000000000070 00000000 status";
    EXPECT_REPLY(wire.send(status + "\n"), idleReply("8000000000000070", "ok"));
    // After a line feed, bytes count again only after the next marker.
    EXPECT_REPLY(wire.send(status + "\nstatus\n"), idleReply("8000000000000070", "ok"));
    // Control bytes, DEL, and bytes above ASCII drop the frame instead of reaching the parser.
    for (const char byte : {'\x01', '\x1f', '\x7f', '\x80'}) {
        EXPECT_REPLY(wire.send(status + std::string(1, byte) + "\n"), "");
        EXPECT_REPLY(wire.send(status + std::string(1, byte) + "status\n"), "");
    }
    // A tilde is printable, so the frame reaches the parser and fails there.
    EXPECT_REPLY(wire.send(status + " ~\n"), idleReply("8000000000000070", "bad_request"));

    // Bytes 99 ms apart stay in one frame; a gap of 100 ms ends it.
    wire.now = 1000;
    EXPECT_REPLY(wire.send("@01 123456789abcdef0 8000000000000071 "), "");
    wire.now += 99;
    EXPECT_REPLY(wire.send("00000000 status\n"), idleReply("8000000000000071", "ok"));
    wire.now = 2000;
    EXPECT_REPLY(wire.send("@01 123456789abcdef0 8000000000000072 00000000 "), "");
    wire.now += 100;
    // The late byte ends the frame, so the rest of the line has no frame to join.
    EXPECT_REPLY(wire.send("xstatus\n"), "");
    EXPECT_REPLY(wire.send("status\n"), "");
    wire.now += 100;
    EXPECT_REPLY(wire.send(status + "\n"), idleReply("8000000000000070", "ok"));

    // The frame holds 143 bytes between the marker and the line feed.
    const std::string header = "01 123456789abcdef0 8000000000000073 00000000 status ";
    const std::string longest = header + std::string(143 - header.size(), 'a');
    assert(longest.size() == 143);
    EXPECT_REPLY(wire.send("@" + longest + "\n"), idleReply("8000000000000073", "bad_request"));
    // One more byte drops the whole frame instead of parsing its first 143 bytes.
    EXPECT_REPLY(wire.send("@" + longest + "a\n"), "");
    EXPECT_REPLY(wire.send("@" + longest + "a\n" + status + "\n"),
                 idleReply("8000000000000070", "ok"));
    const std::string fullStatus = "01 123456789abcdef0 8000000000000074 00000000 status";
    EXPECT_REPLY(wire.send("@" + fullStatus + std::string(143 - fullStatus.size(), ' ') + "x\n"),
                 "");
    EXPECT_REPLY(wire.send("@xx\n" + status + "\n"), idleReply("8000000000000070", "ok"));
}

void envelopeRejectsBadFields() {
    Wire wire;
    // Request ID zero is not a valid mutation or read ID, so the board drops the frame.
    EXPECT_REPLY(wire.command("0000000000000000 00000001 start 020000000001 06"), "");
    EXPECT_REPLY(wire.send("@01 0000000000000000 0000000000000000 00000000 hello\n"), "");
    // Hex fields are lowercase and exactly as wide as the contract says.
    EXPECT_REPLY(wire.send("@01 123456789ABCDEF0 8000000000000075 00000000 status\n"), "");
    EXPECT_REPLY(wire.command("800000000000007A 00000000 status"), "");
    for (const char* scope :
         {"0000000/", "0000000:", "0000000`", "0000000g", "0000000G", "000000000", "0000000"}) {
        EXPECT_REPLY(wire.command((std::string("8000000000000076 ") + scope + " status").c_str()),
                     "");
    }
    EXPECT_REPLY(wire.send("@1 123456789abcdef0 8000000000000077 00000000 status\n"), "");
    EXPECT_REPLY(wire.send("@01  123456789abcdef0 8000000000000077 00000000 status\n"), "");
    EXPECT_CONTAINS(wire.send("@01 123456789abcdef0 8000000000000077 00000000\n"), " bad_request ");
    EXPECT_REPLY(
        wire.command("8000000000000078 0000000a status"),
        "!01 123456789abcdef0 8000000000000078 0000000a unknown_session 00000000 idle none "
        "0000000000000000 03\n");
    assert(wire.visit.starts == 0);
}

void helloNeedsAReadAtBootZero() {
    Wire wire;
    const char* tail = " 00000000 idle none 0000000000000000 03\n";
    EXPECT_REPLY(wire.send("@01 0000000000000000 0000000000000001 00000000 hello\n"),
                 std::string("!01 123456789abcdef0 0000000000000001 00000000 bad_request") + tail);
    EXPECT_REPLY(wire.send("@01 123456789abcdef0 8000000000000002 00000000 hello\n"),
                 std::string("!01 123456789abcdef0 8000000000000002 00000000 bad_request") + tail);
    EXPECT_REPLY(wire.send("@01 0000000000000000 8000000000000003 00000001 hello\n"),
                 std::string("!01 123456789abcdef0 8000000000000003 00000001 bad_request") + tail);
    EXPECT_REPLY(wire.send("@01 0000000000000000 8000000000000004 00000000 hello 01\n"),
                 std::string("!01 123456789abcdef0 8000000000000004 00000000 bad_request") + tail);
    // A hello reply carries the board's version, even when it rejects the client's version.
    EXPECT_REPLY(wire.send("@02 0000000000000000 8000000000000005 00000000 hello\n"),
                 std::string("!01 123456789abcdef0 8000000000000005 00000000 unsupported_version") +
                     tail);
    EXPECT_REPLY(wire.send("@01 0000000000000000 8000000000000006 00000000 hello\n"),
                 std::string("!01 123456789abcdef0 8000000000000006 00000000 ok") + tail);
    // A rejected hello does not consume a mutation ID.
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 start 020000000001 06"),
                    " ok 00000001 running none 0000000000000001 ");
}

void readsCheckScopeAndArguments() {
    Wire wire;
    wire.visit.journal = true;
    const char* idle = " 00000000 idle none 0000000000000000 07";
    EXPECT_REPLY(wire.command("8000000000000001 00000001 status"),
                 std::string("!01 123456789abcdef0 8000000000000001 00000001 unknown_session") +
                     idle + "\n");
    EXPECT_REPLY(
        wire.command("0000000000000001 00000000 stop"),
        "!01 123456789abcdef0 0000000000000001 00000000 unknown_session 00000000 idle none "
        "0000000000000001 07\n");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000000 round_stop"),
                    " unknown_session 00000000 idle none 0000000000000002 07 aps\n");
    EXPECT_REPLY(wire.command("8000000000000002 00000000 round_status_log"),
                 "!01 123456789abcdef0 8000000000000002 00000000 ok 00000000 idle none "
                 "0000000000000002 07 journal none aps\n");
    // Without the scan feature, the board does not know the detail query.
    EXPECT_CONTAINS(wire.command("8000000000000003 00000000 round_status_detail"), " bad_request ");
    EXPECT_CONTAINS(wire.command("0000000000000003 00000001 round 020000000001 06 020000000002 0b"),
                    " ok 00000001 running none ");
    wire.visit.journalStatus = JournalState::pending;
    const std::string running = " 00000001 running none 0000000000000003 07";
    for (const char* verb : {"status", "round_status", "round_status_log"}) {
        for (const char* scope : {"00000000", "00000001", "00000002"}) {
            const std::string id = "8000000000000004";
            const auto reply = wire.command((id + " " + scope + " " + verb).c_str());
            const std::string code = scope[7] == '2' ? "unknown_session" : "ok";
            std::string expected = "!01 123456789abcdef0 ";
            expected += id;
            expected += ' ';
            expected += scope;
            expected += ' ';
            expected += code;
            expected += running;
            if (std::string(verb) == "round_status_log") {
                expected += " journal pending";
            }
            if (std::string(verb) != "status") {
                expected += " aps 020000000001060 0200000000020b0";
            }
            EXPECT_REPLY(reply, expected + "\n");
            // Read requests take no arguments.
            EXPECT_CONTAINS(wire.command((id + " " + scope + " " + verb + " 00").c_str()),
                            " bad_request ");
        }
    }
    // A read request ID cannot carry a mutation, and a mutation ID cannot read.
    EXPECT_CONTAINS(wire.command("8000000000000005 00000001 stop"), " bad_request ");
    EXPECT_CONTAINS(wire.command("0000000000000004 00000001 status"), " bad_request ");
    EXPECT_CONTAINS(wire.command("0000000000000005 00000001 dance"), " bad_request ");
    EXPECT_CONTAINS(wire.command("0000000000000006 00000000 scan"), " bad_request ");
    EXPECT_CONTAINS(wire.command("8000000000000006 00000000 scan_read 0000000000000006 00"),
                    " bad_request ");
    assert(wire.visit.starts == 1);
    assert(wire.visit.stops == 0);
}

void detailWithJournal() {
    TestScan scan;
    Wire wire(boot, &scan);
    wire.visit.journal = true;
    wire.visit.journalStatus = JournalState::saved;
    EXPECT_REPLY(wire.command("8000000000000001 00000000 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                 "0000000000000000 1f journal saved interrupted 0 aps\n");
    EXPECT_CONTAINS(wire.command("8000000000000002 00000000 round_status_detail 00"),
                    " bad_request ");
}

void scanRequestsCheckTheirFields() {
    TestScan scan;
    Wire wire(boot, &scan);
    // No scan has started, so scan ID zero does not name one.
    EXPECT_PLAIN(wire.command("8000000000000001 00000000 scan_read 0000000000000000 00"),
                 "unknown_session");
    EXPECT_PLAIN(wire.command("0000000000000001 00000000 scan_stop 0000000000000000"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("0000000000000002 00000001 scan"), "bad_request");
    EXPECT_PLAIN(wire.command("0000000000000003 00000000 scan 00"), "bad_request");
    assert(scan.starts == 0);
    EXPECT_REPLY(wire.command("0000000000000004 00000000 scan"),
                 "!01 123456789abcdef0 0000000000000004 00000000 ok 00000000 idle none "
                 "0000000000000004 1b\n");
    assert(scan.starts == 1);
    const char* waiting = "!01 123456789abcdef0 8000000000000002 00000000 ok 00000000 idle none "
                          "0000000000000004 1b scan running 00 00\n";
    EXPECT_REPLY(wire.command("8000000000000002 00000000 scan_read 0000000000000004 00"), waiting);
    // While the scan runs there are no rows, so only index zero is valid.
    EXPECT_PLAIN(wire.command("8000000000000003 00000000 scan_read 0000000000000004 01"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("8000000000000003 00000001 scan_read 0000000000000004 00"),
                 "unknown_session");
    EXPECT_PLAIN(wire.command("8000000000000003 00000000 scan_read 0000000000000005 00"),
                 "unknown_session");
    EXPECT_PLAIN(wire.command("8000000000000003 00000000 scan_read 0000000000000004"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("8000000000000003 00000000 scan_read 0000000000000004 00 00"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("8000000000000003 00000000 scan_read 0000000000000004 0"),
                 "bad_request");
    // A wrong scan ID or an extra field does not cancel the scan.
    EXPECT_PLAIN(wire.command("0000000000000005 00000000 scan_stop 0000000000000003"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("0000000000000006 00000001 scan_stop 0000000000000004"),
                 "bad_request");
    EXPECT_PLAIN(wire.command("0000000000000007 00000000 scan_stop 0000000000000004 00"),
                 "bad_request");
    assert(scan.state == ScanState::running);
    EXPECT_REPLY(wire.command("0000000000000008 00000000 scan"),
                 "!01 123456789abcdef0 0000000000000008 00000000 busy 00000000 idle none "
                 "0000000000000008 1b\n");
    assert(scan.starts == 1);
}

void scanReadReturnsRows() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000004 00000000 scan"), " ok ");
    // A complete scan with no rows also reports count zero.
    scan.state = ScanState::complete;
    EXPECT_CONTAINS(wire.command("8000000000000004 00000000 scan_read 0000000000000004 00"),
                    " ok 00000000 idle none 0000000000000004 1b scan complete 00 00\n");
    Target hidden{{}, 11};
    airrounds::session::setAddress(hidden, 0x0200000000a1);
    Target named{{}, 1};
    airrounds::session::setAddress(named, 0x0200000000b2);
    const uint8_t name[]{'A', 0xff};
    const bool added = scan.list.add(hidden, nullptr, 0) && scan.list.add(named, name, 2);
    assert(added);
    EXPECT_REPLY(wire.command("8000000000000005 00000000 scan_read 0000000000000004 00"),
                 "!01 123456789abcdef0 8000000000000005 00000000 ok 00000000 idle none "
                 "0000000000000004 1b scan complete 02 00 0200000000a1 0b -\n");
    EXPECT_REPLY(wire.command("8000000000000006 00000000 scan_read 0000000000000004 01"),
                 "!01 123456789abcdef0 8000000000000006 00000000 ok 00000000 idle none "
                 "0000000000000004 1b scan complete 02 01 0200000000b2 01 41ff\n");
    EXPECT_PLAIN(wire.command("8000000000000007 00000000 scan_read 0000000000000004 02"),
                 "bad_request");
    // A finished scan has nothing to stop.
    EXPECT_CONTAINS(wire.command("0000000000000005 00000000 scan_stop 0000000000000004"), " ok ");
    assert(scan.state == ScanState::complete);
}

void scanAndRoundShareTheReceiver() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000004 00000000 scan"), " ok ");
    scan.state = ScanState::complete;
    const bool added = scan.list.add({{2, 0, 0, 0, 0, 0xa1}, 11}, nullptr, 0) &&
                       scan.list.add({{2, 0, 0, 0, 0, 0xb2}, 1}, nullptr, 0);
    assert(added);
    // A round that is saving its last point also keeps the receiver busy.
    EXPECT_CONTAINS(wire.command("000000000000000a 00000001 round 020000000001 06"),
                    " ok 00000001 running ");
    EXPECT_PLAIN(wire.command("000000000000000b 00000000 scan"), "busy");
    wire.visit.done = true;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("8000000000000008 00000001 round_status"), " finalizing ");
    EXPECT_PLAIN(wire.command("000000000000000c 00000000 scan"), "busy");
    assert(scan.starts == 1);
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("000000000000000d 00000000 scan"), " ok ");
    assert(scan.starts == 2);
    // A new scan replaces the old ID.
    EXPECT_PLAIN(wire.command("8000000000000009 00000000 scan_read 0000000000000004 00"),
                 "unknown_session");
    // Rows from the previous scan stay hidden while the new scan runs.
    assert(scan.list.size() == 2);
    EXPECT_CONTAINS(wire.command("800000000000000a 00000000 scan_read 000000000000000d 00"),
                    " ok 00000001 complete not_observed 000000000000000d 1b scan running 00 00\n");
    EXPECT_PLAIN(wire.command("800000000000000a 00000000 scan_read 000000000000000d 01"),
                 "bad_request");
    // scan_read is a read, so a mutation ID cannot carry it.
    EXPECT_PLAIN(wire.command("000000000000000e 00000000 scan_read 000000000000000d 00"),
                 "bad_request");
    const unsigned polls = scan.polls;
    wire.protocol.poll();
    assert(scan.polls == polls + 1);
}

void scanReportsAnError() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("000000000000000d 00000000 scan"), " ok ");
    scan.state = ScanState::error;
    EXPECT_CONTAINS(wire.command("800000000000000b 00000000 scan_read 000000000000000d 00"),
                    " 1b scan error 00 00\n");
}

void mutationsCheckTheirArguments() {
    TestScan scan;
    Wire wire(boot, &scan);
    // Scope zero never names a session to start.
    EXPECT_CONTAINS(wire.command("0000000000000001 00000000 start 020000000001 06"),
                    " unknown_session 00000000 idle none ");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000000 round 020000000001 06"),
                    " unknown_session 00000000 idle none ");
    // start takes exactly one pair, and every pair needs both fields at full width.
    EXPECT_CONTAINS(wire.command("0000000000000003 00000001 start 020000000001 06 020000000002 06"),
                    " bad_request 00000000 idle none ");
    EXPECT_CONTAINS(wire.command("0000000000000005 00000001 round 020000000001 01 020000000002"),
                    " bad_request 00000000 idle none ");
    EXPECT_CONTAINS(wire.command("0000000000000006 00000001 round 02000000001 01"),
                    " bad_request 00000000 idle none ");
    EXPECT_CONTAINS(wire.command("0000000000000007 00000001 round 020000000001 1"),
                    " bad_request 00000000 idle none ");
    assert(wire.visit.starts == 0);
    EXPECT_CONTAINS(wire.command("0000000000000008 00000001 start 020000000001 06"),
                    " ok 00000001 running none ");
    // stop and round_stop take no arguments.
    EXPECT_CONTAINS(wire.command("0000000000000009 00000001 stop 00"),
                    " bad_request 00000001 running ");
    EXPECT_CONTAINS(wire.command("000000000000000a 00000001 round_stop 00"),
                    " bad_request 00000001 running ");
    assert(wire.visit.stops == 0);
    EXPECT_CONTAINS(wire.command("000000000000000b 00000001 round_stop"),
                    " ok 00000001 finalizing ");
}

void stopDuringTheLastSaveCompletesTheRound() {
    TestScan scan;
    Wire wire(boot, &scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01 020000000002 06"),
                    " running none ");
    wire.visit.done = true;
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    wire.visit.done = true;
    wire.visit.result = Outcome::none;
    wire.protocol.poll();
    assert(wire.visit.starts == 2);
    assert(wire.visit.saveId.slot == 2);
    // The last point ended on its own; a stop during its save skips nothing.
    EXPECT_REPLY(wire.command("0000000000000002 00000001 round_stop"),
                 "!01 123456789abcdef0 0000000000000002 00000001 ok 00000001 finalizing none "
                 "0000000000000002 1b aps 020000000001012 020000000002060\n");
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_REPLY(wire.command("8000000000000003 00000001 round_status_detail"),
                 "!01 123456789abcdef0 8000000000000003 00000001 ok 00000001 complete saved "
                 "0000000000000002 1b interrupted 0 aps 020000000001012 020000000002061\n");
    assert(wire.visit.stops == 2);
    assert(wire.visit.saves == 2);
}

void deadlineEndsAQuietVisit() {
    Wire wire;
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01 020000000002 06"),
                    " running none ");
    // The visit has not finished, but the round deadline of 10 s per point has passed.
    wire.visit.clockUs = 19999999;
    wire.protocol.poll();
    assert(wire.visit.stops == 0);
    wire.visit.clockUs = 20000000;
    wire.protocol.poll();
    assert(wire.visit.stops == 1);
    assert(wire.visit.saves == 1);
    assert(wire.visit.saveId.slot == 1);
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_REPLY(wire.command("8000000000000002 00000001 round_status"),
                 "!01 123456789abcdef0 8000000000000002 00000001 ok 00000001 stopped not_observed "
                 "0000000000000001 03 aps 020000000001011 020000000002060\n");
    assert(wire.visit.starts == 1);
}

const uint8_t longScanName[33]{'L'};
const Target firstRow{{2, 0, 0, 0, 0, 1}, 1};
const Target movedRow{{2, 0, 0, 0, 0, 1}, 6};

void scanListObserveRules() {
    ScanList list;
    bool accepted = list.observe({{1, 0, 0, 0, 0, 1}, 1}, -50, nullptr, 0);
    assert(!accepted);
    accepted = list.observe({{2, 0, 0, 0, 0, 1}, 14}, -50, nullptr, 0);
    assert(!accepted);
    accepted = list.observe(firstRow, -50, nullptr, 1);
    assert(!accepted);
    accepted = list.observe(firstRow, -50, longScanName, 33);
    assert(!accepted);
    assert(list.size() == 0);
    accepted = list.observe(firstRow, -50, longScanName, 32);
    assert(accepted);
    assert(list.size() == 1);
    assert(list[0].nameSize == 32);
    // The same signal or a weaker one keeps the row's channel and name.
    const uint8_t other[]{'O'};
    accepted = list.observe(movedRow, -50, other, 1);
    assert(accepted);
    assert(list.size() == 1);
    assert(list[0].target.channel == 1);
    assert(list[0].nameSize == 32);
    assert(list[0].name[0] == 'L');
    accepted = list.observe(movedRow, -49, nullptr, 0);
    assert(accepted);
    assert(list[0].target.channel == 6);
    assert(list[0].nameSize == 32);
    // The row now holds the stronger signal, so a repeat of the old one does not move it.
    accepted = list.observe(firstRow, -50, nullptr, 0) && list.observe(firstRow, -49, nullptr, 0);
    assert(accepted);
    assert(list[0].target.channel == 6);
}

void scanListNamesAHiddenRow() {
    // A hidden row takes the first name heard for it.
    ScanList list;
    const Target hidden{{2, 0, 0, 0, 0, 2}, 11};
    bool accepted = list.observe(hidden, -70, nullptr, 0) && list.observe(hidden, -70, nullptr, 0);
    assert(accepted);
    assert(list.size() == 1);
    assert(list[0].nameSize == 0);
    const uint8_t other[]{'O'};
    accepted = list.observe(hidden, -90, other, 1);
    assert(accepted);
    assert(list[0].nameSize == 1);
    assert(list[0].name[0] == 'O');
}

void scanListAddRules() {
    // add rejects invalid targets and names, and a full list.
    ScanList added;
    bool valid = added.add({{1, 0, 0, 0, 0, 1}, 1}, nullptr, 0) ||
                 added.add(firstRow, nullptr, 1) || added.add(firstRow, longScanName, 33);
    assert(!valid);
    assert(added.size() == 0);
    valid = added.add(firstRow, longScanName, 32);
    assert(valid);
    // An added row has the weakest signal, so any report updates it.
    valid = added.observe(movedRow, -100, nullptr, 0);
    assert(valid);
    assert(added[0].target.channel == 6);
    for (uint8_t index = 2; index <= ScanList::capacity; ++index) {
        valid = added.add({{2, 0, 0, 0, 0, index}, 1}, nullptr, 0);
        assert(valid);
    }
    valid = added.add({{2, 0, 0, 0, 0, 17}, 1}, nullptr, 0);
    assert(!valid);
    assert(added.size() == ScanList::capacity);
}

void scanListReplacesTheFirstWeakestRow() {
    // In a full list, a tie with the weakest row does not replace it; the first weakest row goes.
    ScanList full;
    for (uint8_t index = 1; index <= ScanList::capacity; ++index) {
        const bool added = full.observe({{2, 0, 0, 0, 0, index}, 1}, -80, longScanName, 32);
        assert(added);
    }
    bool valid = full.observe({{2, 0, 0, 0, 0, 17}, 1}, -80, nullptr, 0);
    assert(!valid);
    valid = full.observe({{2, 0, 0, 0, 0, 18}, 2}, -79, nullptr, 0);
    assert(valid);
    assert(full.size() == ScanList::capacity);
    assert(full[0].target.bssid[5] == 18);
    assert(full[0].target.channel == 2);
    assert(full[1].target.bssid[5] == 2);
    assert(full[ScanList::capacity - 1].target.bssid[5] == 16);
    // The replaced row keeps nothing from the old one.
    const uint8_t empty[sizeof(ScanEntry::name)]{};
    assert(full[0].nameSize == 0);
    assert(memcmp(full[0].name, empty, sizeof(empty)) == 0);
}

void clientChecksScanRows() {
    const std::string header = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle "
                               "none 0000000000000001 1b scan ";
    Reply reply;
    bool valid = readReply((header + "complete 01 00 020000000005 06 -\n").c_str(), reply);
    assert(valid);
    assert(reply.hasScan);
    assert(reply.scan.entry.nameSize == 0);
    valid = readReply((header + "complete 01 00 020000000005 06 4142\n").c_str(), reply);
    assert(valid);
    assert(reply.scan.entry.nameSize == 2);
    assert(reply.scan.entry.name[1] == 'B');
    // A row needs its name field, even for a hidden network.
    valid = readReply((header + "complete 01 00 020000000005 06\n").c_str(), reply);
    assert(!valid);
    valid = readReply((header + "complete 01 00 020000000005 06 -  \n").c_str(), reply);
    assert(!valid);
    valid = readReply((header + "complete 01 00 020000000005 06 414\n").c_str(), reply);
    assert(!valid);
    valid = readReply((header + "complete 01 00 020000000005\n").c_str(), reply);
    assert(!valid);
}

bool clientReads(const char* features, const std::string& details) {
    const std::string line = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle "
                             "none 0000000000000001 " +
                             std::string(features) + details + "\n";
    Reply reply;
    return readReply(line.c_str(), reply);
}

void clientChecksScanDetails() {
    const std::string row = " 020000000005 06 -";
    // Scan fields need the scan feature, a known state, and a count of at most 16.
    assert(clientReads("1b", " scan complete 10 0f" + row));
    assert(!clientReads("13", " scan complete 01 00" + row));
    assert(!clientReads("1b", " scan complete 11 00" + row));
    assert(!clientReads("1b", " scan complete 01 01" + row));
    assert(!clientReads("1b", " scan complete 01 02" + row));
    // Only a nonempty complete scan carries a row; the others report 00 00 and stop there.
    assert(clientReads("1b", " scan running 00 00"));
    assert(!clientReads("1b", " scan running 01 00"));
    assert(!clientReads("1b", " scan running 01 00" + row));
    assert(!clientReads("1b", " scan running 00 01"));
    assert(!clientReads("1b", " scan complete 00 01"));
    assert(!clientReads("1b", " scan complete 00 00 -"));
    // A row needs a valid target and a whole SSID of at most 32 bytes in lowercase hex.
    assert(!clientReads("1b", " scan complete 01 00 010000000005 06 -"));
    assert(!clientReads("1b", " scan complete 01 00 020000000005 0e -"));
    assert(clientReads("1b", " scan complete 01 00 020000000005 06 " + std::string(64, 'a')));
    assert(!clientReads("1b", " scan complete 01 00 020000000005 06 " + std::string(66, 'a')));
    assert(!clientReads("1b", " scan complete 01 00 020000000005 06 4g41"));
    assert(!clientReads("1b", " scan complete 01 00 020000000005 06 41G1"));
}

void firmwareReadReportsTheVersion() {
    Wire wire;
    const std::string version(airrounds::firmwareVersion);
    // Every board answers the read, whatever features it advertises.
    EXPECT_REPLY(wire.command("8000000000000001 00000000 firmware"),
                 "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                 "0000000000000000 03 firmware " +
                     version + "\n");
    // The read needs scope zero, no arguments, a read request ID, and this boot.
    EXPECT_CONTAINS(wire.command("8000000000000003 00000001 firmware"), "unknown_session");
    EXPECT_CONTAINS(wire.command("8000000000000004 00000000 firmware 01"), "bad_request");
    // A mutation ID is refused and leaves the last mutation where it was.
    EXPECT_CONTAINS(wire.command("0000000000000005 00000000 firmware"),
                    " bad_request 00000000 idle none 0000000000000000 ");
    EXPECT_CONTAINS(wire.send("@01 0000000000000001 8000000000000006 00000000 firmware\n"),
                    "wrong_boot");
    assert(wire.command("8000000000000007 00000000 firmware 01").find(" firmware ") ==
           std::string::npos);
}

void clientChecksFirmware() {
    // A version is one to 15 characters of digits and dots, and nothing follows it.
    assert(clientReads("03", " firmware 0.1"));
    assert(clientReads("1f", " firmware 12.345"));
    assert(clientReads("03", " firmware " + std::string(15, '1')));
    assert(!clientReads("03", " firmware " + std::string(16, '1')));
    assert(!clientReads("03", " firmware"));
    assert(!clientReads("03", " firmware 0.1a"));
    assert(!clientReads("03", " firmware 0.1 extra"));
    assert(!clientReads("03", " firmware 0.1 aps"));
    Reply reply;
    const std::string line = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle "
                             "none 0000000000000001 03 firmware 0.1\n";
    const bool read = readReply(line.c_str(), reply);
    assert(read);
    assert(reply.hasFirmware);
    assert(std::string(reply.firmware) == "0.1");
}

void clientChecksRoundDetails() {
    // Journal, interruption, and round fields each need their feature bit.
    assert(clientReads("1f", " journal saved interrupted 1 aps"));
    assert(!clientReads("1b", " journal saved interrupted 1 aps"));
    assert(!clientReads("07", " journal saved interrupted 1 aps"));
    assert(clientReads("07", " journal saved aps"));
    assert(!clientReads("1f", " journal saved interrupted 2 aps"));
    assert(!clientReads("01", " aps"));
    assert(!clientReads("03", " bogus"));
    assert(!clientReads("03", " aps 020000000001061 020000000001061"));
    assert(!clientReads("03", " aps 0200000000010610"));
    assert(!clientReads("03", " aps 020000000001065"));
    assert(clientReads("1b", " aps 020000000001061 020000000002061 020000000003061 "
                             "020000000004061 020000000005061"));
    assert(!clientReads("1b", " aps 020000000001061 020000000002061 020000000003061 "
                              "020000000004061 020000000005061 020000000006061"));

    // The client drops a reply line it cannot hold, without overrunning its buffer.
    assert(!clientReads("03", " aps" + std::string(300, ' ')));
}

void clientRejectsReplyBootZero() {
    const ReplyKey hello{0, 0x800000000000002aULL, 0};
    const char* reply = "!01 0000000000000000 800000000000002a 00000000 ok 00000000 idle none "
                        "0000000000000000 03\n";
    assert(!matchesReply(reply, hello));
    // Boot zero in the expected key is for hello only: a read with scope zero.
    const char* board = "!01 123456789abcdef0 800000000000002a 00000000 ok 00000000 idle none "
                        "0000000000000000 03\n";
    assert(matchesReply(board, hello));
    assert(!matchesReply(board, {0, 0x800000000000002aULL, 1}));
    const char* mutation = "!01 123456789abcdef0 000000000000002a 00000000 ok 00000000 idle none "
                           "0000000000000000 03\n";
    assert(!matchesReply(mutation, {0, 0x2a, 0}));
    assert(matchesReply(mutation, {boot, 0x2a, 0}));
    const char* scoped = "!01 123456789abcdef0 800000000000002a 00000001 ok 00000000 idle none "
                         "0000000000000000 03\n";
    assert(!matchesReply(scoped, hello));
    assert(!matchesReply(scoped, {0, 0x800000000000002aULL, 1}));
    assert(matchesReply(scoped, {boot, 0x800000000000002aULL, 1}));
    // An invalid scope is an invalid envelope, even as the last envelope field.
    const char* badScope = "!01 123456789abcdef0 800000000000002a 0000000g ok 00000000 idle none "
                           "0000000000000000 03\n";
    assert(!matchesReply(badScope, {boot, 0x800000000000002aULL, 0}));
    Reply parsed;
    const bool badScopeRead = readReply(badScope, parsed);
    assert(!badScopeRead);
    // The line must end with its line feed; the last field is not trimmed instead.
    std::string unterminated = board;
    unterminated.back() = 'x';
    const bool unterminatedRead = readReply(unterminated.c_str(), parsed);
    assert(!unterminatedRead);
}

} // namespace

int main() try {
    alarm(30); // A loop that never ends fails the test instead of hanging it.
    negotiationAndValidation();
    retriesAndSave();
    quietStopCompletesInReply();
    interruptedStream();
    replyFraming();
    staleTraffic();
    replayAfterStateChange();
    failures();
    roundRejectsInvalidProfiles();
    roundCompletesEverySlot();
    scanContract();
    scanCancelReportsCleanupError();
    roundDetailsDistinguishAnInterruption();
    scanKeepsStrongerRows();
    fivePointContract();
    roundStopsOnStorageError();
    roundKeepsItsActiveVisit();
    stopAfterTheDeadlineIsNotAnInterruption();
    stopWithASavedPointIsNotAnInterruption();
    stopDuringPreparationIsAnInterruption();
    framingDropsBadFrames();
    envelopeRejectsBadFields();
    helloNeedsAReadAtBootZero();
    readsCheckScopeAndArguments();
    detailWithJournal();
    scanRequestsCheckTheirFields();
    scanReadReturnsRows();
    scanAndRoundShareTheReceiver();
    scanReportsAnError();
    clientRejectsReplyBootZero();
    clientChecksScanRows();
    clientChecksScanDetails();
    clientChecksRoundDetails();
    firmwareReadReportsTheVersion();
    clientChecksFirmware();
    aSavedRoundDoesNotCarryOver();
    mutationsCheckTheirArguments();
    stopDuringTheLastSaveCompletesTheRound();
    deadlineEndsAQuietVisit();
    scanListObserveRules();
    scanListNamesAHiddenRow();
    scanListAddRules();
    scanListReplacesTheFirstWeakestRow();
    std::cout << "UART protocol: 43 groups passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
