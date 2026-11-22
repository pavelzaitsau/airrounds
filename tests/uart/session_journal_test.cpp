// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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
        result = Outcome::none;
        return budget.shareAt(nowUs()) == 0 ? VisitStart::deadline : VisitStart::listening;
    }
    bool observationDone() const override { return false; }
    bool stopReception() override { return true; }
    // Like the board, a visit without a beacon has nothing to save.
    void beginSave(const CaptureId&) override {
        result = beaconHeard ? Outcome::saved : Outcome::not_observed;
    }
    Outcome saveResult() const override { return result; }
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
    bool beaconHeard = false;
    Outcome result = Outcome::none;
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

class TrailingByteFile : public airrounds::storage::SdFile {
public:
    // Static template dispatch adds one byte the journal text does not contain.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    size_t write(const uint8_t* bytes, size_t size) {
        const uint8_t extra = '\n';
        const size_t written = SdFile::write(bytes, size);
        return written == size && SdFile::write(&extra, 1) == 1 ? size : 0;
    }
};

class ShortWriteFile : public airrounds::storage::SdFile {
public:
    // Static template dispatch drops the last byte but reports a full write.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    size_t write(const uint8_t* bytes, size_t size) {
        return SdFile::write(bytes, size - 1) == size - 1 ? size : 0;
    }
    // A verifier that keeps reading at the end of the file would never finish.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    ssize_t read(uint8_t* bytes, size_t size) {
        ++reads_;
        assert(reads_ < 1000 && "the verifier must stop at the end of a short file");
        return SdFile::read(bytes, size);
    }

private:
    unsigned reads_ = 0;
};

// Each fault file fails one step that the journal write must check.
class SyncFailFile : public airrounds::storage::SdFile {
public:
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    size_t write(const uint8_t* bytes, size_t size) {
        written_ += size;
        return SdFile::write(bytes, size);
    }
    // The card fails to flush once it holds unsynced bytes.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool sync() { return written_ == 0 && SdFile::sync(); }

private:
    size_t written_ = 0;
};

class WriteCloseFailFile : public airrounds::storage::SdFile {
public:
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool openNew(const char* path) {
        writing_ = true;
        return SdFile::openNew(path);
    }
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool close() { return SdFile::close() && !writing_; }

private:
    bool writing_ = false;
};

class ReadCloseFailFile : public airrounds::storage::SdFile {
public:
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool openRead(const char* path) {
        reading_ = true;
        return SdFile::openRead(path);
    }
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool close() { return SdFile::close() && !reading_; }

private:
    bool reading_ = false;
};

class OverReportingReadFile : public airrounds::storage::SdFile {
public:
    // Static template dispatch reports one byte more than read() delivered or was asked for.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    ssize_t read(uint8_t* bytes, size_t size) {
        const auto count = SdFile::read(bytes, size);
        return count > 0 ? count + 1 : count;
    }
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
    visit.beaconHeard = true;
    visit.clockUs = 250;
    round.stop();
    assert(round.state() == State::finalizing);
    assert(round.outcome() == Outcome::none);
    assert(round.status().next() == 0);
    const bool concurrentStart = round.start({id.boot, 2}, profile);
    assert(!concurrentStart);
    assert(visit.journalCalls == 1);
    assert(visit.journal.state == State::stopped);
    assert(visit.journal.outcome == Outcome::saved);
    assert(visit.journal.status.results[0] == Outcome::saved);
    assert(visit.journal.status.results[1] == Outcome::none);
    assert(visit.journal.budget.startedUs == 100);
    assert(visit.journal.endedUs == 250);
    round.stop();
    round.poll();
    assert(visit.journalCalls == 1);
    visit.completeJournalWrite();
    round.poll();
    assert(round.state() == State::stopped);
    assert(round.outcome() == Outcome::saved);
    assert(round.status().next() == 1);
    return visit.journal;
}

void testJournalFile(const char* directory, const SessionReport& report) {
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    stats[0] = {100, 140, 12, 3, 2, 86, 85};
    const std::string final = std::string(directory) + "/session-0000000000001234-00000001.log";
    const bool written =
        writeSessionJournal(directory, report, stats, {"build-test", "revision-test"});
    assert(written);
    const auto log = contents(final);
    assert(log ==
           std::string("format=airrounds-session-v3\nfirmware=") + airrounds::firmwareVersion +
               " build=build-test revision=revision-test protocol=01 radio=passive mode=beacon\n"
               "boot=0000000000001234 session=00000001 state=stopped outcome=saved points=2\n"
               "started_us=100 ended_us=250 budget_us=20000000\n"
               "point=1 bssid=020000000001 channel=6 outcome=saved listen_started_us=100 "
               "listen_ended_us=140 observed_frames=12 accepted_frames=3 "
               "filtered_frames=9 queue_overflows=2 bytes_written=86 verified_file_size=85 "
               "pcap=visit-0000000000001234-00000001-1.pcap\n"
               "point=2 bssid=020000000002 channel=11 outcome=none listen_started_us=0 "
               "listen_ended_us=0 observed_frames=0 accepted_frames=0 filtered_frames=0 "
               "queue_overflows=0 bytes_written=0 verified_file_size=0 pcap=-\n");
    assert(!std::ifstream(final + ".part"));
    const bool overwritten =
        writeSessionJournal(directory, report, stats, {"another-build", "revision-test"});
    assert(!overwritten);
    assert(contents(final) == log);
    // The board refuses before it stages anything.
    assert(!std::ifstream(final + ".part"));
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
    assert(!retried && "the leftover .part file from the failed write blocks the retry");
    assert(!std::ifstream(staged));
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
    assert(started);
    assert(visit.starts == 0);
    assert(round.state() == State::running);
    round.poll();
    assert(visit.starts == 0);

    const auto filename = std::string(directory) + "/session-0000000000001234-00000004.log";
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    const JournalBuild build{"build-test", "revision-test"};
    const bool opened =
        writeSessionJournal(directory, visit.opened, stats, build, JournalFile::open);
    assert(opened);
    assert(contents(filename + ".open").find("state=running outcome=none") != std::string::npos);
    const auto pendingSnapshot = contents(filename + ".open");
    round.stop();
    assert(visit.starts == 0);
    visit.completeJournalWrite();
    round.poll();
    assert(visit.starts == 0);
    assert(visit.journal.state == State::stopped);
    assert(visit.journal.status.results[0] == Outcome::not_observed);
    const bool interrupted =
        writeSessionJournal<InterruptedFile>(directory, visit.journal, stats, build);
    assert(!interrupted);
    assert(contents(filename + ".open") == pendingSnapshot);
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
    assert(visit.starts == 0);
    assert(visit.journal.state == State::error);
    assert(visit.journal.status.results[0] == Outcome::storage_error);
    visit.completeJournalWrite();
    round.poll();
    assert(round.state() == State::error);
    assert(round.outcome() == Outcome::storage_error);
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
    assert(opened);
    assert(std::ifstream(filename + ".open"));
    const auto checkpoint = contents(filename + ".open");
    const bool reopened = writeSessionJournal(directory, final, stats, build, JournalFile::open);
    assert(!reopened);
    assert(contents(filename + ".open") == checkpoint);
    assert(!std::ifstream(filename + ".open.part"));
    const bool closed = writeSessionJournal(directory, final, stats, build);
    assert(closed);
    assert(std::ifstream(filename));
    assert(!std::ifstream(filename + ".open"));
    assert(contents(filename).find("state=stopped outcome=saved") != std::string::npos);
    // A published final journal also blocks a late checkpoint.
    const bool lateCheckpoint =
        writeSessionJournal(directory, start, stats, build, JournalFile::open);
    assert(!lateCheckpoint);
    assert(!std::ifstream(filename + ".open"));
    assert(!std::ifstream(filename + ".open.part"));
}

void testSinglePointJournal(const char* directory) {
    SessionReport report;
    report.id = {0x1234, 8};
    report.state = State::complete;
    report.outcome = Outcome::saved;
    const bool added = report.status.profile.add({{2, 0, 0, 0, 0, 9}, 1});
    assert(added);
    report.status.results[0] = Outcome::saved;
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    const JournalBuild build{"build-test", "revision-test"};
    const bool written = writeSessionJournal(directory, report, stats, build);
    assert(written);
    const auto filename = std::string(directory) + "/session-0000000000001234-00000008.log";
    // One point keeps the single-visit capture name, without a slot suffix.
    assert(contents(filename).find(" pcap=visit-0000000000001234-00000008.pcap\n") !=
           std::string::npos);

    report.id.session = 9;
    report.outcome = Outcome::not_observed;
    report.status.results[0] = Outcome::not_observed;
    const bool quiet = writeSessionJournal(directory, report, stats, build);
    assert(quiet);
    const auto quietLog = std::string(directory) + "/session-0000000000001234-00000009.log";
    assert(contents(quietLog).find(" outcome=not_observed listen_started_us=0 ") !=
           std::string::npos);
    assert(contents(quietLog).find(" pcap=-\n") != std::string::npos);
}

template <typename File>
void checkFaultBlocksPublish(const char* directory, SessionReport report, uint32_t sessionId) {
    report.id.session = sessionId;
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    char filename[512];
    const bool named = sessionJournalPath(filename, sizeof(filename), directory, report.id);
    assert(named);
    const bool published =
        writeSessionJournal<File>(directory, report, stats, {"build-test", "revision-test"});
    assert(!published);
    assert(!std::ifstream(filename));
    assert(std::ifstream(std::string(filename) + ".part"));
}

void testFaultsBlockPublish(const char* directory, const SessionReport& report) {
    checkFaultBlocksPublish<TrailingByteFile>(directory, report, 10);
    checkFaultBlocksPublish<ShortWriteFile>(directory, report, 11);
    checkFaultBlocksPublish<SyncFailFile>(directory, report, 12);
    checkFaultBlocksPublish<WriteCloseFailFile>(directory, report, 13);
    checkFaultBlocksPublish<ReadCloseFailFile>(directory, report, 14);
}

void testPathsFitTheirBuffers() {
    char path[48];
    const VisitId id{0x1234, 1};
    // The journal name has 38 characters and needs one more byte for its terminator.
    bool fits = sessionJournalPath(path, 39, "", id);
    assert(fits);
    assert(std::string(path) == "/session-0000000000001234-00000001.log");
    fits = sessionJournalPath(path, 38, "", id);
    assert(!fits);
    fits = capturePath(path, 38, "", {id, 0});
    assert(fits);
    assert(std::string(path) == "/visit-0000000000001234-00000001.pcap");
    fits = capturePath(path, 37, "", {id, 0});
    assert(!fits);
    fits = capturePath(path, 40, "", {id, 5});
    assert(fits);
    assert(std::string(path) == "/visit-0000000000001234-00000001-5.pcap");
    fits = capturePath(path, 39, "", {id, 5});
    assert(!fits);
}

void testJournalCapacity(const char* directory) {
    SessionReport report;
    report.id = {0x1234, 15};
    report.state = State::stopped;
    report.outcome = Outcome::not_observed;
    const VisitStatistics stats[airrounds::session::Profile::capacity]{};
    char text[4096];
    // Measure the text with an empty build ID, then pad the ID to reach the 2304-byte buffer.
    const auto headerOnly =
        detail::formatSessionJournal(text, sizeof(text), report, stats, {"", ""});
    assert(headerOnly > 0);
    assert(headerOnly < 2303);
    const std::string headerFits(2303 - headerOnly, 'h');
    bool written = writeSessionJournal(directory, report, stats, {headerFits.c_str(), ""});
    const auto first = std::string(directory) + "/session-0000000000001234-0000000f.log";
    assert(written);
    assert(contents(first).size() == 2303);
    report.id.session = 16;
    const std::string headerFull(2304 - headerOnly, 'h');
    written = writeSessionJournal(directory, report, stats, {headerFull.c_str(), ""});
    const auto second = std::string(directory) + "/session-0000000000001234-00000010.log";
    assert(!written);
    assert(!std::ifstream(second));
    assert(!std::ifstream(second + ".part"));

    const bool added = report.status.profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);
    report.id.session = 17;
    const auto onePoint = detail::formatSessionJournal(text, sizeof(text), report, stats, {"", ""});
    assert(onePoint > headerOnly);
    assert(onePoint < 2303);
    const std::string lineFits(2303 - onePoint, 'l');
    written = writeSessionJournal(directory, report, stats, {lineFits.c_str(), ""});
    const auto third = std::string(directory) + "/session-0000000000001234-00000011.log";
    assert(written);
    assert(contents(third).size() == 2303);
    assert(contents(third).back() == '\n');
    report.id.session = 18;
    const std::string lineFull(2304 - onePoint, 'l');
    written = writeSessionJournal(directory, report, stats, {lineFull.c_str(), ""});
    const auto fourth = std::string(directory) + "/session-0000000000001234-00000012.log";
    assert(!written);
    assert(!std::ifstream(fourth));
    assert(!std::ifstream(fourth + ".part"));
    const std::string overlong(3000, 'o');
    written = writeSessionJournal(directory, report, stats, {overlong.c_str(), ""});
    assert(!written);
    assert(!std::ifstream(fourth));
    assert(!std::ifstream(fourth + ".part"));
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

void testOverReportedReadFailsVerification(const char* directory, const SessionReport& report) {
    checkFaultBlocksPublish<OverReportingReadFile>(directory, report, 19);
}

// An EAPOL (data) round records mode=eapol and the renamed counters, and filtered_frames does not
// underflow: observedFrames counts the data frames, so acceptedFrames is at most observedFrames.
void testEapolJournal(const char* directory) {
    SessionReport report;
    report.id = {0x1234, 20};
    report.state = State::complete;
    report.outcome = Outcome::saved;
    report.status.mode = eapolModeId;
    const bool added = report.status.profile.add({{2, 0, 0, 0, 0, 1}, 6});
    assert(added);
    report.status.results[0] = Outcome::saved;
    VisitStatistics stats[airrounds::session::Profile::capacity]{};
    stats[0] = {100, 57000000, 5, 1, 0, 300, 299}; // 5 observed, 1 accepted, 4 filtered.
    const bool written =
        writeSessionJournal(directory, report, stats, {"build-test", "revision-test"});
    assert(written);
    const auto body = contents(std::string(directory) + "/session-0000000000001234-00000014.log");
    assert(body.find("format=airrounds-session-v3\n") != std::string::npos);
    assert(body.find(" radio=passive mode=eapol\n") != std::string::npos);
    assert(body.find(" observed_frames=5 accepted_frames=1 filtered_frames=4 ") != std::string::npos);
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

// NOLINTNEXTLINE(bugprone-exception-escape): An allocation failure must fail this host test.
int main(int argc, char** argv) {
    if (const char* problem = directoryProblem(argc, argv)) {
        std::cerr << "session_journal_test: " << problem << "; pass an empty temporary directory\n";
        return EXIT_FAILURE;
    }
    alarm(30); // A loop that never ends fails the test instead of hanging it.
    const auto report = testRound();
    testJournalFile(argv[1], report);
    testInterruptedJournal(argv[1], report);
    testCorruptJournal(argv[1], report);
    testPreparationAndRecovery(argv[1]);
    testPreparationDeadline();
    testClosingAnOpenJournal(argv[1], report);
    testFullFivePointJournal(argv[1]);
    testSinglePointJournal(argv[1]);
    testFaultsBlockPublish(argv[1], report);
    testPathsFitTheirBuffers();
    testJournalCapacity(argv[1]);
    testOverReportedReadFailsVerification(argv[1], report);
    testEapolJournal(argv[1]);
    std::cout << "Session journal: format, checkpoint, faults, and capacity passed\n";
}
