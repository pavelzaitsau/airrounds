#include "pending_write.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using airrounds::storage::writePending;
using airrounds::storage::WriteStatus;

namespace {

constexpr std::array<uint8_t, 4> sample{0x11, 0x22, 0x33, 0x44};

struct MemoryFile {
    bool open = true;
    size_t writeLimit = sample.size();
    bool badCount = false;
    size_t calls = 0;
    std::vector<uint8_t> contents;

    explicit operator bool() const { return open; }

    size_t write(const uint8_t* bytes, size_t size) {
        ++calls;
        if (badCount) {
            return std::numeric_limits<size_t>::max();
        }
        const size_t count = std::min(size, writeLimit);
        contents.insert(contents.end(), bytes, bytes + count);
        return count;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testFullWriteAndRepeat() {
    MemoryFile file;
    size_t accepted = 0;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::complete,
            "full write must complete");
    require(accepted == sample.size(), "full write must account for every byte");
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::complete,
            "completed writes must be safe to repeat");
    require(file.calls == 1 && file.contents == std::vector<uint8_t>(sample.begin(), sample.end()),
            "repeating a completed write must not append bytes");
}

void testClosedFileAndRetry() {
    MemoryFile file;
    file.open = false;
    size_t accepted = 0;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::unavailable,
            "closed file must report unavailable");
    require(accepted == 0 && file.calls == 0, "failed open must leave the entire block pending");
    file.open = true;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::complete,
            "opening the file must allow the pending block to be written");
}

void testShortWriteAndRetry() {
    MemoryFile file;
    file.writeLimit = 2;
    size_t accepted = 0;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::partial,
            "short write must report partial");
    require(accepted == 2 && file.contents == std::vector<uint8_t>({0x11, 0x22}),
            "short write must retain the exact accepted prefix");
    file.writeLimit = 0;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::no_progress,
            "full storage must report no progress");
    require(accepted == 2, "failed retry must preserve the previously accepted prefix");
    file.writeLimit = 2;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::complete,
            "retry must complete the block");
    require(file.contents == std::vector<uint8_t>(sample.begin(), sample.end()),
            "retry must only append the unwritten suffix");
}

void testEmptyAndInvalidWrites() {
    MemoryFile file;
    size_t accepted = 0;
    // Cppcheck can prove the expected result for this explicit invalid/empty input.
    // cppcheck-suppress knownConditionTrueFalse
    require(writePending(file, nullptr, 0, accepted) == WriteStatus::complete,
            "empty block must complete without I/O");
    require(writePending(file, nullptr, 1, accepted) == WriteStatus::invalid && accepted == 0 &&
                file.calls == 0,
            "nonempty null block must be rejected without I/O");
    accepted = sample.size() + 1;
    // Cppcheck can prove the expected result for this explicit invalid/empty input.
    // cppcheck-suppress knownConditionTrueFalse
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::invalid &&
                file.calls == 0,
            "out-of-range offset must be rejected without I/O");
    file.badCount = true;
    accepted = 0;
    require(writePending(file, sample.data(), sample.size(), accepted) == WriteStatus::invalid &&
                accepted == 0 && file.calls == 1,
            "invalid sink count must not advance the offset");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): Allocation failure must fail this host test.
int main() {
    testFullWriteAndRepeat();
    testClosedFileAndRetry();
    testShortWriteAndRetry();
    testEmptyAndInvalidWrites();
    std::cout << "pending write: 4 groups passed\n";
}
