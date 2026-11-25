// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "uart_protocol.h"

namespace {

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

void expectReply(const std::string& actual, const std::string& expected, int line) {
    if (actual != expected) {
        std::cerr << "selftest_test.cpp:" << line
                  << ": reply failed\n  expected: " << visible(expected)
                  << "\n  actual:   " << visible(actual) << '\n';
        std::abort();
    }
}

void expectContains(const std::string& actual, const std::string& part, int line) {
    if (actual.find(part) == std::string::npos) {
        std::cerr << "selftest_test.cpp:" << line
                  << ": reply contains failed\n  wanted: " << visible(part)
                  << "\n  actual: " << visible(actual) << '\n';
        std::abort();
    }
}

#define EXPECT_REPLY(actual, expected) expectReply((actual), (expected), __LINE__)
#define EXPECT_CONTAINS(actual, part) expectContains((actual), (part), __LINE__)

using airrounds::session::Target;
using namespace airrounds::uart;

constexpr uint64_t boot = 0x123456789abcdef0ULL;

// A backend that supports the self-test. runSelfTest and cardMounted return whatever the test sets,
// so the test drives the wire format without hardware.
class SelfTestVisit final : public VisitBackend {
public:
    uint64_t nowUs() const override { return clockUs; }
    VisitStart start(const Target&, const VisitBudget&) override {
        ++starts;
        done = false;
        result = Outcome::none;
        receiving = true;
        return VisitStart::listening;
    }
    bool observationDone() const override { return done; }
    bool stopReception() override {
        receiving = false;
        return true;
    }
    void beginSave(const CaptureId&) override {}
    Outcome saveResult() const override { return result; }

    bool supportsSelfTest() const override { return support; }
    bool cardMounted() override {
        ++cardProbes;
        return card;
    }
    SelfTest runSelfTest() override {
        ++selfTests;
        return test;
    }

    uint64_t clockUs = 0;
    bool done = false;
    bool receiving = false;
    Outcome result = Outcome::none;
    bool support = true;
    bool card = true;
    SelfTest test;
    unsigned cardProbes = 0;
    unsigned selfTests = 0;
    unsigned starts = 0;
};

class TestScan final : public ScanBackend {
public:
    void startScan() override { state = ScanState::running; }
    void pollScan() override {}
    void stopScan() override { state = ScanState::idle; }
    ScanState scanState() const override { return state; }
    const ScanList& scanList() const override { return list; }

    ScanState state = ScanState::idle;
    ScanList list;
};

class Wire {
public:
    explicit Wire(ScanBackend* scan = nullptr) : protocol(visit, boot, scan) {}

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

    std::string command(const char* tail) {
        return send(std::string("@01 123456789abcdef0 ") + tail + "\n");
    }

    SelfTestVisit visit;
    Protocol protocol;
    uint32_t now = 0;
};

// Feature bit 6 rides on the advertised features; with a scan the board advertises 7b.
void advertisesTheSelfTestBit() {
    Wire plain;
    EXPECT_CONTAINS(plain.command("8000000000000001 00000000 status"),
                    " idle none 0000000000000000 43\n");
    TestScan scan;
    Wire wire(&scan);
    EXPECT_CONTAINS(wire.command("8000000000000001 00000000 status"),
                    " idle none 0000000000000000 7b\n");
}

void cardReportsTheMountState() {
    Wire wire;
    wire.visit.card = true;
    EXPECT_REPLY(wire.command("8000000000000001 00000000 card"),
                 "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                 "0000000000000000 43 card 1\n");
    wire.visit.card = false;
    EXPECT_REPLY(wire.command("8000000000000002 00000000 card"),
                 "!01 123456789abcdef0 8000000000000002 00000000 ok 00000000 idle none "
                 "0000000000000000 43 card 0\n");
    assert(wire.visit.cardProbes == 2);
    // A card reply parses back to the mount state.
    Reply reply;
    const auto mounted = wire.command("8000000000000003 00000000 card");
    const bool decoded = readReply(mounted.c_str(), reply);
    assert(decoded);
    assert(reply.hasCard && !reply.card);
}

void selfTestEncodesEachCheck() {
    Wire wire;
    wire.visit.test = {CheckResult::pass, CheckResult::pass, CheckResult::pass, CheckResult::pass};
    EXPECT_REPLY(wire.command("8000000000000001 00000000 selftest"),
                 "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                 "0000000000000000 43 selftest pppp\n");
    // A failed microSD mount blocks the test PCAP write.
    wire.visit.test = {CheckResult::fail, CheckResult::pass, CheckResult::pass,
                       CheckResult::blocked};
    const auto blocked = wire.command("8000000000000002 00000000 selftest");
    EXPECT_CONTAINS(blocked, " selftest fppb\n");
    Reply reply;
    const bool decoded = readReply(blocked.c_str(), reply);
    assert(decoded);
    assert(reply.hasSelfTest);
    assert(reply.selfTest.sd == CheckResult::fail);
    assert(reply.selfTest.radio == CheckResult::pass);
    assert(reply.selfTest.probe == CheckResult::pass);
    assert(reply.selfTest.write == CheckResult::blocked);
    // A blocked radio reports its dependent probe as not run.
    wire.visit.test = {CheckResult::pass, CheckResult::fail, CheckResult::not_run,
                       CheckResult::pass};
    EXPECT_CONTAINS(wire.command("8000000000000003 00000000 selftest"), " selftest pf-p\n");
    assert(wire.visit.selfTests == 3);
}

void commandsCheckScopeAndArguments() {
    Wire wire;
    // Scope must be zero: these are board-level reads.
    EXPECT_CONTAINS(wire.command("8000000000000001 00000001 card"), " unknown_session ");
    EXPECT_CONTAINS(wire.command("8000000000000002 00000001 selftest"), " unknown_session ");
    // Read requests take no arguments.
    EXPECT_CONTAINS(wire.command("8000000000000003 00000000 card 00"), " bad_request ");
    EXPECT_CONTAINS(wire.command("8000000000000004 00000000 selftest 00"), " bad_request ");
    // A read command needs a read request ID; a mutation ID cannot carry it.
    EXPECT_CONTAINS(wire.command("0000000000000001 00000000 card"), " bad_request ");
    EXPECT_CONTAINS(wire.command("0000000000000002 00000000 selftest"), " bad_request ");
    assert(wire.visit.cardProbes == 0);
    assert(wire.visit.selfTests == 0);
}

void anOlderBoardRejectsTheCommands() {
    Wire wire;
    wire.visit.support = false;
    EXPECT_CONTAINS(wire.command("8000000000000001 00000000 status"),
                    " idle none 0000000000000000 03\n");
    EXPECT_CONTAINS(wire.command("8000000000000002 00000000 card"), " bad_request ");
    EXPECT_CONTAINS(wire.command("8000000000000003 00000000 selftest"), " bad_request ");
    assert(wire.visit.cardProbes == 0);
    assert(wire.visit.selfTests == 0);
}

void aRunningRoundOrScanBlocksTheCommands() {
    TestScan scan;
    Wire wire(&scan);
    EXPECT_CONTAINS(wire.command("0000000000000001 00000001 round 020000000001 01"), " running ");
    EXPECT_CONTAINS(wire.command("8000000000000002 00000000 card"), " busy ");
    EXPECT_CONTAINS(wire.command("8000000000000003 00000000 selftest"), " busy ");
    // The round's microSD preflight probed the card once; the busy commands probe nothing.
    assert(wire.visit.cardProbes == 1);
    assert(wire.visit.selfTests == 0);
    // Finish the round, then a scan also blocks the commands.
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    EXPECT_CONTAINS(wire.command("0000000000000004 00000000 scan"), " ok ");
    EXPECT_CONTAINS(wire.command("8000000000000005 00000000 selftest"), " busy ");
    EXPECT_CONTAINS(wire.command("0000000000000006 00000000 scan_stop 0000000000000004"), " ok ");
    // With the receiver free again, the self-test runs.
    EXPECT_CONTAINS(wire.command("8000000000000007 00000000 selftest"), " selftest ");
    assert(wire.visit.selfTests == 1);
}

// A selftest or card reply from a board without feature bit 6 does not parse.
void theParserRequiresTheFeatureBit() {
    const char* withoutBit = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                             "0000000000000000 03 card 1\n";
    const char* withBit = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                          "0000000000000000 43 card 1\n";
    Reply reply;
    bool decoded = readReply(withoutBit, reply);
    assert(!decoded);
    decoded = readReply(withBit, reply);
    assert(decoded);
    assert(reply.hasCard && reply.card);
    // A self-test field that is not four characters is rejected.
    const char* shortTest = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                            "0000000000000000 43 selftest ppp\n";
    decoded = readReply(shortTest, reply);
    assert(!decoded);
    const char* badChar = "!01 123456789abcdef0 8000000000000001 00000000 ok 00000000 idle none "
                          "0000000000000000 43 selftest pppx\n";
    decoded = readReply(badChar, reply);
    assert(!decoded);
}

// The microSD preflight: a card-capable board fails a round before it opens the radio when the card
// is absent, so a card-less round reports storage_error up front instead of saving nothing.
void cardPreflightBlocksARound() {
    airrounds::session::Profile profile;
    const bool added = profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);

    SelfTestVisit absent;
    absent.card = false;
    PassiveRound blocked(absent);
    const bool started = blocked.start({boot, 1}, profile);
    assert(started);
    assert(blocked.state() == State::error);
    assert(blocked.outcome() == Outcome::storage_error);
    assert(absent.starts == 0); // The radio never opened.

    SelfTestVisit present;
    present.card = true;
    PassiveRound running(present);
    running.start({boot, 1}, profile);
    assert(running.state() == State::running);
    assert(present.starts == 1);
}

} // namespace

int main() {
    advertisesTheSelfTestBit();
    cardPreflightBlocksARound();
    cardReportsTheMountState();
    selfTestEncodesEachCheck();
    commandsCheckScopeAndArguments();
    anOlderBoardRejectsTheCommands();
    aRunningRoundOrScanBlocksTheCommands();
    theParserRequiresTheFeatureBit();
    std::cout << "selftest_test ok\n";
    return 0;
}
