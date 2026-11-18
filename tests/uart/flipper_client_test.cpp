#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../../flipper/session_client.h"
#include "../../flipper/visit_labels.h"
#include "uart_protocol.h"

namespace {
using airrounds::flipper::Link;
using airrounds::flipper::SessionClient;
using namespace airrounds::uart;

constexpr airrounds::session::Target target{{2, 0, 0, 0, 0, 1}, 6};
constexpr uint64_t boot = 0x123456789abcdef0ULL;

class Visit final : public VisitBackend {
public:
    bool start(const airrounds::session::Target& selected) override {
        assert(selected.channel == target.channel && selected.bssid[0] == target.bssid[0]);
        ++starts;
        done = false;
        outcome = Outcome::none;
        return true;
    }
    bool observationDone() const override { return done; }
    bool stopReception() override {
        ++stops;
        return true;
    }
    void beginSave(VisitId) override {}
    Outcome saveResult() const override { return outcome; }

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

    void handshake() {
        client.poll(now);
        assert(outbound.find(" hello\n") != std::string::npos);
        receive("ROM: reset" + deliver());
        assert(client.link() != Link::ready);
        client.poll(now);
        assert(outbound.find(" status\n") != std::string::npos);
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
    auto labels = airrounds::flipper::visitLabels(link.client);
    assert(std::string(labels.state) == "Reception stopped");
    assert(std::string(labels.detail) == "Saving...");
    assert(!link.client.stop(link.client.visitId(), link.now));
    link.visit.outcome = Outcome::saved;
    link.board.poll();
    link.now += 500;
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(link.client.state() == State::stopped);
    assert(link.client.outcome() == Outcome::saved);
    labels = airrounds::flipper::visitLabels(link.client);
    assert(std::string(labels.state) == "Stopped");
    assert(std::string(labels.detail) == "Saved");
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
    assert(link.outbound.find(" 0000000000000002 00000001 start ") != std::string::npos);
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
    assert(std::string(airrounds::flipper::visitLabels(link.client).state) == "Start rejected");
    link.client.refresh(link.now);
    link.client.poll(link.now);
    link.receive(link.deliver());
    assert(std::string(airrounds::flipper::visitLabels(link.client).state) == "Listening");
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
    const auto labels = airrounds::flipper::visitLabels(link.client);
    assert(std::string(labels.state) == "Error");
    assert(std::string(labels.detail) == "SD save failed");

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
    std::cout << "Flipper client checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
