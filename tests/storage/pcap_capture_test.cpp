#include "pcap_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <csignal>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace airrounds::storage;

namespace {

// Classic PCAP with a single synthetic Ethernet frame.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): Immutable host test fixture.
const std::vector<uint8_t> sample{
    0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0xff, 0xff,
    0,    0,    1,    0,    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 14, 0, 0,    0,
    14,   0,    0,    0,    2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0,  2, 0x88, 0xb5,
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void require(bool condition, const std::string& message) {
    require(condition, message.c_str());
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

template <typename Capture> void enqueue(Capture& capture, const std::vector<uint8_t>& bytes) {
    require(capture.enqueue(bytes.data(), bytes.size()) == EnqueueStatus::queued,
            "queue synthetic PCAP bytes");
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void testFinishAndNextFile(const std::string& directory) {
    const auto first = directory + "/first.pcap";
    const auto second = directory + "/second.pcap";
    {
        PcapCapture<2, 64> capture(first.c_str());
        require(capture.open(), "open first capture");
        require(capture.enqueue(sample.data(), 8) == EnqueueStatus::queued, "queue prefix");
        require(capture.drainOne() == WriteStatus::complete, "write prefix");
        require(capture.enqueue(sample.data() + 8, sample.size() - 8) == EnqueueStatus::queued,
                "queue remaining bytes");
        require(recoverPcap(first.c_str()) == CaptureState::incomplete,
                "do not report saved before finalization");
        require(capture.finish() == CaptureState::saved,
                "finish drains and checks the closed file");
        require(capture.finish() == CaptureState::saved, "repeat finish is safe");
        require(capture.enqueue(sample.data(), 1) == EnqueueStatus::invalid,
                "reject writes after finalization");
    }
    {
        PcapCapture<2, 64> capture(first.c_str());
        require(!capture.open(), "refuse to replace a capture");
    }
    {
        PcapCapture<2, 64> capture(second.c_str());
        require(capture.open(), "open an independent capture");
        enqueue(capture, sample);
        require(capture.finish() == CaptureState::saved, "finish the next capture");
    }
    require(readFile(first) == sample && readFile(second) == sample, "keep both files intact");
    require(recoverPcap(first.c_str()) == CaptureState::saved &&
                recoverPcap(second.c_str()) == CaptureState::saved,
            "restart verifies both files independently");
}

void testInvalidPcapAndOverflow(const std::string& directory) {
    auto malformed = sample;
    malformed[32] = 30; // The record claims a payload longer than its original frame.
    const std::vector<std::vector<uint8_t>> badInputs{
        {sample.begin(), sample.begin() + 12},
        {sample.begin(), sample.begin() + 24},
        {sample.begin(), sample.begin() + 33},
        {sample.begin(), sample.end() - 1},
        malformed,
    };
    for (size_t index = 0; index < badInputs.size(); ++index) {
        const auto path = directory + "/bad" + std::to_string(index) + ".pcap";
        PcapCapture<1, 64> capture(path.c_str());
        const auto name = " (bad input " + std::to_string(index) + ")";
        require(capture.open(), "create malformed capture" + name);
        enqueue(capture, badInputs[index]);
        require(capture.finish() == CaptureState::error, "reject malformed or empty PCAP" + name);
        require(recoverPcap(path.c_str()) == CaptureState::incomplete,
                "leave failed output unsealed" + name);
    }

    const auto overflow = directory + "/overflow.pcap";
    PcapCapture<1, 64> capture(overflow.c_str());
    require(capture.open(), "create overflow case");
    enqueue(capture, sample);
    require(capture.enqueue(sample.data(), 1) == EnqueueStatus::full, "drop one block");
    require(capture.finish() == CaptureState::incomplete, "overflow prevents publishing a capture");
    require(recoverPcap(overflow.c_str()) == CaptureState::incomplete,
            "leave overflow output unsealed");
}

void testInterruptedProcess(const std::string& directory) {
    const auto path = directory + "/interrupted.pcap";
    require(recoverPcap(path.c_str()) == CaptureState::missing, "unknown capture is missing");
    const pid_t child = fork();
    require(child >= 0, "fork for restart test");
    if (child == 0) {
        PcapCapture<1, 64> capture(path.c_str());
        require(capture.open(), "create interrupted capture");
        enqueue(capture, sample);
        require(capture.drainOne() == WriteStatus::complete, "write before restart");
        _exit(0); // Skip finalization and object cleanup.
    }

    int childStatus = 0;
    require(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
                WEXITSTATUS(childStatus) == 0,
            "capture process exited before finalization");
    require(readFile(path) == sample, "interruption left a structurally valid PCAP");
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "a valid unsealed PCAP stays incomplete after restart");
}

void testDamageAfterSave(const std::string& directory) {
    const auto path = directory + "/damaged.pcap";
    PcapCapture<2, 64> capture(path.c_str());
    require(capture.open(), "create damage case");
    enqueue(capture, sample);
    const std::vector<uint8_t> record(sample.begin() + 24, sample.end());
    enqueue(capture, record);
    require(capture.finish() == CaptureState::saved, "finish before damage");
    auto corrupt = readFile(path);
    corrupt.back() ^= 1U;
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(corrupt.data()),
                     static_cast<std::streamsize>(corrupt.size()));
    }
    require(recoverPcap(path.c_str()) == CaptureState::error,
            "restart detects changed payload bytes even when the length matches");
    require(truncate(path.c_str(), static_cast<off_t>(sample.size())) == 0,
            "drop one complete record");
    require(recoverPcap(path.c_str()) == CaptureState::error,
            "restart detects truncation exactly at a record boundary");
}

struct FaultFile : SdFile {
    enum Fault {
        none,
        zero_write,
        short_seal,
        short_data,
        invalid_count,
        sync_data,
        close_data,
        reread,
        overlong_read,
        corrupt_read,
        close_read_data,
        sync_seal,
        close_seal,
        seal_reread,
        seal_magic,
        seal_size,
        seal_crc,
        close_read_seal,
        publish_seal
    };
    static Fault fault;

    // Offset of the receipt byte that a reread fault flips: magic, size, or CRC field.
    static int damagedReceiptByte() {
        switch (fault) {
        case seal_magic:
            return 6;
        case seal_size:
            return 8;
        case seal_crc:
            return 16;
        default:
            return -1;
        }
    }

    bool writingSeal = false;
    bool writingData = false;
    bool reading = false;
    bool readingSeal = false;

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool openNew(const char* path) {
        writingSeal = std::string(path).find(".ok.part") != std::string::npos;
        writingData = !writingSeal;
        return SdFile::openNew(path);
    }

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool openRead(const char* path) {
        reading = true;
        readingSeal = std::string(path).find(".ok.part") != std::string::npos;
        return SdFile::openRead(path);
    }

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    size_t write(const uint8_t* bytes, size_t count) {
        if (fault == zero_write && writingData) {
            return 0;
        }
        if (fault == invalid_count && writingData) {
            return std::numeric_limits<size_t>::max();
        }
        const bool shortened =
            (fault == short_seal && writingSeal) || (fault == short_data && writingData);
        return SdFile::write(bytes, shortened ? std::min(count, size_t{3}) : count);
    }

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    ssize_t read(uint8_t* bytes, size_t count) {
        if (fault == reread || (fault == seal_reread && readingSeal)) {
            return -1;
        }
        // Like a card driver, the faulty file reads at most one sector per call.
        const auto got =
            SdFile::read(bytes, fault == overlong_read ? std::min(count, size_t{512}) : count);
        if (fault == corrupt_read && got == 14) {
            bytes[got - 1] ^= 1U;
        }
        // The staged receipt arrives in one read.
        const int damaged = damagedReceiptByte();
        if (readingSeal && damaged >= 0 && got == static_cast<ssize_t>(pcapReceiptSize)) {
            bytes[damaged] ^= 1U;
        }
        // Claim one byte more than the file returned.
        return fault == overlong_read && got > 0 ? got + 1 : got;
    }

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool sync() {
        if (fault == (writingSeal ? sync_seal : sync_data)) {
            return false;
        }
        return SdFile::sync();
    }

    // Fault injection uses static template dispatch, not virtual calls.
    // cppcheck-suppress duplInheritedMember
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool close() {
        const bool closed = SdFile::close();
        return closed && !(writingSeal && fault == close_seal) &&
               !(writingData && fault == close_data) &&
               !(reading && fault == (readingSeal ? close_read_seal : close_read_data));
    }

    // cppcheck-suppress duplInheritedMember
    static bool publish(const char* from, const char* to) {
        return fault != publish_seal && SdFile::publish(from, to);
    }
};

FaultFile::Fault FaultFile::fault = FaultFile::none;

void testShortSealWrite(const std::string& directory) {
    const auto path = directory + "/short-seal.pcap";
    PcapCapture<1, 64, FaultFile> capture(path.c_str());
    require(capture.open(), "create short receipt write case");
    enqueue(capture, sample);
    FaultFile::fault = FaultFile::short_seal;
    require(capture.finish() == CaptureState::saved, "retry the unwritten receipt bytes");
    FaultFile::fault = FaultFile::none;
    require(recoverPcap(path.c_str()) == CaptureState::saved, "read the complete receipt");
}

void testFinalizationErrors(const std::string& directory) {
    const std::pair<FaultFile::Fault, const char*> faults[]{
        {FaultFile::zero_write, "zero_write"},
        {FaultFile::invalid_count, "invalid_count"},
        {FaultFile::sync_data, "sync_data"},
        {FaultFile::close_data, "close_data"},
        {FaultFile::reread, "reread"},
        {FaultFile::overlong_read, "overlong_read"},
        {FaultFile::corrupt_read, "corrupt_read"},
        {FaultFile::close_read_data, "close_read_data"},
        {FaultFile::sync_seal, "sync_seal"},
        {FaultFile::close_seal, "close_seal"},
        {FaultFile::seal_reread, "seal_reread"},
        {FaultFile::seal_magic, "seal_magic"},
        {FaultFile::seal_size, "seal_size"},
        {FaultFile::seal_crc, "seal_crc"},
        {FaultFile::close_read_seal, "close_read_seal"},
        {FaultFile::publish_seal, "publish_seal"},
    };
    for (const auto& item : faults) {
        const std::string name = std::string(" (fault ") + item.second + ")";
        const auto path = directory + "/fault-" + item.second + ".pcap";
        PcapCapture<1, 64, FaultFile> capture(path.c_str());
        require(capture.open(), "create fault case" + name);
        enqueue(capture, sample);
        FaultFile::fault = item.first;
        require(capture.finish() == CaptureState::error, "surface a finalization error" + name);
        FaultFile::fault = FaultFile::none;
        require(recoverPcap(path.c_str()) == CaptureState::incomplete,
                "a failed finalization must not appear saved on restart" + name);
    }
}

// Reverses every header and first-record field after the magic number.
void swapFieldOrder(std::vector<uint8_t>& bytes) {
    std::reverse(bytes.begin() + 4, bytes.begin() + 6);
    std::reverse(bytes.begin() + 6, bytes.begin() + 8);
    for (std::vector<uint8_t>::difference_type offset = 8; offset < 40; offset += 4) {
        std::reverse(bytes.begin() + offset, bytes.begin() + offset + 4);
    }
}

void testClassicFormats(const std::string& directory) {
    constexpr std::array<std::array<uint8_t, 4>, 4> magic{{
        {{0xd4, 0xc3, 0xb2, 0xa1}},
        {{0xa1, 0xb2, 0xc3, 0xd4}},
        {{0x4d, 0x3c, 0xb2, 0xa1}},
        {{0xa1, 0xb2, 0x3c, 0x4d}},
    }};
    for (size_t index = 0; index < magic.size(); ++index) {
        auto bytes = sample;
        std::copy(magic[index].begin(), magic[index].end(), bytes.begin());
        if (index % 2 != 0) {
            swapFieldOrder(bytes);
        }

        const auto path = directory + "/format" + std::to_string(index) + ".pcap";
        PcapCapture<1, 64> capture(path.c_str());
        require(capture.open(), "create a classic PCAP variant");
        enqueue(capture, bytes);
        require(capture.finish() == CaptureState::saved &&
                    recoverPcap(path.c_str()) == CaptureState::saved,
                "accept both byte orders and timestamp resolutions");
    }
}

void testSealRecovery(const std::string& directory) {
    const auto path = directory + "/seal.pcap";
    PcapCapture<1, 64> capture(path.c_str());
    require(capture.open(), "create seal case");
    enqueue(capture, sample);
    require(capture.finish() == CaptureState::saved, "save the test capture");

    const auto seal = path + ".ok";
    const auto stage = seal + ".part";
    require(rename(seal.c_str(), stage.c_str()) == 0, "stage a valid seal");
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "even a valid staged seal cannot mark a capture saved");
    require(rename(stage.c_str(), seal.c_str()) == 0, "restore the published seal");

    const auto good = readFile(seal);
    {
        std::ofstream output(stage, std::ios::binary);
        output.write(reinterpret_cast<const char*>(good.data()),
                     static_cast<std::streamsize>(good.size()));
    }
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "two seal names after an interrupted rename remain incomplete");
    require(unlink(stage.c_str()) == 0, "remove the staged seal");
    {
        std::ofstream output(seal, std::ios::binary | std::ios::app);
        output.put('X');
    }
    require(recoverPcap(path.c_str()) == CaptureState::error, "reject a malformed seal");
    require(unlink(path.c_str()) == 0, "remove the data file");
    require(recoverPcap(path.c_str()) == CaptureState::error, "a seal alone is an error");
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write the test file");
}

void putLittle32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

std::vector<uint8_t> withMagic(std::vector<uint8_t> bytes, std::array<uint8_t, 4> magic) {
    std::copy(magic.begin(), magic.end(), bytes.begin());
    return bytes;
}

// Little-endian nanosecond PCAP with the sample's header and record.
std::vector<uint8_t> nanoSample() {
    return withMagic(sample, {{0x4d, 0x3c, 0xb2, 0xa1}});
}

std::vector<uint8_t> withLittle32(std::vector<uint8_t> bytes, size_t offset, uint32_t value) {
    putLittle32(bytes, offset, value);
    return bytes;
}

std::vector<uint8_t> withByte(std::vector<uint8_t> bytes, size_t offset, uint8_t value) {
    bytes[offset] = value;
    return bytes;
}

CaptureState finishBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    PcapCapture<1, 128> capture(path.c_str());
    require(capture.open(), "open a capture for the test bytes");
    enqueue(capture, bytes);
    return capture.finish();
}

// Writes the bytes with a matching receipt, skipping the check that finish() runs.
CaptureState recoverWithReceipt(const std::string& path, const std::vector<uint8_t>& bytes) {
    writeFile(path, bytes);
    pcap::Crc32 crc;
    crc.update(bytes.data(), bytes.size());
    require(detail::publishSeal<SdFile>(path, detail::Seal{bytes.size(), crc.value()}),
            "publish a receipt that matches the test bytes");
    return recoverPcap(path.c_str());
}

struct RejectedPcap {
    const char* description;
    std::vector<uint8_t> bytes;
};

void testRejectedPcapFields(const std::string& directory) {
    require(recoverWithReceipt(directory + "/receipt-control.pcap", sample) == CaptureState::saved,
            "a matching receipt for a valid PCAP recovers as saved");

    auto nearMissMagic = withMagic(sample, {{0xa1, 0xb2, 0xc3, 0xd5}});
    swapFieldOrder(nearMissMagic); // Every other field parses as big-endian.
    std::vector<uint8_t> emptyRecordFirst(sample.begin(), sample.begin() + 24);
    emptyRecordFirst.insert(emptyRecordFirst.end(), 16, 0);
    emptyRecordFirst.insert(emptyRecordFirst.end(), sample.begin() + 24, sample.end());

    const std::vector<RejectedPcap> cases{
        {"reject an unknown magic number", nearMissMagic},
        {"reject major version 1", withByte(sample, 4, 1)},
        {"reject minor version 3", withByte(sample, 6, 3)},
        {"reject a zero snapshot length", withLittle32(sample, 16, 0)},
        {"reject a record with no captured bytes", emptyRecordFirst},
        {"reject a record longer than the snapshot length", withLittle32(sample, 16, 13)},
        {"reject a record longer than its original frame", withLittle32(sample, 36, 13)},
        {"reject a microsecond fraction of one second", withLittle32(sample, 28, 1000000)},
        {"reject a nanosecond fraction of one second", withLittle32(nanoSample(), 28, 1000000000)},
    };
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto name = directory + "/field" + std::to_string(index);
        require(finishBytes(name + "-finish.pcap", cases[index].bytes) == CaptureState::error,
                cases[index].description);
        require(recoverWithReceipt(name + "-receipt.pcap", cases[index].bytes) ==
                    CaptureState::error,
                cases[index].description);
    }
}

void testTimestampFractionLimits(const std::string& directory) {
    const auto micro = withLittle32(sample, 28, 999999);
    require(finishBytes(directory + "/micro-finish.pcap", micro) == CaptureState::saved &&
                recoverWithReceipt(directory + "/micro-receipt.pcap", micro) == CaptureState::saved,
            "accept the largest microsecond fraction");
    const auto nano = withLittle32(nanoSample(), 28, 999999999);
    require(finishBytes(directory + "/nano-finish.pcap", nano) == CaptureState::saved &&
                recoverWithReceipt(directory + "/nano-receipt.pcap", nano) == CaptureState::saved,
            "accept the largest nanosecond fraction");
}

void testBytesAppendedAfterSave(const std::string& directory) {
    const auto path = directory + "/appended.pcap";
    require(finishBytes(path, sample) == CaptureState::saved, "save before appending");
    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output.put(0);
    }
    require(recoverPcap(path.c_str()) == CaptureState::error,
            "restart rejects bytes appended after the sealed length");
}

void testReceiptLayout(const std::string& directory) {
    pcap::Crc32 check;
    const std::string digits = "123456789";
    check.update(reinterpret_cast<const uint8_t*>(digits.data()), digits.size());
    require(check.value() == 0xcbf43926U, "compute the standard CRC-32 check value");

    const auto path = directory + "/layout.pcap";
    require(finishBytes(path, sample) == CaptureState::saved, "save the layout case");
    // ARPCAP version 1, then the PCAP size and its CRC-32 (zlib), both little-endian.
    const std::vector<uint8_t> receipt{'A', 'R', 'P', 'C', 'A', 'P', 1,    0,    54,   0,
                                       0,   0,   0,   0,   0,   0,   0x36, 0xc3, 0x95, 0xd7};
    require(pcapReceiptSize == receipt.size() && readFile(path + ".ok") == receipt,
            "write the documented receipt record");
}

void testForeignReceiptMagic(const std::string& directory) {
    const auto path = directory + "/magic.pcap";
    require(finishBytes(path, sample) == CaptureState::saved, "save the magic case");
    const auto good = readFile(path + ".ok");
    for (size_t index = 0; index < 8; ++index) {
        writeFile(path + ".ok", withByte(good, index, static_cast<uint8_t>(good[index] ^ 1U)));
        require(recoverPcap(path.c_str()) == CaptureState::error,
                "reject a receipt with another magic number or version");
    }
    writeFile(path + ".ok", good);
    require(recoverPcap(path.c_str()) == CaptureState::saved, "restore the receipt");
}

void testOpenRefusesStaleReceipt(const std::string& directory) {
    for (const char* suffix : {".ok", ".ok.part"}) {
        const auto path = directory + "/stale" + suffix + ".pcap";
        writeFile(path + suffix, {1});
        PcapCapture<1, 64> capture(path.c_str());
        require(!capture.open(), "refuse a capture path that already has a receipt file");
        require(SdFile::probe(path.c_str()) == FilePresence::missing,
                "a refused capture creates no data file");
        require(capture.enqueue(sample.data(), sample.size()) == EnqueueStatus::invalid &&
                    capture.finish() == CaptureState::error,
                "a refused capture accepts no bytes and reports an error");
        require(readFile(path + suffix) == std::vector<uint8_t>{1}, "keep the stale receipt file");
    }
}

void testOpenOnce(const std::string& directory) {
    const auto path = directory + "/once.pcap";
    PcapCapture<1, 64> capture(path.c_str());
    require(capture.enqueue(sample.data(), sample.size()) == EnqueueStatus::invalid &&
                capture.drainOne() == WriteStatus::unavailable,
            "a capture accepts no bytes before open");
    require(capture.open(), "open the capture");
    require(!capture.open(), "a second open is refused");
    enqueue(capture, sample);
    require(capture.finish() == CaptureState::saved, "a second open leaves the capture usable");
    require(capture.drainOne() == WriteStatus::unavailable, "no writes after finish");

    PcapCapture<1, 64> unopened((directory + "/unopened.pcap").c_str());
    require(unopened.finish() == CaptureState::incomplete, "finish before open saves nothing");

    const auto blocked = directory + "/missing/blocked.pcap";
    PcapCapture<1, 64> failed(blocked.c_str());
    require(!failed.open() && !failed.open(), "a failed open stays failed");
    PcapCapture<1, 64> unnamed(nullptr);
    require(!unnamed.open() && unnamed.finish() == CaptureState::error,
            "a capture without a path fails");
    // Cppcheck can prove the expected result for this explicit invalid input.
    // cppcheck-suppress knownConditionTrueFalse
    require(recoverPcap(nullptr) == CaptureState::error && recoverPcap("") == CaptureState::error,
            "recovery without a path is an error");
}

void testUnreadablePaths(const std::string& directory) {
    const auto folder = directory + "/folder.pcap";
    require(mkdir(folder.c_str(), 0700) == 0, "create a directory at the data path");
    require(recoverPcap(folder.c_str()) == CaptureState::error, "a directory is not a capture");

    for (const char* suffix : {".ok", ".ok.part"}) {
        const auto path = directory + "/folder" + suffix + ".pcap";
        writeFile(path, sample);
        require(mkdir((path + suffix).c_str(), 0700) == 0, "create a directory at a receipt path");
        require(recoverPcap(path.c_str()) == CaptureState::error,
                "a directory is not a receipt file");
    }

    writeFile(directory + "/plain", sample);
    require(recoverPcap((directory + "/plain/inner.pcap").c_str()) == CaptureState::error,
            "a path that cannot be checked is an error");
}

void testReceiptAppearsDuringCapture(const std::string& directory) {
    const auto path = directory + "/raced.pcap";
    PcapCapture<1, 64> capture(path.c_str());
    require(capture.open(), "open the raced capture");
    enqueue(capture, sample);
    writeFile(path + ".ok", {7});
    require(capture.finish() == CaptureState::error, "never replace a receipt that appeared");
    require(readFile(path + ".ok") == std::vector<uint8_t>{7}, "keep the other receipt");
}

void testPartialDataWrites(const std::string& directory) {
    const auto path = directory + "/partial-data.pcap";
    PcapCapture<1, 64, FaultFile> capture(path.c_str());
    require(capture.open(), "create the partial data case");
    enqueue(capture, sample);
    FaultFile::fault = FaultFile::short_data;
    require(capture.drainOne() == WriteStatus::partial && capture.bytesAccepted() == 3,
            "count the accepted prefix of a partly written block");
    require(capture.finish() == CaptureState::saved, "finish retries partial data writes");
    FaultFile::fault = FaultFile::none;
    require(capture.bytesAccepted() == sample.size() && readFile(path) == sample &&
                recoverPcap(path.c_str()) == CaptureState::saved,
            "save every byte once after partial writes");
}

void testStalledLaterBlock(const std::string& directory) {
    const auto path = directory + "/stalled.pcap";
    PcapCapture<2, 64, FaultFile> capture(path.c_str());
    require(capture.open(), "create the stalled block case");
    enqueue(capture, sample);
    require(capture.drainOne() == WriteStatus::complete, "write a complete PCAP first");
    enqueue(capture, std::vector<uint8_t>(sample.begin() + 24, sample.end()));
    FaultFile::fault = FaultFile::zero_write;
    require(capture.finish() == CaptureState::error,
            "a queued block that makes no progress fails the capture");
    FaultFile::fault = FaultFile::none;
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "a capture that lost a queued block is not saved");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): Allocation failure must fail this host test.
int main(int argc, char** argv) {
    signal(SIGALRM, reportStuckGroup);
    requireEmptyDirectory(argc, argv);
    runGroup("testFinishAndNextFile", [&] { testFinishAndNextFile(argv[1]); });
    runGroup("testInvalidPcapAndOverflow", [&] { testInvalidPcapAndOverflow(argv[1]); });
    runGroup("testInterruptedProcess", [&] { testInterruptedProcess(argv[1]); });
    runGroup("testDamageAfterSave", [&] { testDamageAfterSave(argv[1]); });
    runGroup("testShortSealWrite", [&] { testShortSealWrite(argv[1]); });
    runGroup("testFinalizationErrors", [&] { testFinalizationErrors(argv[1]); });
    runGroup("testClassicFormats", [&] { testClassicFormats(argv[1]); });
    runGroup("testSealRecovery", [&] { testSealRecovery(argv[1]); });
    runGroup("testRejectedPcapFields", [&] { testRejectedPcapFields(argv[1]); });
    runGroup("testTimestampFractionLimits", [&] { testTimestampFractionLimits(argv[1]); });
    runGroup("testBytesAppendedAfterSave", [&] { testBytesAppendedAfterSave(argv[1]); });
    runGroup("testReceiptLayout", [&] { testReceiptLayout(argv[1]); });
    runGroup("testForeignReceiptMagic", [&] { testForeignReceiptMagic(argv[1]); });
    runGroup("testOpenRefusesStaleReceipt", [&] { testOpenRefusesStaleReceipt(argv[1]); });
    runGroup("testOpenOnce", [&] { testOpenOnce(argv[1]); });
    runGroup("testUnreadablePaths", [&] { testUnreadablePaths(argv[1]); });
    runGroup("testReceiptAppearsDuringCapture", [&] { testReceiptAppearsDuringCapture(argv[1]); });
    runGroup("testPartialDataWrites", [&] { testPartialDataWrites(argv[1]); });
    runGroup("testStalledLaterBlock", [&] { testStalledLaterBlock(argv[1]); });
    std::cout << "PCAP capture: " << groupsPassed << " groups passed\n";
}
