#pragma once

#include <atomic>
#include <string.h>

#include "pending_write.h"

namespace airrounds {
namespace storage {

enum class EnqueueStatus {
    queued,
    full,
    invalid,
};

// One producer calls enqueue and one consumer owns the file and calls drainOne.
template <size_t Capacity, size_t MaxBlockSize, typename File>
class WriteQueue {
    static_assert(Capacity > 0 && Capacity < UINT32_MAX, "invalid queue capacity");
    static_assert(MaxBlockSize > 0, "invalid block size");

public:
    explicit WriteQueue(File& file) : file_(file) {}

    EnqueueStatus enqueue(const uint8_t* data, size_t size) {
        if (data == nullptr || size == 0 || size > MaxBlockSize) {
            return EnqueueStatus::invalid;
        }

        const uint32_t index = writeIndex_.load(std::memory_order_relaxed);
        const uint32_t next = nextIndex(index);
        if (next == readIndex_.load(std::memory_order_acquire)) {
            const uint32_t count = overflowCount_.load(std::memory_order_relaxed);
            if (count < UINT32_MAX) {
                overflowCount_.store(count + 1, std::memory_order_relaxed);
            }
            return EnqueueStatus::full;
        }

        Block& block = blocks_[index];
        memcpy(block.data, data, size);
        block.size = size;
        writeIndex_.store(next, std::memory_order_release);
        return EnqueueStatus::queued;
    }

    WriteStatus drainOne() {
        if (failure_ != WriteStatus::complete) {
            return failure_;
        }

        const uint32_t index = readIndex_.load(std::memory_order_relaxed);
        if (index == writeIndex_.load(std::memory_order_acquire)) {
            return WriteStatus::idle;
        }

        const Block& block = blocks_[index];
        const auto status = writePending(file_, block.data, block.size, accepted_);
        if (status == WriteStatus::invalid ||
            (status != WriteStatus::unavailable && file_.lastError() != 0)) {
            // Keep the front block for diagnosis. A retry after a hard error needs a file check.
            failure_ = status == WriteStatus::invalid ? status : WriteStatus::io_error;
            return failure_;
        }
        if (status == WriteStatus::complete) {
            accepted_ = 0;
            readIndex_.store(nextIndex(index), std::memory_order_release);
        }
        return status;
    }

    uint32_t overflowCount() const {
        return overflowCount_.load(std::memory_order_relaxed);
    }

private:
    struct Block {
        uint8_t data[MaxBlockSize];
        size_t size;
    };

    static uint32_t nextIndex(uint32_t index) {
        return index == Capacity ? 0 : index + 1;
    }

    File& file_;
    Block blocks_[Capacity + 1];
    std::atomic<uint32_t> readIndex_{0};
    std::atomic<uint32_t> writeIndex_{0};
    std::atomic<uint32_t> overflowCount_{0};
    size_t accepted_ = 0;
    WriteStatus failure_ = WriteStatus::complete;
};

}  // namespace storage
}  // namespace airrounds
