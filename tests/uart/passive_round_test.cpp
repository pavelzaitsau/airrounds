#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "../../firmware/esp32/airrounds_uart/capture_path.h"
#include "passive_session.h"

namespace {

using namespace airrounds;
using namespace airrounds::uart;
using session::PassiveSession;
using session::Target;

constexpr uint64_t boot = 0x123456789abcdef0ULL;
constexpr Target first{{2, 0, 0, 0, 0, 1}, 6};
constexpr Target moved{{2, 0, 0, 0, 0, 2}, 13};
constexpr Target quietSecond{{2, 0, 0, 0, 0, 2}, 6};

std::array<uint8_t, 45> beacon(const Target& target) {
    std::array<uint8_t, 45> frame{0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(frame.data() + 10, target.bssid, sizeof(target.bssid));
    memcpy(frame.data() + 16, target.bssid, sizeof(target.bssid));
    frame[32] = 100; // Beacon interval.
    frame[34] = 1;   // Infrastructure network.
    frame[37] = 1;
    frame[38] = 'A';
    frame[39] = 1;
    frame[40] = 1;
    frame[41] = 0x82;
    frame[42] = 3;
    frame[43] = 1;
    frame[44] = target.channel;
    return frame;
}

class Visit final : public VisitBackend {
public:
    explicit Visit(const char* directory) : directory_(directory) {}

    bool start(const Target& target) override {
        assert(!receiving_);
        visit_.reset();
        result_ = Outcome::none;
        elapsed_ = 0;
        receiving_ = visit_.start(&target, 1) == session::StartResult::started;
        return receiving_;
    }

    bool observationDone() const override {
        return visit_.hasSample() || elapsed_ >= PassiveSession::windowUs;
    }

    bool stopReception() override {
        assert(receiving_);
        receiving_ = false;
        return true;
    }

    void beginSave(const CaptureId& id) override {
        assert(!receiving_);
        const auto file = path(id);
        switch (visit_.finish(file.c_str())) {
        case session::VisitState::saved:
            result_ = Outcome::saved;
            break;
        case session::VisitState::not_observed:
            result_ = Outcome::not_observed;
            break;
        default:
            result_ = Outcome::storage_error;
        }
    }

    Outcome saveResult() const override { return result_; }

    void receive(const Target& target, uint64_t elapsed = 1) {
        assert(receiving_);
        const auto frame = beacon(target);
        visit_.receive(frame.data(), frame.size(), target.channel, elapsed);
    }

    void expire() { elapsed_ = PassiveSession::windowUs; }
    uint8_t channel() const { return visit_.channel(); }
    bool hasSample() const { return visit_.hasSample(); }

    std::string path(const CaptureId& id) const {
        std::string buffer(directory_.size() + 56, '\0');
        if (!capturePath(&buffer[0], buffer.size(), directory_.c_str(), id)) {
            std::abort();
        }
        buffer.resize(strlen(buffer.c_str()));
        return buffer;
    }

private:
    std::string directory_;
    PassiveSession visit_;
    uint64_t elapsed_ = 0;
    bool receiving_ = false;
    Outcome result_ = Outcome::none;
};

void checkFile(const std::string& file, const Target& target) {
    assert(storage::recoverPcap(file.c_str()) == storage::CaptureState::saved);
    std::ifstream input(file, std::ios::binary);
    input.seekg(40);
    const auto expected = beacon(target);
    decltype(beacon(target)) actual{};
    input.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    assert(input && actual == expected);
    assert(input.peek() == std::char_traits<char>::eof());
}

void checkActive(const PassiveRound& round, const Visit& visit, size_t index) {
    assert(round.state() == State::running);
    assert(round.status().next() == index);
    assert(visit.channel() == round.status().profile[index].channel);
}

void checkRound(const char* directory, uint32_t sessionId,
                const std::array<uint8_t, 2>& laterChannels, bool available) {
    const Target second{{2, 0, 0, 0, 0, 2}, laterChannels[0]};
    const Target third{{2, 0, 0, 0, 0, 3}, laterChannels[1]};
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(second) && profile.add(third);
    assert(valid);
    Visit visit(directory);
    PassiveRound round(visit);
    const bool started = round.start({boot, sessionId}, profile);
    assert(started);
    checkActive(round, visit, 0);

    visit.receive(second);
    assert(!visit.hasSample());
    visit.receive(first);
    round.poll();
    checkActive(round, visit, 1);
    assert(round.status().results[0] == Outcome::saved);

    visit.receive(first);
    visit.receive(moved);
    visit.receive(third);
    assert(!visit.hasSample());
    round.poll();
    assert(round.status().next() == 1);
    if (available) {
        visit.receive(second);
    } else {
        visit.receive(second, PassiveSession::windowUs);
        assert(!visit.hasSample());
        visit.expire();
    }
    round.poll();
    checkActive(round, visit, 2);
    assert(round.status().results[1] == (available ? Outcome::saved : Outcome::not_observed));

    visit.receive(first);
    assert(!visit.hasSample());
    visit.receive(third);
    round.poll();
    round.poll();
    assert(round.state() == State::complete && round.outcome() == Outcome::saved);
    assert(round.status().next() == 3 && round.status().results[2] == Outcome::saved);

    const VisitId id{boot, sessionId};
    checkFile(visit.path({id, 1}), first);
    checkFile(visit.path({id, 3}), third);
    if (available) {
        checkFile(visit.path({id, 2}), second);
    } else {
        const auto missing = visit.path({id, 2});
        assert(storage::recoverPcap(missing.c_str()) == storage::CaptureState::missing);
    }
}

void checkQuietRound(const char* directory) {
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(quietSecond);
    assert(valid);
    Visit visit(directory);
    PassiveRound round(visit);
    const VisitId id{boot, 4};
    const bool started = round.start(id, profile);
    assert(started);
    visit.expire();
    round.poll();
    assert(round.state() == State::running);
    visit.expire();
    round.poll();
    assert(round.state() == State::complete && round.outcome() == Outcome::not_observed);
    assert(round.status().results[0] == Outcome::not_observed);
    assert(round.status().results[1] == Outcome::not_observed);
    for (unsigned slot : {1U, 2U}) {
        const auto file = visit.path({id, slot});
        assert(storage::recoverPcap(file.c_str()) == storage::CaptureState::missing);
    }
}

} // namespace

int main(int argc, char** argv) try {
    assert(argc == 2);
    checkRound(argv[1], 1, {6, 6}, true);
    checkRound(argv[1], 2, {1, 11}, true);
    checkRound(argv[1], 3, {6, 11}, false);
    checkQuietRound(argv[1]);
    std::cout << "Passive round: shared channel, channel change, and quiet visits passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
