// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "../../flipper/esp_flasher.h"

namespace {

using namespace airrounds::flipper;

constexpr uint8_t slipEnd = 0xC0;
constexpr uint8_t slipEsc = 0xDB;

void slipEncode(uint8_t command, uint32_t value, const std::vector<uint8_t>& payload,
                std::deque<uint8_t>& out) {
    std::vector<uint8_t> packet{0x01, command, static_cast<uint8_t>(payload.size()),
                                static_cast<uint8_t>(payload.size() >> 8U)};
    for (unsigned shift = 0; shift < 32; shift += 8) {
        packet.push_back(static_cast<uint8_t>(value >> shift));
    }
    packet.insert(packet.end(), payload.begin(), payload.end());
    out.push_back(slipEnd);
    for (const uint8_t byte : packet) {
        if (byte == slipEnd) {
            out.push_back(slipEsc);
            out.push_back(0xDC);
        } else if (byte == slipEsc) {
            out.push_back(slipEsc);
            out.push_back(0xDD);
        } else {
            out.push_back(byte);
        }
    }
    out.push_back(slipEnd);
}

// A fake ESP32-S2 ROM loader: it decodes the SLIP command frames the flasher sends, reassembles the
// written blocks, and queues the responses the flasher reads back.
struct FakeBoard {
    std::deque<uint8_t> out;
    std::map<uint32_t, std::vector<uint8_t>> blocks;
    std::vector<uint8_t> image; // The source the flasher reads and the board should receive.
    std::string reportedMd5 = std::string(32, 'a');
    uint32_t beginBlocks = 0;
    uint32_t beginOffset = 0;
    uint32_t beginBlockSize = 0;
    bool ended = false;
    bool answerSync = true;
    bool checksumOk = true;
    bool shortImage = false;
    bool oversizedMd5 = false;
    bool escapeInResponses = false;
    std::vector<bool> bootCalls;

    // SLIP decode of the incoming command stream.
    std::vector<uint8_t> current;
    bool inFrame = false;
    bool escaped = false;

    void feed(const uint8_t* data, size_t size) {
        for (size_t index = 0; index < size; ++index) {
            const uint8_t byte = data[index];
            if (byte == slipEnd) {
                if (inFrame && !current.empty()) {
                    process(current);
                }
                current.clear();
                inFrame = true;
                continue;
            }
            if (!inFrame) {
                continue;
            }
            uint8_t value = byte;
            if (escaped) {
                value = byte == 0xDC ? slipEnd : byte == 0xDD ? slipEsc : byte;
                escaped = false;
            } else if (byte == slipEsc) {
                escaped = true;
                continue;
            }
            current.push_back(value);
        }
    }

    static uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
        return static_cast<uint32_t>(bytes[offset]) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
               (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
    }

    void process(const std::vector<uint8_t>& packet) {
        const uint8_t command = packet[1];
        const uint8_t checksum = packet[4];
        const std::vector<uint8_t> data(packet.begin() + 8, packet.end());
        const std::vector<uint8_t> okStatus{0, 0};
        // A value field full of 0xC0/0xDB exercises the flasher's incoming SLIP un-escape; the
        // flasher ignores the value, but must un-escape it to keep the frame aligned.
        const uint32_t responseValue = escapeInResponses ? 0xDBC0DBC0U : 0;
        switch (command) {
        case 0x08: // SYNC
            if (answerSync) {
                slipEncode(command, responseValue, okStatus, out);
            }
            break;
        case 0x02: // FLASH_BEGIN
            beginBlocks = readU32(data, 4);
            beginBlockSize = readU32(data, 8);
            beginOffset = readU32(data, 12);
            slipEncode(command, responseValue, okStatus, out);
            break;
        case 0x03: // FLASH_DATA
            recordBlock(data, checksum);
            slipEncode(command, responseValue, okStatus, out);
            break;
        case 0x04: // FLASH_END
            ended = true;
            slipEncode(command, responseValue, okStatus, out);
            break;
        case 0x13: { // SPI_FLASH_MD5
            std::string digest = reportedMd5;
            // A malformed or noisy loader can frame a response far longer than the 32-hex digest.
            // The flasher must clamp the copy to its own buffer rather than overflow it.
            if (oversizedMd5) {
                digest.resize(46, reportedMd5.empty() ? 'a' : reportedMd5[0]);
            }
            std::vector<uint8_t> payload(digest.begin(), digest.end());
            payload.push_back(0);
            payload.push_back(0);
            slipEncode(command, responseValue, payload, out);
            break;
        }
        default:
            break;
        }
    }

    void recordBlock(const std::vector<uint8_t>& data, uint8_t checksum) {
        const uint32_t sequence = readU32(data, 4);
        const std::vector<uint8_t> block(data.begin() + 16, data.end());
        const uint8_t computed =
            std::accumulate(block.begin(), block.end(), static_cast<uint8_t>(0xEF),
                            [](uint8_t accumulator, uint8_t byte) {
                                return static_cast<uint8_t>(accumulator ^ byte);
                            });
        if (computed != checksum) {
            checksumOk = false;
        }
        blocks[sequence] = block;
    }

    // Reassemble the written blocks in order and truncate to the image size.
    std::vector<uint8_t> received(size_t size) const {
        std::vector<uint8_t> all;
        for (const auto& entry : blocks) {
            all.insert(all.end(), entry.second.begin(), entry.second.end());
        }
        all.resize(size);
        return all;
    }
};

void writeIo(void* context, const uint8_t* data, size_t size) {
    static_cast<FakeBoard*>(context)->feed(data, size);
}

bool readByteIo(void* context, uint8_t* byte, uint32_t /*timeoutMs*/) {
    auto& board = *static_cast<FakeBoard*>(context);
    if (board.out.empty()) {
        return false;
    }
    *byte = board.out.front();
    board.out.pop_front();
    return true;
}

void bootIo(void* context, bool down) {
    static_cast<FakeBoard*>(context)->bootCalls.push_back(down);
}

size_t readImageIo(void* context, uint32_t offset, uint8_t* out, size_t size) {
    const auto& board = *static_cast<FakeBoard*>(context);
    if (board.shortImage || offset + size > board.image.size()) {
        return 0;
    }
    memcpy(out, board.image.data() + offset, size);
    return size;
}

FlasherIo ioFor(FakeBoard& board) {
    FlasherIo io;
    io.context = &board;
    io.write = writeIo;
    io.readByte = readByteIo;
    io.boot = bootIo;
    io.readImage = readImageIo;
    return io;
}

FlashManifest manifestFor(const std::string& md5, uint32_t size) {
    std::string json = "{ \"offset\": 0, \"size\": " + std::to_string(size) +
                       ", \"variant\": "
                       "\"passive\", \"md5\": \"" +
                       md5 + "\" }";
    return parseManifest(json.c_str());
}

std::vector<uint8_t> patternImage(size_t size) {
    std::vector<uint8_t> image(size);
    for (size_t index = 0; index < size; ++index) {
        image[index] = static_cast<uint8_t>(index * 7 + 3);
    }
    return image;
}

void manifestParsing() {
    const auto valid =
        parseManifest("{ \"offset\": 4096, \"size\": 780000, \"variant\": \"active\", "
                      "\"md5\": \"0123456789abcdef0123456789abcdef\" }");
    assert(valid.valid);
    assert(valid.offset == 4096);
    assert(valid.size == 780000);
    assert(valid.active);
    assert(std::string(valid.md5) == "0123456789abcdef0123456789abcdef");

    // An uppercase digest is accepted and lowercased.
    const auto upper = parseManifest("{ \"size\": 10, \"offset\": 0, "
                                     "\"md5\": \"ABCDEF0123456789ABCDEF0123456789\" }");
    assert(upper.valid && !upper.active);
    assert(std::string(upper.md5) == "abcdef0123456789abcdef0123456789");

    // A missing size, a short digest, and a non-hex digest each reject the manifest.
    assert(
        !parseManifest("{ \"offset\": 0, \"md5\": \"0123456789abcdef0123456789abcdef\" }").valid);
    assert(!parseManifest("{ \"size\": 10, \"offset\": 0, \"md5\": \"abcd\" }").valid);
    assert(!parseManifest("{ \"size\": 10, \"offset\": 0, "
                          "\"md5\": \"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\" }")
                .valid);
}

void flashesTheImage() {
    FakeBoard board;
    board.image = patternImage(2500); // Three 1024-byte blocks, the last partial.
    const std::string md5(32, 'c');
    board.reportedMd5 = md5;
    EspFlasher flasher(ioFor(board));
    const FlashError result = flasher.flash(manifestFor(md5, 2500));
    assert(result == FlashError::none);
    assert(flasher.stage() == FlashStage::done);
    assert(board.beginBlocks == 3);
    assert(board.beginBlockSize == EspFlasher::blockSize);
    assert(board.beginOffset == 0);
    assert(board.ended);
    assert(board.checksumOk);
    assert(board.received(2500) == board.image);
    // The last block is padded to the block size with 0xFF.
    assert(board.blocks.at(2).size() == EspFlasher::blockSize);
    assert(board.blocks.at(2).back() == 0xFF);
    // Enter the loader before the flash, boot the application only after a verified write.
    assert(board.bootCalls.size() == 2);
    assert(board.bootCalls.front() == true);
    assert(board.bootCalls.back() == false);
}

void slipRoundTripsEscapes() {
    FakeBoard board;
    board.image = std::vector<uint8_t>{0xC0, 0xDB, 0xC0, 0xDB, 0xDC, 0xDD, 0x00, 0xFF, 0xC0};
    const std::string md5(32, 'd');
    board.reportedMd5 = md5;
    EspFlasher flasher(ioFor(board));
    const FlashError result =
        flasher.flash(manifestFor(md5, static_cast<uint32_t>(board.image.size())));
    assert(result == FlashError::none);
    assert(board.checksumOk);
    assert(board.received(board.image.size()) == board.image);
}

void decodesIncomingSlipEscapes() {
    FakeBoard board;
    board.image = patternImage(1024);
    const std::string md5(32, 'a');
    board.reportedMd5 = md5;
    // Every board response carries 0xC0/0xDB in its value field. A broken incoming un-escape would
    // misalign the frame and the status or digest would read wrong, so a clean flash proves it.
    board.escapeInResponses = true;
    EspFlasher flasher(ioFor(board));
    const FlashError result = flasher.flash(manifestFor(md5, 1024));
    assert(result == FlashError::none);
    assert(board.checksumOk);
}

void mismatchStopsBeforeReset() {
    FakeBoard board;
    board.image = patternImage(1024);
    board.reportedMd5 = std::string(32, 'e'); // The board reports a digest the manifest does not.
    EspFlasher flasher(ioFor(board));
    const FlashError result = flasher.flash(manifestFor(std::string(32, 'f'), 1024));
    assert(result == FlashError::mismatch);
    assert(flasher.stage() == FlashStage::verify);
    // The board entered the loader but was never reset into the application.
    assert(board.bootCalls.size() == 1 && board.bootCalls.front() == true);
}

void oversizedMd5ResponseDoesNotOverflow() {
    FakeBoard board;
    board.image = patternImage(1024);
    const std::string md5(32, 'a');
    board.reportedMd5 = md5;
    board.oversizedMd5 = true; // The board frames a 46-byte digest; the flasher clamps the copy.
    EspFlasher flasher(ioFor(board));
    // The first 32 bytes still match the manifest, so the verify passes; the point is the clamp
    // keeps the oversized response from writing past the flasher's digest buffer (ASan would trip).
    const FlashError result = flasher.flash(manifestFor(md5, 1024));
    assert(result == FlashError::none);
}

void syncFailureReported() {
    FakeBoard board;
    board.image = patternImage(1024);
    board.answerSync = false;
    EspFlasher flasher(ioFor(board));
    const FlashError result = flasher.flash(manifestFor(std::string(32, 'a'), 1024));
    assert(result == FlashError::sync);
    assert(board.bootCalls.size() == 1); // Entered the loader, never reset.
}

void imageReadErrorReported() {
    FakeBoard board;
    board.image = patternImage(1024);
    board.shortImage = true;
    const std::string md5(32, 'a');
    board.reportedMd5 = md5;
    EspFlasher flasher(ioFor(board));
    const FlashError result = flasher.flash(manifestFor(md5, 1024));
    assert(result == FlashError::image);
}

void invalidManifestIsRejected() {
    FakeBoard board;
    EspFlasher flasher(ioFor(board));
    assert(flasher.flash(parseManifest("{}")) == FlashError::manifest);
    assert(board.bootCalls.empty()); // Nothing drives the board without a valid manifest.
}

} // namespace

int main() {
    manifestParsing();
    flashesTheImage();
    slipRoundTripsEscapes();
    decodesIncomingSlipEscapes();
    mismatchStopsBeforeReset();
    oversizedMd5ResponseDoesNotOverflow();
    syncFailureReported();
    imageReadErrorReported();
    invalidManifestIsRejected();
    std::cout << "esp_flasher_test ok\n";
    return 0;
}
