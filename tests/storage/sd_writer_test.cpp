#include "sd_file.h"
#include "write_queue.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <sys/resource.h>
#include <string>
#include <thread>
#include <vector>

using namespace airrounds::storage;

namespace {

const uint8_t first[] = {1, 2, 3, 4};
const uint8_t second[] = {5, 6};
const uint8_t third[] = {7};
const std::vector<uint8_t> all{1, 2, 3, 4, 5, 6, 7};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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
    TestFile file;
    WriteQueue<2, 4, TestFile> queue(file);
    uint8_t input[] = {1, 2, 3, 4};
    require(queue.enqueue(input, sizeof(input)) == EnqueueStatus::queued, "first block");
    input[0] = 0;
    require(queue.enqueue(second, sizeof(second)) == EnqueueStatus::queued, "second block");
    require(queue.enqueue(third, sizeof(third)) == EnqueueStatus::full, "full queue");
    require(queue.enqueue(nullptr, 1) == EnqueueStatus::invalid, "reject null block");
    require(queue.enqueue(first, 0) == EnqueueStatus::invalid, "reject empty block");
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
    require(fullQueue.drainOne() == WriteStatus::io_error && full.lastError() == ENOSPC,
            "surface a full-card error");
    full.writeLimit = sizeof(first);
    require(fullQueue.drainOne() == WriteStatus::io_error && full.calls == 1,
            "block retries after an error with an unknown offset");
    require(fullQueue.enqueue(second, sizeof(second)) == EnqueueStatus::full,
            "retain the pending block after hard failure");

    for (size_t writeLimit : {size_t{2}, sizeof(first)}) {
        TestFile damaged;
        WriteQueue<1, 4, TestFile> damagedQueue(damaged);
        require(damagedQueue.enqueue(first, sizeof(first)) == EnqueueStatus::queued,
                "queue block");
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
    file.duringWrite = {};
    require(status == WriteStatus::complete, "first write completed");
    require(queue.drainOne() == WriteStatus::complete, "write block added during I/O");
    require(file.contents == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}) &&
                queue.overflowCount() == 1,
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

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "pass an empty temporary directory");
    testOverflowAndWrap();
    testPartialAndZeroWrite();
    testHardErrorAndInvalidCount();
    testProducerDuringWrite();
    testOpenFailureAndExclusiveCreate(argv[1]);
    testFileSizeLimit(argv[1]);
    std::cout << "SD writer: 6 groups passed\n";
}
