#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

#include "session_journal.h"

namespace {

using namespace airrounds::uart;

class Visit final : public VisitBackend {
public:
    uint64_t nowUs() const override { return clockUs; }
    void beginRound(const SessionReport& report) override {
        opened = report;
        pending = holdPreparation;
    }
    VisitStart start(const airrounds::session::Target&, const VisitBudget& budget) override {
        ++starts;
        return budget.shareAt(nowUs()) == 0 ? VisitStart::deadline : VisitStart::listening;
    }
    bool observationDone() const override { return false; }
    bool stopReception() override { return true; }
    void beginSave(const CaptureId&) override {}
    Outcome saveResult() const override { return Outcome::saved; }
    void beginJournal(const SessionReport& report) override {
        ++journalCalls;
        journal = report;
        pending = true;
    }
    bool journalPending() const override { return pending; }
    void completeJournalWrite() {
        assert(pending);
        pending = false;
    }

    SessionReport opened;
    SessionReport journal;
    uint64_t clockUs = 100;
    unsigned starts = 0;
    unsigned journalCalls = 0;
    bool pending = false;
    bool holdPreparation = false;
};

class InterruptedFile : public airrounds::storage::SdFile {
public:
    // Template dispatch selects this write function for the fault-injection file.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    size_t write(const uint8_t* bytes, size_t size) {
        if (wrote_) {
            return 0;
        }
        wrote_ = true;
        return SdFile::write(bytes, size / 2);
    }

private:
    bool wrote_ = false;
};

class CorruptReadFile : public airrounds::storage::SdFile {
public:
    // Static template dispatch supplies a read-back fault on the staged file.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    ssize_t read(uint8_t* bytes, size_t size) {
        const auto count = SdFile::read(bytes, size);
        if (count > 0) {
            bytes[0] ^= 1U;
        }
        return count;
    }
};

std::string contents(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

SessionReport testRound() {
    Visit visit;
    PassiveRound round(visit);
    airrounds::session::Profile profile;
    const bool profileAdded =
        profile.add({{2, 0, 0, 0, 0, 1}, 6}) && profile.add({{2, 0, 0, 0, 0, 2}, 11});
    assert(profileAdded);
    const VisitId id{0x1234, 1};
    const bool started = round.start(id, profile);
    assert(started);
    round.stop();
    assert(round.state() == State::finalizing && round.outcome() == Outcome::none);
    assert(round.status().next() == 0);
    const bool concurrentStart = round.start({id.boot, 2}, profile);
    assert(!concurrentStart);
    assert(visit.journalCalls == 1);
    assert(visit.journal.state == State::stopped && visit.journal.outcome == Outcome::saved);
    assert(visit.journal.status.results[0] == Outcome::saved);
    assert(visit.journal.status.results[1] == Outcome::none);
    round.stop();
    round.poll();
    assert(visit.journalCalls == 1);
    visit.completeJournalWrite();
    round.poll();
    assert(round.state() == State::stopped && round.outcome() == Outcome::saved);
    assert(round.status().next() == 1);
    return visit.journal;
}

void testJournalFile(const char* directory, const SessionReport& report) {
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    stats[0] = {100, 140, 12, 3, 0, 85, 85};
    const std::string final = std::string(directory) + "/session-0000000000001234-00000001.log";
    const bool written =
        writeSessionJournal(directory, report, stats, {"build-test", "revision-test"});
    assert(written);
    const auto log = contents(final);
    assert(log.find("format=airrounds-session-v1") != std::string::npos);
    assert(log.find("build=build-test revision=revision-test") != std::string::npos);
    assert(log.find("state=stopped outcome=saved") != std::string::npos);
    assert(log.find("point=1 bssid=020000000001 channel=6 outcome=saved") != std::string::npos);
    assert(log.find("management_frames=12 accepted_beacons=3 filtered_frames=9") !=
           std::string::npos);
    assert(log.find("bytes_written=85 verified_file_size=85") != std::string::npos);
    assert(log.find("point=2 bssid=020000000002 channel=11 outcome=none") != std::string::npos);
    assert(!std::ifstream(final + ".part"));
    const bool overwritten =
        writeSessionJournal(directory, report, stats, {"another-build", "revision-test"});
    assert(!overwritten);
    assert(contents(final) == log);
}

void testInterruptedJournal(const char* directory, SessionReport interrupted) {
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    interrupted.id.session = 2;
    const std::string staged = std::string(directory) + "/session-0000000000001234-00000002.log";
    const bool published = writeSessionJournal<InterruptedFile>(directory, interrupted, stats,
                                                                {"build-test", "revision-test"});
    assert(!published);
    assert(!std::ifstream(staged));
    assert(!contents(staged + ".part").empty());
    const bool retried =
        writeSessionJournal(directory, interrupted, stats, {"build-test", "revision-test"});
    assert(!retried);
}

void testCorruptJournal(const char* directory, SessionReport corrupt) {
    corrupt.id.session = 3;
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    const auto filename = std::string(directory) + "/session-0000000000001234-00000003.log";
    const bool published = writeSessionJournal<CorruptReadFile>(directory, corrupt, stats,
                                                                {"build-test", "revision-test"});
    assert(!published);
    assert(!std::ifstream(filename));
    assert(std::ifstream(filename + ".part"));
}

void testPreparationAndRecovery(const char* directory) {
    Visit visit;
    visit.holdPreparation = true;
    PassiveRound round(visit);
    airrounds::session::Profile profile;
    const bool added = profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);
    const VisitId id{0x1234, 4};
    const bool started = round.start(id, profile);
    assert(started && visit.starts == 0 && round.state() == State::running);
    round.poll();
    assert(visit.starts == 0);

    const auto filename = std::string(directory) + "/session-0000000000001234-00000004.log";
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    const JournalBuild build{"build-test", "revision-test"};
    const bool opened =
        writeSessionJournal(directory, visit.opened, stats, build, JournalFile::open);
    assert(opened &&
           contents(filename + ".open").find("state=running outcome=none") != std::string::npos);
    const auto pendingSnapshot = contents(filename + ".open");
    round.stop();
    assert(visit.starts == 0);
    visit.completeJournalWrite();
    round.poll();
    assert(visit.starts == 0 && visit.journal.state == State::stopped);
    assert(visit.journal.status.results[0] == Outcome::not_observed);
    const bool interrupted =
        writeSessionJournal<InterruptedFile>(directory, visit.journal, stats, build);
    assert(!interrupted && contents(filename + ".open") == pendingSnapshot);
}

void testPreparationDeadline() {
    Visit visit;
    visit.holdPreparation = true;
    PassiveRound round(visit);
    airrounds::session::Profile profile;
    const bool added = profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);
    const bool started = round.start({0x1234, 6}, profile);
    assert(started);
    visit.clockUs += visit.opened.budget.durationUs;
    visit.completeJournalWrite();
    round.poll();
    assert(round.state() == State::finalizing);
    assert(visit.starts == 0 && visit.journal.state == State::error);
    assert(visit.journal.status.results[0] == Outcome::storage_error);
    visit.completeJournalWrite();
    round.poll();
    assert(round.state() == State::error && round.outcome() == Outcome::storage_error);
}

void testClosingAnOpenJournal(const char* directory, SessionReport final) {
    final.id.session = 5;
    auto start = final;
    start.state = State::running;
    start.outcome = Outcome::none;
    start.status.results[0] = Outcome::none;
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    const JournalBuild build{"build-test", "revision-test"};
    const auto filename = std::string(directory) + "/session-0000000000001234-00000005.log";
    const bool opened = writeSessionJournal(directory, start, stats, build, JournalFile::open);
    assert(opened && std::ifstream(filename + ".open"));
    const bool closed = writeSessionJournal(directory, final, stats, build);
    assert(closed && std::ifstream(filename));
    assert(!std::ifstream(filename + ".open"));
}

void testFullFivePointJournal(const char* directory) {
    SessionReport report;
    report.id = {0x1234, 7};
    report.state = State::complete;
    report.outcome = Outcome::saved;
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    for (size_t index = 0; index < airrounds::session::Profile::capacity; ++index) {
        const airrounds::session::Target target{{2, 0, 0, 0, 0, static_cast<uint8_t>(index + 1)},
                                                13};
        const bool added = report.status.profile.add(target);
        assert(added);
        report.status.results[index] = Outcome::saved;
        stats[index] = {UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT32_MAX,
                        UINT32_MAX, UINT64_MAX, SIZE_MAX};
    }
    const JournalBuild build{"0123456789abcdef", "0123456789abcdef0123456789abcdef01234567"};
    const bool written = writeSessionJournal(directory, report, stats, build);
    const auto path = std::string(directory) + "/session-0000000000001234-00000007.log";
    assert(written);
    const auto body = contents(path);
    assert(body.find("points=5") != std::string::npos);
    assert(body.find("pcap=visit-0000000000001234-00000007-5.pcap\n") != std::string::npos);
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): An allocation failure must fail this host test.
int main(int argc, char** argv) {
    assert(argc == 2);
    const auto report = testRound();
    testJournalFile(argv[1], report);
    testInterruptedJournal(argv[1], report);
    testCorruptJournal(argv[1], report);
    testPreparationAndRecovery(argv[1]);
    testPreparationDeadline();
    testClosingAnOpenJournal(argv[1], report);
    testFullFivePointJournal(argv[1]);
}
