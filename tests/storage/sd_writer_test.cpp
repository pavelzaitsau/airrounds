#include "sd_file.h"
#include "write_queue.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace airrounds::storage;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool threadSanitizer = true;
#elif defined(__has_feature)
constexpr bool threadSanitizer = __has_feature(thread_sanitizer);
#else
constexpr bool threadSanitizer = false;
#endif

const uint8_t first[] = {1, 2, 3, 4};
const uint8_t second[] = {5, 6};
const uint8_t third[] = {7};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// The group that is running, named by the watchdog if the group does not finish in time.
char runningGroup[64] = "argument check";
size_t groupsPassed = 0;

} // namespace

// Plain C and async-signal-safe calls only.
extern "C" void reportStuckGroup(int /*signal*/) {
    static const char prefix[] = "watchdog: this test group did not finish in time: ";
    size_t length = 0;
    while (runningGroup[length] != '\0') {
        ++length;
    }
    write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write(STDERR_FILENO, runningGroup, length);
    write(STDERR_FILENO, "\n", 1);
    _exit(EXIT_FAILURE);
}

namespace {

// Runs one test group; the watchdog fails it, by name, after `seconds`.
template <typename Group> void runGroup(const char* name, Group group, unsigned seconds = 30) {
    snprintf(runningGroup, sizeof(runningGroup), "%s", name);
    alarm(seconds);
    group();
    alarm(0);
    ++groupsPassed;
}

// Each run needs new file names, so the binary refuses a missing, unusable, or used directory.
void requireEmptyDirectory(int argc, char** argv) {
    DIR* directory = argc == 2 ? opendir(argv[1]) : nullptr;
    bool empty = directory != nullptr;
    for (const dirent* entry = empty ? readdir(directory) : nullptr; entry != nullptr && empty;
         entry = readdir(directory)) {
        empty = strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
    }
    if (directory != nullptr) {
        closedir(directory);
    }
    require(empty, "pass an empty temporary directory");
}

struct TestFile {
    bool available = true;
    bool invalidCount = false;
    int error = 0;
    size_t writeLimit = sizeof(first);
    size_t calls = 0;
    std::vector<uint8_t> contents;
    std::function<void()> duringWrite;

    explicit operator bool() const { return available; }
    int lastError() const { return error; }

    size_t write(const uint8_t* data, size_t size) {
        ++calls;
        if (duringWrite) {
            duringWrite();
        }
        if (invalidCount) {
            return std::numeric_limits<size_t>::max();
        }
        const size_t count = std::min(size, writeLimit);
        contents.insert(contents.end(), data, data + count);
        return count;
    }
};

void testOverflowAndWrap() {
    const std::vector<uint8_t> all{1, 2, 3, 4, 5, 6, 7};
    TestFile file;
    WriteQueue<2, 4, TestFile> queue(file);
    uint8_t input[] = {1, 2, 3, 4};
    require(queue.enqueue(input, sizeof(input)) == EnqueueStatus::queued, "first block");
    input[0] = 0;
    require(queue.enqueue(second, sizeof(second)) == EnqueueStatus::queued, "second block");
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::full, "full queue");
    // This assertion deliberately checks a statically known invalid input.
    // cppcheck-suppress knownConditionTrueFalse
    require(queue.enqueue(nullptr, 1) == EnqueueStatus::invalid, "reject null block");
    // This assertion deliberately checks a statically known invalid input.
    // cppcheck-suppress knownConditionTrueFalse
    require(queue.enqueue(first, 0) == EnqueueStatus::invalid, "reject empty block");
    // This assertion deliberately checks a statically known invalid input.
    // cppcheck-suppress knownConditionTrueFalse
    require(queue.enqueue(first, sizeof(first) + 1) == EnqueueStatus::invalid,
            "reject oversized block before copying");
    require(queue.overflowCount() == 1, "count only valid blocks rejected for overflow");
    require(queue.drainOne() == WriteStatus::complete, "write first block");
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::queued, "reuse freed space");
    require(queue.drainOne() == WriteStatus::complete, "write second block");
    require(queue.drainOne() == WriteStatus::complete, "write third block");
    require(queue.drainOne() == WriteStatus::idle && file.calls == 3,
            "draining an empty queue must not write");
    require(file.contents == all && queue.overflowCount() == 1, "keep FIFO order across wrap");
}

void testPartialAndZeroWrite() {
    TestFile file;
    WriteQueue<2, 4, TestFile> queue(file);
    require(queue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue first");
    require(queue.enqueue(second, sizeof(second)) == EnqueueStatus::queued, "queue second");
    file.writeLimit = 2;
    require(queue.drainOne() == WriteStatus::partial, "report partial write");
    // drainOne consumed the previous limit through its templated File reference.
    // cppcheck-suppress redundantAssignment
    file.writeLimit = 0;
    require(queue.drainOne() == WriteStatus::no_progress, "report zero write");
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::full, "keep partial slot owned");
    require(file.contents == std::vector<uint8_t>({1, 2}), "record the accepted prefix once");
    file.writeLimit = sizeof(first);
    require(queue.drainOne() == WriteStatus::complete, "finish the remainder");
    require(queue.drainOne() == WriteStatus::complete, "write next block");
    require(file.contents == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}),
            "retry only the suffix before the next block");
}

void testHardErrorAndInvalidCount() {
    TestFile full;
    WriteQueue<1, 4, TestFile> fullQueue(full);
    require(fullQueue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue block");
    full.writeLimit = 0;
    full.error = ENOSPC;
    require(fullQueue.drainOne() == WriteStatus::io_error && full.contents.empty(),
            "surface a full-card error");
    full.writeLimit = sizeof(first);
    require(fullQueue.drainOne() == WriteStatus::io_error && full.calls == 1,
            "block retries after an error with an unknown offset");
    require(fullQueue.enqueue(second, sizeof(second)) == EnqueueStatus::full,
            "retain the pending block after hard failure");

    for (size_t writeLimit : {size_t{2}, sizeof(first)}) {
        TestFile damaged;
        WriteQueue<1, 4, TestFile> damagedQueue(damaged);
        require(damagedQueue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue block");
        damaged.writeLimit = writeLimit;
        damaged.error = EIO;
        require(damagedQueue.drainOne() == WriteStatus::io_error,
                "surface an I/O error even when FAT reports progress");
        require(damaged.contents == std::vector<uint8_t>(first, first + writeLimit),
                "keep the accepted prefix once");
        require(damagedQueue.drainOne() == WriteStatus::io_error && damaged.calls == 1,
                "never retry a failed write without checking the file");
    }

    TestFile broken;
    WriteQueue<1, 4, TestFile> brokenQueue(broken);
    require(brokenQueue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue block");
    broken.invalidCount = true;
    require(brokenQueue.drainOne() == WriteStatus::invalid, "reject an invalid write count");
    require(brokenQueue.drainOne() == WriteStatus::invalid && broken.calls == 1,
            "block retries after an invalid count");
}

void testProducerDuringWrite() {
    TestFile file;
    WriteQueue<2, 4, TestFile> queue(file);
    require(queue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue first");
    std::promise<void> writing;
    std::promise<void> resume;
    auto writingReady = writing.get_future();
    auto resumeReady = resume.get_future();
    file.duringWrite = [&] {
        writing.set_value();
        resumeReady.wait();
    };
    WriteStatus status = WriteStatus::invalid;
    std::thread consumer([&] { status = queue.drainOne(); });
    require(writingReady.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "consumer must reach the write");
    require(queue.enqueue(second, sizeof(second)) == EnqueueStatus::queued,
            "producer can use a free slot during a write");
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::full,
            "producer cannot overwrite a slot during a write");
    resume.set_value();
    consumer.join();
    // The consumer invoked this callback before join; stop calling it on later drains.
    // cppcheck-suppress redundantAssignment
    file.duringWrite = {};
    require(status == WriteStatus::complete, "first write completed");
    require(queue.drainOne() == WriteStatus::complete, "write block added during I/O");
    require(file.contents == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}) && queue.overflowCount() == 1,
            "keep data and count overflow while the consumer is busy");
}

void testOpenFailureAndExclusiveCreate(const std::string& directory) {
    const auto path = directory + "/capture.pcap";
    {
        SdFile file;
        WriteQueue<1, 4, SdFile> queue(file);
        require(queue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue block");
        const auto missing = directory + "/missing/capture.pcap";
        require(!file.openNew(missing.c_str()) && file.lastError() == ENOENT,
                "surface file creation failure");
        require(queue.drainOne() == WriteStatus::unavailable,
                "failed open keeps the block pending");
        require(file.openNew(path.c_str()) && file.lastError() == 0, "create a fresh file");
        require(!file.openNew(missing.c_str()) && file.lastError() == EBUSY,
                "keep the original file open");
        require(queue.drainOne() == WriteStatus::complete, "write the retained block");
    }
    SdFile collision;
    require(!collision.openNew(path.c_str()) && collision.lastError() == EEXIST,
            "refuse to overwrite an existing capture");
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    require(bytes == std::vector<uint8_t>(first, first + sizeof(first)),
            "opening a new file must not truncate an existing capture");
}

void testFileSizeLimit(const std::string& directory) {
    const auto path = directory + "/limited.pcap";
    SdFile file;
    WriteQueue<1, 4, SdFile> queue(file);
    require(file.openNew(path.c_str()), "create file for write failure test");
    require(queue.enqueue(first, sizeof(first)) == EnqueueStatus::queued, "queue first");

    struct rlimit original{};
    require(getrlimit(RLIMIT_FSIZE, &original) == 0, "read file size limit");
    const auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
    require(previousHandler != SIG_ERR, "ignore file-size signal for this test");
    const struct rlimit blocked{0, original.rlim_max};
    require(setrlimit(RLIMIT_FSIZE, &blocked) == 0, "set zero file size limit");
    const auto status = queue.drainOne();
    const auto error = file.lastError();
    require(setrlimit(RLIMIT_FSIZE, &original) == 0, "restore file size limit");
    std::signal(SIGXFSZ, previousHandler);

    require(status == WriteStatus::io_error && error == EFBIG, "surface error from write syscall");
    require(queue.drainOne() == WriteStatus::io_error, "stop after the failed syscall");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.tellg() == 0, "the queue must not retry even after the limit is lifted");
}

void testBytesAccepted() {
    TestFile file;
    WriteQueue<2, 4, TestFile> queue(file);
    require(queue.enqueue(first, sizeof(first)) == EnqueueStatus::queued &&
                queue.enqueue(second, sizeof(second)) == EnqueueStatus::queued,
            "queue two blocks");
    file.writeLimit = 3;
    require(queue.drainOne() == WriteStatus::partial && queue.bytesWritten() == 0 &&
                queue.bytesAccepted() == 3,
            "count the accepted prefix of the front block");
    require(queue.drainOne() == WriteStatus::complete && queue.bytesWritten() == 4 &&
                queue.bytesAccepted() == 4,
            "count a completed block once");
    // drainOne consumed the previous limit through its templated File reference.
    // cppcheck-suppress redundantAssignment
    file.writeLimit = 1;
    require(queue.drainOne() == WriteStatus::partial && queue.bytesWritten() == 4 &&
                queue.bytesAccepted() == 5,
            "add the prefix of the next block");
}

void testOverflowCountSaturates() {
    TestFile file;
    WriteQueue<1, 1, TestFile> queue(file);
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::queued, "fill the queue");
    for (uint64_t attempt = 0; attempt <= std::numeric_limits<uint32_t>::max(); ++attempt) {
        queue.enqueue(third, sizeof(third));
    }
    require(queue.overflowCount() == std::numeric_limits<uint32_t>::max(),
            "the overflow count stops at its maximum instead of wrapping");
}

void testFileNamesAndModes(const std::string& directory) {
    SdFile file;
    require(!file.openNew("") && file.lastError() == EINVAL && !file, "reject an empty file name");
    require(!file.openNew(nullptr) && file.lastError() == EINVAL && !file,
            "reject a missing file name");
    require(!file.openRead((directory + "/absent").c_str()) && file.lastError() == ENOENT,
            "surface a missing file on read");
    require(!file.openRead(directory.c_str()) && !file, "refuse to read a directory");

    const auto fifo = directory + "/fifo";
    require(mkfifo(fifo.c_str(), 0600) == 0, "create a FIFO");
    // A blocking open of a FIFO without a writer would hang here.
    const unsigned watchdog = alarm(5);
    const bool opened = file.openRead(fifo.c_str());
    alarm(watchdog);
    require(!opened && !file, "refuse to read a FIFO without blocking");

    const auto path = directory + "/mode.pcap";
    const mode_t mask = umask(0); // Check the requested mode, not the caller's umask.
    const bool created = file.openNew(path.c_str());
    umask(mask);
    require(created && static_cast<bool>(file), "create a file");
    struct stat info{};
    require(stat(path.c_str(), &info) == 0 &&
                (static_cast<unsigned>(info.st_mode) & 0777U) == 0600U,
            "create files readable only by the owner");
    errno = ENOENT;
    require(file.write(first, sizeof(first)) == sizeof(first) && file.lastError() == 0,
            "a successful write clears the error code");
    require(file.close() && !file && file.close(), "closing twice is harmless");
}

void testCloseFailure(const std::string& directory) {
    const auto path = directory + "/close.pcap";
    std::ofstream(path) << "x";
    // Single-threaded here: no other thread can take or close the descriptor number in between.
    const int spare = ::open(path.c_str(), O_RDONLY);
    require(spare >= 0 && ::close(spare) == 0, "find the lowest free descriptor");
    SdFile file;
    require(file.openRead(path.c_str()), "open the file on that descriptor");
    struct stat byDescriptor{};
    struct stat byPath{};
    require(fstat(spare, &byDescriptor) == 0 && stat(path.c_str(), &byPath) == 0 &&
                byDescriptor.st_ino == byPath.st_ino && byDescriptor.st_dev == byPath.st_dev,
            "the file uses the lowest free descriptor");
    // The file reopened the closed descriptor number, so this closes the file's descriptor.
    // cppcheck-suppress doubleFree
    require(::close(spare) == 0, "close the descriptor behind the file");
    require(!file.sync(), "report a failed sync");
    require(!file.close() && !file, "report a failed close");
}

void testDescriptorZero(const std::string& directory) {
    const auto path = directory + "/zero.pcap";
    const int input = dup(0); // Fails only when standard input is already closed.
    require(input < 0 || ::close(0) == 0, "free descriptor zero");
    SdFile file;
    const bool opened = file.openNew(path.c_str());
    struct stat byDescriptor{};
    struct stat byPath{};
    const bool onZero = fstat(0, &byDescriptor) == 0 && stat(path.c_str(), &byPath) == 0 &&
                        byDescriptor.st_ino == byPath.st_ino;
    const bool usable = static_cast<bool>(file);
    const bool closed = file.close();
    require(input < 0 || (dup2(input, 0) == 0 && ::close(input) == 0), "restore standard input");
    require(opened && onZero, "open a file on descriptor zero");
    require(usable && closed, "descriptor zero is an open file");
}

void testProbePublishRemove(const std::string& directory) {
    const auto from = directory + "/from";
    const auto to = directory + "/to";
    std::ofstream(from) << "new";
    std::ofstream(to) << "old";
    require(SdFile::probe(from.c_str()) == FilePresence::present &&
                SdFile::probe((directory + "/none").c_str()) == FilePresence::missing &&
                SdFile::probe(directory.c_str()) == FilePresence::error &&
                SdFile::probe((from + "/inner").c_str()) == FilePresence::error,
            "classify regular, missing, and unusable paths");
    require(!SdFile::publish(from.c_str(), to.c_str()), "never replace a published file");
    std::ifstream kept(to);
    require(std::string(std::istreambuf_iterator<char>(kept), {}) == "old" &&
                SdFile::probe(from.c_str()) == FilePresence::present,
            "a refused publish keeps both files");
    SdFile::remove(to.c_str());
    require(SdFile::probe(to.c_str()) == FilePresence::missing, "remove a file");
    require(SdFile::publish(from.c_str(), to.c_str()) &&
                SdFile::probe(from.c_str()) == FilePresence::missing &&
                SdFile::probe(to.c_str()) == FilePresence::present,
            "publish renames the staged file");
    require(!SdFile::publish(from.c_str(), (directory + "/again").c_str()),
            "publishing a missing file fails");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): Allocation failure must fail this host test.
int main(int argc, char** argv) {
    signal(SIGALRM, reportStuckGroup);
    requireEmptyDirectory(argc, argv);
    runGroup("testOverflowAndWrap", testOverflowAndWrap);
    runGroup("testPartialAndZeroWrite", testPartialAndZeroWrite);
    runGroup("testHardErrorAndInvalidCount", testHardErrorAndInvalidCount);
    runGroup("testProducerDuringWrite", testProducerDuringWrite);
    runGroup("testOpenFailureAndExclusiveCreate",
             [&] { testOpenFailureAndExclusiveCreate(argv[1]); });
    runGroup("testFileSizeLimit", [&] { testFileSizeLimit(argv[1]); });
    runGroup("testBytesAccepted", testBytesAccepted);
    size_t skipped = 0;
    if (threadSanitizer) {
        // ThreadSanitizer makes four billion atomic updates take minutes.
        std::cout << "SD writer: skipped testOverflowCountSaturates under ThreadSanitizer\n";
        ++skipped;
    } else {
        // About 2 s at -O1 and 100 s in an -O0 AddressSanitizer build.
        runGroup("testOverflowCountSaturates", testOverflowCountSaturates, 600);
    }
    runGroup("testFileNamesAndModes", [&] { testFileNamesAndModes(argv[1]); });
    runGroup("testCloseFailure", [&] { testCloseFailure(argv[1]); });
    runGroup("testDescriptorZero", [&] { testDescriptorZero(argv[1]); });
    runGroup("testProbePublishRemove", [&] { testProbePublishRemove(argv[1]); });
    std::cout << "SD writer: " << groupsPassed << " groups passed, " << skipped << " skipped\n";
}
