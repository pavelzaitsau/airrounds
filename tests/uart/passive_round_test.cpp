#include <array>
// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../firmware/esp32/uart/session_journal.h"
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
    explicit Visit(const char* directory, bool checkpoint = false)
        : directory_(directory), checkpoint_(checkpoint) {}

    void beginRound(const SessionReport& report) override {
        if (checkpoint_) {
            const VisitStatistics stats[session::Profile::capacity]{};
            const bool written = writeSessionJournal(directory_.c_str(), report, stats,
                                                     {"fixture", "unknown"}, JournalFile::open);
            assert(written);
        }
    }

    uint64_t nowUs() const override { return now_; }

    VisitStart start(const Target& target, const VisitBudget& budget) override {
        assert(!receiving_);
        ++setupCount;
        lastRoundDurationUs = budget.round.durationUs;
        lastShareUs = budget.shareAt(now_ + setupDelayUs);
        now_ += setupDelayUs;
        setupDelayUs = 0;
        const auto shareUs = budget.shareAt(now_);
        if (shareUs == 0) {
            return VisitStart::deadline;
        }
        visit_.reset();
        result_ = Outcome::none;
        startedUs_ = now_;
        const auto started = visit_.startAdaptive(target, shareUs);
        assert(started == session::StartResult::started);
        receiving_ = true;
        return VisitStart::listening;
    }

    bool observationDone() const override { return visit_.observationDone(now_ - startedUs_); }

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

    Outcome saveResult() const override { return holdSave ? Outcome::none : result_; }

    void receive(const Target& target) {
        assert(receiving_);
        const auto frame = beacon(target);
        visit_.receive(frame.data(), frame.size(), target.channel, now_ - startedUs_);
    }

    void selectMode(uint8_t modeId) override {
        ++selectCount;
        lastModeId = modeId;
    }

    void seedTransmitBudget(uint32_t frames) override {
        ++seedCount;
        lastTransmitCap = frames;
    }

    void pollMode() override { ++pollModeCount; }

    void advance(uint64_t us) { now_ += us; }
    void expire() { now_ = startedUs_ + PassiveSession::windowUs; }
    bool holdSave = false;
    uint64_t setupDelayUs = 0;
    unsigned setupCount = 0;
    uint64_t lastRoundDurationUs = 0;
    uint64_t lastShareUs = 0;
    uint8_t lastModeId = beaconModeId;
    unsigned selectCount = 0;
    uint32_t lastTransmitCap = 0;
    unsigned seedCount = 0;
    unsigned pollModeCount = 0;
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
    bool checkpoint_;
    PassiveSession visit_;
    uint64_t now_ = 0;
    uint64_t startedUs_ = 0;
    bool receiving_ = false;
    Outcome result_ = Outcome::none;
};

void checkVisitShares() {
    const RoundBudget round{1000, 10};
    assert(round.remainingAt(1000) == 10);
    assert(round.remainingAt(1003) == 7);
    assert(round.remainingAt(1010) == 0);
    assert(round.remainingAt(1020) == 0);
    // Each share rounds up, so any time left gives the current point a positive share.
    assert((VisitBudget{round, 3}.shareAt(1000) == 4));
    assert((VisitBudget{round, 3}.shareAt(1001) == 3));
    assert((VisitBudget{round, 2}.shareAt(1009) == 1));
    assert((VisitBudget{round, 1}.shareAt(1009) == 1));
    assert((VisitBudget{round, 1}.shareAt(1010) == 0));
}

void checkFile(const std::string& file, const Target& target) {
    assert(storage::recoverPcap(file.c_str()) == storage::CaptureState::saved);
    std::ifstream input(file, std::ios::binary);
    input.seekg(40);
    const auto expected = beacon(target);
    decltype(beacon(target)) actual{};
    input.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    assert(input);
    assert(actual == expected);
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
    visit.advance(4999999);
    visit.receive(first);
    visit.advance(1);
    round.poll();
    checkActive(round, visit, 0);
    visit.advance(999998);
    visit.receive(first);
    visit.advance(1);
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
    }
    visit.expire();
    if (!available) {
        visit.receive(second);
        assert(!visit.hasSample());
    }
    round.poll();
    checkActive(round, visit, 2);
    assert(round.status().results[1] == (available ? Outcome::saved : Outcome::not_observed));

    visit.receive(first);
    assert(!visit.hasSample());
    visit.receive(third);
    visit.expire();
    round.poll();
    assert(round.state() == State::complete);
    assert(round.outcome() == Outcome::saved);
    assert(round.status().next() == 3);
    assert(round.status().results[2] == Outcome::saved);

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
    assert(round.state() == State::complete);
    assert(round.outcome() == Outcome::not_observed);
    assert(round.status().results[0] == Outcome::not_observed);
    assert(round.status().results[1] == Outcome::not_observed);
    for (unsigned slot : {1U, 2U}) {
        const auto file = visit.path({id, slot});
        assert(storage::recoverPcap(file.c_str()) == storage::CaptureState::missing);
    }
}

void checkDeadlineStopsTheRound(PassiveRound& round, const Visit& visit,
                                const session::Profile& profile) {
    assert(round.state() == State::stopped);
    assert(round.outcome() == Outcome::saved);
    assert(visit.setupCount == 1);
    assert(round.status().results[1] == Outcome::none);
    assert(round.status().results[2] == Outcome::none);
    const bool restarted = round.start({boot, 7}, profile);
    assert(restarted);
    checkActive(round, visit, 0);
}

void checkLaterPointsShareTheRest(PassiveRound& round, Visit& visit, const VisitId& id,
                                  const Target& third) {
    // Two points share the six seconds left after the save and radio setup.
    checkActive(round, visit, 1);
    visit.advance(2999999);
    round.poll();
    checkActive(round, visit, 1);
    visit.receive(quietSecond);
    visit.advance(1);
    round.poll();
    checkActive(round, visit, 2);
    assert(round.status().results[1] == Outcome::saved);
    visit.advance(3000000);
    visit.receive(third);
    assert(!visit.hasSample());
    round.poll();
    assert(round.state() == State::complete);
    assert(round.outcome() == Outcome::saved);
    assert(round.status().results[2] == Outcome::not_observed);
    checkFile(visit.path({id, 2}), quietSecond);
}

void checkRoundDeadline(const char* directory, bool canStartNextVisit) {
    const Target third{{2, 0, 0, 0, 0, 3}, 11};
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(quietSecond) && profile.add(third);
    assert(valid);
    Visit visit(directory);
    PassiveRound round(visit);
    const VisitId id{boot, canStartNextVisit ? 5U : 6U};
    const bool started = round.start(id, profile);
    assert(started);

    visit.receive(first);
    visit.holdSave = true;
    visit.expire();
    round.poll();
    assert(round.state() == State::finalizing);

    visit.advance(canStartNextVisit ? 17000000U : 25000000U);
    round.poll();
    assert(round.state() == State::finalizing);
    visit.holdSave = false;
    visit.setupDelayUs = 2000000;
    round.poll();
    assert(round.status().results[0] == Outcome::saved);
    checkFile(visit.path({id, 1}), first);

    if (canStartNextVisit) {
        checkLaterPointsShareTheRest(round, visit, id, third);
    } else {
        checkDeadlineStopsTheRound(round, visit, profile);
    }
}

void checkSetupDeadline(const char* directory) {
    const Target third{{2, 0, 0, 0, 0, 3}, 11};
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(quietSecond) && profile.add(third);
    assert(valid);
    Visit visit(directory);
    PassiveRound round(visit);
    const bool started = round.start({boot, 8}, profile);
    assert(started);
    visit.expire();
    round.poll();
    checkActive(round, visit, 1);

    visit.setupDelayUs = 20000000;
    visit.expire();
    round.poll();
    assert(round.state() == State::stopped);
    assert(round.outcome() == Outcome::not_observed);
    assert(round.status().results[0] == Outcome::not_observed);
    assert(round.status().results[1] == Outcome::not_observed);
    assert(round.status().results[2] == Outcome::none);

    visit.setupDelayUs = 30000000;
    const bool restarted = round.start({boot, 9}, profile);
    assert(restarted);
    assert(round.state() == State::error);
    assert(round.outcome() == Outcome::radio_error);
    assert(round.status().results[0] == Outcome::radio_error);
}

void checkPowerLossKeepsCheckpoint(const char* directory) {
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(quietSecond);
    assert(valid);
    const VisitId id{boot, 10};
    Visit visit(directory, true);
    {
        // The round object goes away mid-round, as it does when the board loses power.
        PassiveRound round(visit);
        const bool started = round.start(id, profile);
        assert(started);
        visit.receive(first);
        visit.expire();
        round.poll();
        checkActive(round, visit, 1);
        assert(round.status().results[0] == Outcome::saved);
    }
    checkFile(visit.path({id, 1}), first);
    const auto unobserved = visit.path({id, 2});
    assert(storage::recoverPcap(unobserved.c_str()) == storage::CaptureState::missing);
    char final[512];
    const bool hasPath = sessionJournalPath(final, sizeof(final), directory, id);
    assert(hasPath);
    const auto checkpoint = std::string(final) + ".open";
    std::ifstream input(checkpoint);
    assert(input);
    std::string line;
    std::getline(input, line);
    std::getline(input, line);
    std::getline(input, line);
    assert(line == "boot=123456789abcdef0 session=0000000a state=running outcome=none points=2");
    std::getline(input, line);
    std::getline(input, line);
    assert(line.find("point=1 bssid=020000000001 channel=6 outcome=none ") == 0);
    std::getline(input, line);
    assert(line.find("point=2 bssid=020000000002 channel=6 outcome=none ") == 0);
    assert(storage::SdFile::probe(final) == storage::FilePresence::missing);
}

void checkStopDuringSave(const char* directory) {
    const Target third{{2, 0, 0, 0, 0, 3}, 11};
    session::Profile profile;
    const bool valid = profile.add(first) && profile.add(quietSecond) && profile.add(third);
    assert(valid);
    Visit visit(directory);
    PassiveRound round(visit);
    const VisitId id{boot, 11};
    const bool started = round.start(id, profile);
    assert(started);

    visit.receive(first);
    visit.holdSave = true;
    visit.expire();
    round.poll();
    assert(round.state() == State::finalizing);
    // Stop during the first save keeps that file and skips the later points.
    round.stop();
    assert(round.state() == State::finalizing);
    visit.holdSave = false;
    round.poll();
    assert(round.state() == State::stopped);
    assert(round.outcome() == Outcome::saved);
    assert(round.status().results[0] == Outcome::saved);
    assert(round.status().results[1] == Outcome::none);
    assert(round.status().results[2] == Outcome::none);
    assert(round.status().interruption == VisitInterruption::no);
    assert(visit.setupCount == 1);
    checkFile(visit.path({id, 1}), first);
    const auto skipped = visit.path({id, 2});
    assert(storage::recoverPcap(skipped.c_str()) == storage::CaptureState::missing);
    round.poll();
    assert(round.state() == State::stopped);
    assert(visit.setupCount == 1);
}

// The test writes fixed file names, so it needs a fresh directory of its own.
const char* directoryProblem(int argc, char** argv) {
    if (argc != 2) {
        return "the directory argument is missing";
    }
    struct stat info;
    if (stat(argv[1], &info) != 0 || !S_ISDIR(info.st_mode)) {
        return "the argument is not a directory";
    }
    DIR* directory = opendir(argv[1]);
    if (directory == nullptr) {
        return "the directory cannot be read";
    }
    bool empty = true;
    while (const dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            empty = false;
        }
    }
    closedir(directory);
    return empty ? nullptr : "the directory is not empty";
}

} // namespace

void checkPerModeBudget(const char* directory) {
    session::Profile one;
    assert(one.add(first));
    {
        Visit visit(directory);
        PassiveRound round(visit);
        assert(round.start({boot, 30}, one)); // Default mode is Beacon.
        assert(visit.selectCount == 1 && visit.lastModeId == beaconModeId);
        assert(round.status().mode == beaconModeId);
        // A one-point Beacon round keeps the T10 10 s budget and share.
        assert(visit.lastRoundDurationUs == 10000000);
        assert(visit.lastShareUs == 10000000);
    }
    {
        Visit visit(directory);
        PassiveRound round(visit);
        assert(round.start({boot, 31}, one, eapolModeId));
        assert(visit.selectCount == 1 && visit.lastModeId == eapolModeId);
        assert(round.status().mode == eapolModeId);
        // A one-point EAPOL round gets 60 s.
        assert(visit.lastRoundDurationUs == 60000000);
        assert(visit.lastShareUs == 60000000);
    }
    session::Profile five;
    for (uint8_t index = 1; index <= 5; ++index) {
        assert(five.add({{2, 0, 0, 0, 0, index}, 6}));
    }
    Visit visit(directory);
    PassiveRound round(visit);
    assert(round.start({boot, 32}, five, eapolModeId));
    // Five points share 300 s; the fair share gives the first point 60 s.
    assert(visit.lastRoundDurationUs == 300000000);
    assert(visit.lastShareUs == 60000000);
    // A start after an EAPOL round returns the visit to Beacon.
    round.stop();
    round.poll();
    assert(round.start({boot, 33}, one));
    assert(visit.lastModeId == beaconModeId && visit.lastRoundDurationUs == 10000000);
}

void checkTransmitBudgetAndPoll(const char* directory) {
    session::Profile one;
    assert(one.add(first));
    // A Beacon round seeds a zero transmit budget and still gets poll() time while running.
    {
        Visit visit(directory);
        PassiveRound round(visit);
        assert(round.start({boot, 40}, one));
        assert(visit.seedCount == 1 && visit.lastTransmitCap == 0);
        round.poll();
        assert(round.state() == State::running && visit.pollModeCount >= 1);
    }
    // The active mode seeds 30 frames (5 points, 2 bursts of 3) and gets the 60 s budget.
    {
        Visit visit(directory);
        PassiveRound round(visit);
        assert(round.start({boot, 41}, one, activeModeId));
        assert(visit.lastModeId == activeModeId && visit.lastTransmitCap == 30);
        assert(round.status().mode == activeModeId && visit.lastRoundDurationUs == 60000000);
    }
}

int main(int argc, char** argv) try {
    if (const char* problem = directoryProblem(argc, argv)) {
        std::cerr << "passive_round_test: " << problem << "; pass an empty temporary directory\n";
        return EXIT_FAILURE;
    }
    alarm(30); // A loop that never ends fails the test instead of hanging it.
    checkVisitShares();
    checkPerModeBudget(argv[1]);
    checkTransmitBudgetAndPoll(argv[1]);
    checkRound(argv[1], 1, {6, 6}, true);
    checkRound(argv[1], 2, {1, 11}, true);
    checkRound(argv[1], 3, {6, 11}, false);
    checkQuietRound(argv[1]);
    checkRoundDeadline(argv[1], true);
    checkRoundDeadline(argv[1], false);
    checkSetupDeadline(argv[1]);
    checkPowerLossKeepsCheckpoint(argv[1]);
    checkStopDuringSave(argv[1]);
    std::cout << "Passive round: visit order, extensions, and round deadline passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
