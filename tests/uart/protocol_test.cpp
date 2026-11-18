#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "uart_protocol.h"

namespace {

using airrounds::session::Target;
using namespace airrounds::uart;

constexpr uint64_t boot = 0x123456789abcdef0ULL;

class Visit final : public VisitBackend {
public:
    bool start(const Target&) override {
        assert(!receiving);
        ++starts;
        done = false;
        result = Outcome::none;
        receiving = radioStarts;
        return radioStarts;
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

    unsigned starts = 0;
    unsigned stops = 0;
    unsigned saves = 0;
    bool done = false;
    bool receiving = false;
    bool radioStarts = true;
    bool radioStops = true;
    Outcome result = Outcome::none;
    CaptureId saveId{};
};

class Wire {
public:
    explicit Wire(uint64_t bootId = boot) : protocol(visit, bootId) {}

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
    assert(wire.send("@01 0000000000000000 8000000000000011 00000000 hello\n") ==
           "!01 123456789abcdef0 8000000000000011 00000000 ok 00000000 idle none 0000000000000000 "
           "03\n");
    assert(wire.command("0000000000000012 00000001 start 020000000001 06", "02") ==
           "!02 123456789abcdef0 0000000000000012 00000001 unsupported_version 00000000 idle none "
           "0000000000000000 03\n");
    assert(wire.send("@01 0000000000000000 0000000000000013 00000001 start 020000000001 06\n") ==
           "!01 0000000000000000 0000000000000013 00000001 wrong_boot 00000000 idle none "
           "0000000000000000 03\n");
    assert(wire.command("0000000000000014 00000001 start 010000000001 06").find("bad_target") !=
           std::string::npos);
    assert(wire.command("0000000000000015 00000001 start 020000000001 0e").find("bad_target") !=
           std::string::npos);
    assert(wire.command("0000000000000016 00000001 start").find("bad_request") !=
           std::string::npos);
    assert(
        wire.command("0000000000000017 00000002 start 020000000001 06").find("unknown_session") !=
        std::string::npos);
    assert(wire.command("8000000000000018 00000001 start 020000000001 06").find("bad_request") !=
           std::string::npos);
    assert(wire.send("@02 0000000000000000 8000000000000019 00000000 hello\n")
               .find("unsupported_version") != std::string::npos);
    assert(wire.visit.starts == 0);
}

void retriesAndSave() {
    Wire wire;
    const char* start = "0000000000000020 00000001 start 020000000001 06";
    const char* stop = "0000000000000025 00000001 stop";
    const std::string running = wire.command(start);
    assert(running == "!01 123456789abcdef0 0000000000000020 00000001 ok 00000001 running none "
                      "0000000000000020 03\n");
    assert(wire.command(start) == running);
    assert(wire.command(stop, "02").find("unsupported_version") != std::string::npos);
    assert(wire.visit.stops == 0);
    assert(wire.command("0000000000000022 00000001 start 020000000002 06").find("conflict") !=
           std::string::npos);
    assert(wire.command("0000000000000023 00000002 start 020000000001 06").find("busy") !=
           std::string::npos);
    const std::string stopped = wire.command(stop);
    assert(stopped == "!01 123456789abcdef0 0000000000000025 00000001 ok 00000001 finalizing none "
                      "0000000000000025 03\n");
    assert(wire.command(stop) == stopped);
    assert(wire.command("8000000000000024 00000000 status").find("00000001 finalizing none") !=
           std::string::npos);
    wire.protocol.poll();
    wire.visit.result = Outcome::saved;
    wire.protocol.poll();
    assert(wire.command(stop).find("00000001 stopped saved") != std::string::npos);
    assert(wire.command("0000000000000023 00000002 start 020000000001 06").find("stale_request") !=
           std::string::npos);
    assert(wire.command(start).find("00000001 stopped saved") != std::string::npos);
    assert(wire.visit.starts == 1 && wire.visit.stops == 1 && wire.visit.saves == 1);
    assert(wire.visit.saveId.visit.matches({boot, 1}) && wire.visit.saveId.slot == 0);
}

void quietStopCompletesInReply() {
    Wire wire;
    wire.command("0000000000000001 00000001 start 020000000001 06");
    wire.visit.result = Outcome::not_observed;
    const auto reply = wire.command("0000000000000002 00000001 stop");
    assert(reply.find("00000001 stopped not_observed") != std::string::npos);
    assert(wire.visit.stops == 1);
}

void interruptedStream() {
    Wire wire;
    const char* start = "0000000000000030 00000001 start 020000000001 06";
    wire.now = UINT32_MAX - 50U;
    assert(wire.send("@01 123456789abcdef0 0000000000000030 00000001 start ").empty());
    wire.now += 101U;
    assert(wire.send("020000000001 06\n").empty());
    assert(wire.visit.starts == 0);
    assert(wire.send("@01 123456789abcdef0 ").empty());
    assert(wire.command(start).find("00000001 running none") != std::string::npos);
    wire.visit.done = true;
    wire.protocol.poll();
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    assert(
        wire.command("8000000000000031 00000000 status").find("00000001 complete not_observed") !=
        std::string::npos);
    assert(wire.command(start).find("00000001 complete not_observed") != std::string::npos);
    assert(wire.visit.starts == 1 && wire.visit.stops == 1);
    assert(wire.send("@" + std::string(200, 'x') + "\n").empty());
    assert(wire.send(std::string("@01\0\n", 5)).empty());
    assert(wire.command("8000000000000032 00000000 status").find("complete") != std::string::npos);
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
    assert(wire.command("0000000000000042 00000001 stop").find("stale_request") !=
           std::string::npos);
    assert(wire.command("0000000000000040 00000001 start 020000000001 06").find("stale_request") !=
           std::string::npos);
    assert(wire.command("0000000000000045 00000001 stop").find("unknown_session") !=
           std::string::npos);
    assert(wire.visit.starts == 2 && wire.visit.stops == 1);
    Wire rebooted(boot + 1);
    const std::string wrongBoot =
        rebooted.command("0000000000000040 00000001 start 020000000001 06");
    assert(wrongBoot.find("wrong_boot") != std::string::npos);
    assert(matchesReply(wrongBoot.c_str(), {boot, 0x40, 1}));
    assert(!matchesReply(wrongBoot.c_str(), {boot + 1, 0x40, 1}));
    assert(rebooted.visit.starts == 0);
    const std::string wrongVersion = wire.command("0000000000000046 00000002 stop", "02");
    assert(wrongVersion.find("unsupported_version") != std::string::npos);
    assert(!matchesReply(wrongVersion.c_str(), {boot, 0x46, 2}));
}

void replayAfterStateChange() {
    Wire wire;
    const char* oldStop = "0000000000000060 00000001 stop";
    assert(wire.command(oldStop).find("unknown_session") != std::string::npos);
    assert(wire.command("0000000000000060 00000001 start 020000000001 06").find("conflict") !=
           std::string::npos);
    wire.command("0000000000000061 00000001 start 020000000001 06");
    const char* busyStart = "0000000000000062 00000002 start 020000000001 06";
    assert(wire.command(busyStart).find("busy") != std::string::npos);
    wire.visit.done = true;
    wire.visit.result = Outcome::not_observed;
    wire.protocol.poll();
    assert(wire.command(busyStart).find("busy") != std::string::npos);
    assert(wire.command("0000000000000063 00000002 start 020000000001 06").find("running") !=
           std::string::npos);
    assert(wire.command(oldStop).find("stale_request") != std::string::npos);
    assert(wire.visit.starts == 2 && wire.visit.stops == 1);
}

void roundRejectsInvalidProfiles() {
    Wire wire;
    assert(wire.command("0000000000000001 00000001 round").find("bad_request") !=
           std::string::npos);
    assert(wire.command("0000000000000002 00000001 round 020000000001 06 020000000001 0b")
               .find("bad_target") != std::string::npos);
    assert(wire.command("0000000000000003 00000001 round 020000000001 06 020000000002 0e")
               .find("bad_target") != std::string::npos);
    assert(wire.command("0000000000000004 00000001 round 020000000001 06 020000000002 0b "
                        "020000000003 01 020000000004 0d")
               .empty());
    assert(wire.visit.starts == 0);
}

void roundCompletesEverySlot() {
    Wire wire;
    const char* start =
        "0000000000000001 00000001 round 020000000001 06 020000000002 06 020000000003 0b";
    const char* status = "8000000000000002 00000000 round_status";
    assert(wire.command(start).find("ok 00000001 running none") != std::string::npos);
    assert(wire.command(start).find("ok 00000001 running none") != std::string::npos);
    assert(wire.visit.starts == 1);
    wire.visit.done = true;
    wire.protocol.poll();
    assert(wire.command(status).find("finalizing none") != std::string::npos);
    assert(wire.visit.starts == 1 && wire.visit.saveId.slot == 1);
    assert(wire.command("0000000000000003 00000001 round 020000000002 06 020000000001 06 "
                        "020000000003 0b")
               .find("conflict") != std::string::npos);
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
    assert(wire.command(status).find("complete saved") != std::string::npos);
    assert(wire.visit.starts == 3 && wire.visit.stops == 3 && wire.visit.saves == 3);
    assert(wire.visit.saveId.visit.matches({boot, 1}) && wire.visit.saveId.slot == 3);
    Reply parsed;
    const auto completed = wire.command(status);
    const bool valid = readReply(completed.c_str(), parsed);
    assert(valid);
    assert(parsed.hasRound && parsed.round.profile.size() == 3 && parsed.round.next() == 3);
    assert(parsed.round.results[0] == Outcome::saved &&
           parsed.round.results[1] == Outcome::not_observed &&
           parsed.round.results[2] == Outcome::saved);
    const auto legacy = wire.command("8000000000000004 00000000 status");
    const bool legacyValid = readReply(legacy.c_str(), parsed);
    assert(legacyValid && !parsed.hasRound);
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
    assert(!emptyAccepted && round.state() == State::idle && visit.starts == 0);

    const bool valid = profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(valid);
    const bool started = round.start({boot, 1}, profile);
    const bool startedWhileRunning = round.start({boot, 2}, profile);
    assert(started && !startedWhileRunning && round.state() == State::running && visit.starts == 1);

    visit.done = true;
    round.poll();
    assert(round.state() == State::finalizing && visit.saveId.visit.matches({boot, 1}));
    const bool startedWhileSaving = round.start({boot, 2}, profile);
    assert(!startedWhileSaving && round.state() == State::finalizing && visit.starts == 1);

    visit.result = Outcome::saved;
    round.poll();
    assert(round.state() == State::complete);
    const bool restarted = round.start({boot, 2}, profile);
    assert(restarted && round.state() == State::running && visit.starts == 2);
}

void roundStopsOnStorageError() {
    Wire wire;
    const auto start = wire.command(
        "0000000000000001 00000001 round 020000000001 01 020000000002 06 020000000003 0b");
    assert(start.find("running none") != std::string::npos);
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
    assert(reply.state == State::error && reply.outcome == Outcome::storage_error);
    assert(reply.round.results[0] == Outcome::saved);
    assert(reply.round.results[1] == Outcome::storage_error);
    assert(reply.round.results[2] == Outcome::none);
    assert(wire.visit.starts == 2 && wire.visit.saves == 2);
}

void failures() {
    Wire wire;
    wire.visit.radioStarts = false;
    const char* start = "0000000000000050 00000001 start 020000000001 06";
    assert(wire.command(start).find("00000001 error radio_error") != std::string::npos);
    assert(wire.command(start).find("00000001 error radio_error") != std::string::npos);
    assert(wire.visit.starts == 1);
    wire.visit.radioStarts = true;
    wire.command("0000000000000051 00000002 start 020000000001 06");
    wire.command("0000000000000052 00000002 stop");
    wire.visit.result = Outcome::storage_error;
    wire.protocol.poll();
    assert(wire.command("8000000000000053 00000002 status").find("00000002 error storage_error") !=
           std::string::npos);
    wire.command("0000000000000054 00000003 start 020000000001 06");
    wire.visit.radioStops = false;
    assert(wire.command("0000000000000055 00000003 stop").find("00000003 error radio_error") !=
           std::string::npos);
    assert(wire.visit.saves == 1);
}

} // namespace

int main() try {
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
    roundStopsOnStorageError();
    roundKeepsItsActiveVisit();
    std::cout << "UART protocol: 12 groups passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
