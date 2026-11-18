#pragma once

#include <stddef.h>
#include <stdint.h>

namespace airrounds {
namespace storage {

enum class WriteStatus {
    complete,
    unavailable,
    partial,
    no_progress,
    invalid,
    io_error,
    idle,
};

// One caller owns the file, the offset, and the immutable block until it completes.
// Keep the file positioned immediately after the accepted prefix on every retry.
// An invalid result requires checking the file before retrying.
// Accepted bytes are reported by the file write API and have not been verified on disk.
template <typename File>
WriteStatus writePending(File& file, const uint8_t* bytes, size_t size, size_t& accepted) {
    if (accepted > size || (accepted < size && bytes == nullptr)) {
        return WriteStatus::invalid;
    }
    if (accepted == size) {
        return WriteStatus::complete;
    }
    if (!file) {
        return WriteStatus::unavailable;
    }

    const size_t remaining = size - accepted;
    const size_t written = file.write(bytes + accepted, remaining);
    if (written > remaining) {
        return WriteStatus::invalid;
    }
    if (written == 0) {
        return WriteStatus::no_progress;
    }

    accepted += written;
    return accepted == size ? WriteStatus::complete : WriteStatus::partial;
}

}  // namespace storage
}  // namespace airrounds
