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

class Loopback {
public:
    Loopback() : client(write, this, 0x42), board(visit, boot) {}

    std::string deliver() {
        std::string reply;
        for (char byte : outbound) {
            char line[Protocol::replyCapacity];
            if (board.receive({static_cast<uint8_t>(byte), now}, line)) {
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
        const char* request = visit.journalSupported && rounds ? " round_status_log\n"
                              : rounds                         ? " round_status\n"
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
    uint32_t now = 0;
    std::string outbound;
};

void stopAndSave() {
    Loopback link;
    link.handshake();
    assert(link.client.start(target, link.now));
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
    assert(link.client.start(target, link.now));
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
    assert(link.client.start(target, link.now));
    const auto dropped = link.outbound;
    link.outbound.clear();
    link.now += 950;
    link.client.poll(link.now);
    link.now += 500;
    link.handshake();
    assert(link.visit.starts == 0);
    assert(link.client.start(target, link.now));
    assert(link.outbound != dropped);
    assert(link.outbound.find(" 0000000000000002 00000001 round ") != std::string::npos);
    link.receive(link.deliver());
    assert(link.visit.starts == 1);
}

void stopKeepsTheSelectedVisit() {
    Loopback link;
    link.handshake();
    assert(link.client.start(target, link.now));
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
    assert(link.client.start(changed, link.now));
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
    assert(link.client.start(target, link.now));
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
    assert(!link.client.start(target, link.now));
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
    assert(link.client.start(target, link.now));
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
    assert(link.client.start(target, link.now));
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
    assert(link.client.start(target, link.now));
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
    std::cout << "Flipper client checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
