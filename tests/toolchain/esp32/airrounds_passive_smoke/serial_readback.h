#pragma once

#include "pcap_format.h"
#include "sd_file.h"

namespace airrounds {
namespace bench {

template <typename SerialOutput>
bool printHexFile(SerialOutput& output, const char* path, size_t expectedSize, const char* label) {
    storage::SdFile input;
    if (expectedSize == 0 || !input.openRead(path)) {
        return false;
    }
    size_t remaining = expectedSize;
    const size_t labelLength = strlen(label);
    constexpr size_t chunkSize = 24;
    constexpr char hex[] = "0123456789abcdef";
    uint8_t bytes[chunkSize];
    while (remaining != 0) {
        const size_t length = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        if (!storage::pcap::readExact(input, bytes, length)) {
            return false;
        }
        char encoded[2 * chunkSize + 1];
        for (size_t index = 0; index < length; ++index) {
            encoded[2 * index] = hex[bytes[index] >> 4U];
            encoded[2 * index + 1] = hex[bytes[index] & 0x0fU];
        }
        encoded[2 * length] = '\0';
        if (output.printf("%s %s\n", label, encoded) != labelLength + 2 * length + 2) {
            return false;
        }
        remaining -= length;
    }

    uint8_t extra;
    const bool atEnd = input.read(&extra, 1) == 0;
    return input.close() && atEnd;
}

} // namespace bench
} // namespace airrounds
