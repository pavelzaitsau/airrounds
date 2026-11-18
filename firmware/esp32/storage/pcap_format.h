#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace airrounds {
namespace storage {
namespace pcap {

class Crc32 {
public:
    void update(const uint8_t* data, size_t size) {
        static constexpr uint32_t table[] = {
            0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
            0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
            0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
        };
        for (size_t index = 0; index < size; ++index) {
            value_ ^= data[index];
            value_ = (value_ >> 4U) ^ table[value_ & 0xfU];
            value_ = (value_ >> 4U) ^ table[value_ & 0xfU];
        }
    }

    uint32_t value() const { return value_ ^ UINT32_MAX; }

private:
    uint32_t value_ = UINT32_MAX;
};

template <typename File> bool readExact(File& file, uint8_t* data, size_t size) {
    while (size != 0) {
        const auto count = file.read(data, size);
        if (count <= 0 || static_cast<size_t>(count) > size) {
            return false;
        }
        data += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

inline uint32_t readNumber(const uint8_t* bytes, size_t size, bool littleEndian) {
    uint32_t value = 0;
    for (size_t index = 0; index < size; ++index) {
        value = (value << 8U) | bytes[littleEndian ? size - 1 - index : index];
    }
    return value;
}

// Keep the existing size/CRC template API; both values are checked by callers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
template <typename File> bool validate(File& file, uint64_t expectedSize, uint32_t expectedCrc) {
    uint8_t header[24];
    if (expectedSize < sizeof(header) || !readExact(file, header, sizeof(header))) {
        return false;
    }

    const uint32_t magic = readNumber(header, 4, false);
    const bool littleEndian = magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1;
    const bool nano = magic == 0xa1b23c4d || magic == 0x4d3cb2a1;
    if (!(littleEndian || nano || magic == 0xa1b2c3d4) ||
        readNumber(header + 4, 2, littleEndian) != 2 ||
        readNumber(header + 6, 2, littleEndian) != 4) {
        return false;
    }

    const uint32_t snapshotLength = readNumber(header + 16, 4, littleEndian);
    const uint32_t fractionsPerSecond = nano ? 1000000000 : 1000000;
    uint64_t remaining = expectedSize - sizeof(header);
    if (snapshotLength == 0 || remaining == 0) {
        return false;
    }

    Crc32 crc;
    crc.update(header, sizeof(header));
    uint8_t record[16];
    uint8_t buffer[256];
    while (remaining != 0) {
        if (remaining < sizeof(record) || !readExact(file, record, sizeof(record))) {
            return false;
        }
        remaining -= sizeof(record);
        crc.update(record, sizeof(record));

        const uint32_t captured = readNumber(record + 8, 4, littleEndian);
        const uint32_t original = readNumber(record + 12, 4, littleEndian);
        if (captured == 0 || captured > snapshotLength || captured > original ||
            captured > remaining || readNumber(record + 4, 4, littleEndian) >= fractionsPerSecond) {
            return false;
        }

        for (uint32_t unread = captured; unread != 0;) {
            const size_t chunk = unread < sizeof(buffer) ? unread : sizeof(buffer);
            if (!readExact(file, buffer, chunk)) {
                return false;
            }
            crc.update(buffer, chunk);
            unread -= chunk;
        }
        remaining -= captured;
    }

    return crc.value() == expectedCrc && file.read(buffer, 1) == 0;
}

} // namespace pcap
} // namespace storage
} // namespace airrounds
