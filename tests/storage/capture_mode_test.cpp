#include "../../firmware/esp32/session/beacon_mode.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

using namespace airrounds;
using session::BeaconMode;
using session::CaptureMode;
using session::ModeResult;
using session::PassiveSession;
using session::Target;

namespace {

constexpr Target target{{2, 0, 0, 0, 0, 1}, 6};
constexpr std::array<uint8_t, 47> beacon{
    0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, 0,    0, 0, 0, 1,
    2,    0, 0, 0, 0,    1,    0,    0,    0,    0,    0, 0,    0, 0, 0, 0,
    100,  0, 1, 0, 0,    3,    'L',  'a',  'b',  1,    1, 0x82, 3, 1, 6,
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};
    return {bytes.begin(), bytes.end()};
}

bool exists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0;
}

std::array<uint8_t, 47> withSsid(char first) {
    auto frame = beacon;
    frame[38] = static_cast<uint8_t>(first);
    return frame;
}

// The same frames at the same times must give the same decisions and the same file.
void testBeaconModeMatchesPassiveSession(const std::string& directory) {
    PassiveSession reference;
    BeaconMode mode;
    CaptureMode& contract = mode;
    contract.reset();
    constexpr uint64_t budgetUs = 10000000;
    require(reference.startAdaptive(target, budgetUs) == session::StartResult::started &&
                contract.begin(target, budgetUs),
            "both visits start");
    require(contract.id() == session::CaptureModeId::beacon &&
                contract.needs().frameTypes == storage::managementFrames,
            "Beacon is mode 00 and needs management frames only");

    struct Delivery {
        std::array<uint8_t, 47> frame;
        uint8_t channel;
        uint64_t elapsedUs;
    };
    auto otherBssid = beacon;
    otherBssid[15] = 9;
    otherBssid[21] = 9;
    const Delivery deliveries[] = {
        {beacon, 11, 100000},        {otherBssid, 6, 200000},     {beacon, 6, 1200000},
        {withSsid('M'), 6, 4500000}, {withSsid('N'), 6, 5400000}, {withSsid('P'), 6, 6300000},
        {withSsid('O'), 6, 6900000}, {beacon, 6, 7100000},
    };
    for (const auto& delivery : deliveries) {
        const auto& frame = delivery.frame;
        require(
            reference.receive(frame.data(), frame.size(), delivery.channel, delivery.elapsedUs) ==
                contract.onFrame(frame.data(), frame.size(), delivery.channel, delivery.elapsedUs),
            "both accept the same frames");
        require(reference.observationDone(delivery.elapsedUs) == contract.done(delivery.elapsedUs),
                "both close the window at the same time");
    }
    // The observation at 6.3 s asks for 7.3 s; only the 7 s cap ends the window.
    require(!contract.done(6999999) && contract.done(7000000),
            "the 7 s cap closes the Beacon window");
    require(contract.finish() == ModeResult::found, "a received beacon is found");

    require(contract.recordCount() == 1, "Beacon saves one record");
    const auto record = contract.record(0);
    require(record.size == beacon.size() &&
                std::equal(beacon.begin(), beacon.end(), record.frame) &&
                record.timestampUs == 1200000,
            "the record is the first accepted beacon");

    const std::string referencePath = directory + "/reference.pcap";
    const std::string modePath = directory + "/mode.pcap";
    require(reference.finish(referencePath.c_str()) == session::VisitState::saved,
            "the reference saves");
    const auto receipt =
        storage::writePcapRecords(modePath.c_str(), contract, contract.needs().frameTypes);
    require(receipt.saved && receipt.bytesWritten == reference.captureStatistics().bytesWritten &&
                receipt.queueOverflows == reference.captureStatistics().queueOverflows,
            "the shared writer saves with the same statistics");
    const auto expected = readFile(referencePath);
    require(!expected.empty() && expected == readFile(modePath) &&
                expected.size() == storage::pcapFileSize(contract),
            "BeaconMode gives the writer the same frame and time as PassiveSession");

    // The bytes the T9 firmware wrote, fixed here so a writer defect cannot hide in both files.
    const std::vector<uint8_t> golden{
        0xd4, 0xc3, 0xb2, 0xa1, 2,    0,    4,    0, 0, 0, 0, 0, 0, 0, 0, 0, // magic, version
        0x00, 0x09, 0,    0,    105,  0,    0,    0,                         // snap 2304, link 105
        1,    0,    0,    0,    0x40, 0x0d, 0x03, 0,                         // 1 s 200000 us
        47,   0,    0,    0,    47,   0,    0,    0}; // captured and real length
    require(expected.size() == golden.size() + beacon.size() &&
                std::equal(golden.begin(), golden.end(), expected.begin()) &&
                std::equal(beacon.begin(), beacon.end(), expected.begin() + 40),
            "the file has the T9 PCAP header and the beacon bytes");
}

// New observations keep asking for more time; the 7 s cap ends the window first.
void testBeaconWindowCap() {
    BeaconMode mode;
    mode.reset();
    require(mode.begin(target, 10000000), "the visit starts");
    const std::array<std::pair<char, uint64_t>, 4> observations{
        {{'L', 1200000}, {'M', 4500000}, {'N', 5400000}, {'P', 6300000}}};
    for (const auto& observation : observations) {
        const auto frame = withSsid(observation.first);
        require(mode.onFrame(frame.data(), frame.size(), 6, observation.second),
                "each new observation is accepted");
    }
    const auto late = withSsid('O');
    require(mode.onFrame(late.data(), late.size(), 6, 6900000) && !mode.done(6999999),
            "a beacon before the cap is accepted");
    require(mode.done(7000000) && !mode.onFrame(beacon.data(), beacon.size(), 6, 7000000),
            "the window closes at the 7 s cap although the last observation asked for 7.3 s");
}

void testQuietBeaconVisit() {
    BeaconMode mode;
    mode.reset();
    require(mode.begin(target, 10000000), "the visit starts");
    require(!mode.done(4999999) && mode.done(5000000), "a quiet visit ends after 5 s");
    require(mode.finish() == ModeResult::not_observed && mode.recordCount() == 0,
            "a quiet visit has no record");
    mode.reset();
    require(mode.begin(target, 10000000), "reset allows the next visit");
}

void testFrameTypes() {
    std::array<uint8_t, 24> frame{};
    frame[0] = 0x80;
    require(storage::frameTypeOf(frame.data(), frame.size()) == storage::managementFrames,
            "a beacon is a management frame");
    require(storage::frameTypeOf(frame.data(), 23) == 0, "a short management frame is refused");
    frame[0] = 0xd4;
    require(storage::frameTypeOf(frame.data(), 10) == storage::controlFrames,
            "an ACK is a control frame");
    frame[0] = 0x08;
    require(storage::frameTypeOf(frame.data(), frame.size()) == storage::dataFrames,
            "a data frame is a data frame");
    frame[0] = 0x0c;
    require(storage::frameTypeOf(frame.data(), frame.size()) == 0,
            "an extension frame has no type bit");
    // cppcheck-suppress knownConditionTrueFalse
    require(storage::frameTypeOf(frame.data(), 9) == 0, "a frame shorter than an ACK is refused");
    // cppcheck-suppress knownConditionTrueFalse
    require(storage::frameTypeOf(nullptr, 24) == 0, "a missing frame is refused");
}

void testWriterKeepsDeclaredTypes(const std::string& directory) {
    std::array<uint8_t, 32> data{};
    data[0] = 0x08;
    const std::string refused = directory + "/refused.pcap";
    const auto receipt = storage::writePcapRecord(refused.c_str(), data.data(), data.size(), 0,
                                                  storage::managementFrames);
    require(!receipt.saved && receipt.bytesWritten == 0 && !exists(refused),
            "a management-only mode cannot save a data frame");

    const std::string admitted = directory + "/admitted.pcap";
    require(
        storage::writePcapRecord(admitted.c_str(), data.data(), data.size(), 0, storage::dataFrames)
            .saved,
        "a mode admitted for data frames saves one");

    std::array<uint8_t, 2400> oversized{};
    oversized[0] = 0x80;
    const std::string large = directory + "/large.pcap";
    require(!storage::writePcapRecord(large.c_str(), oversized.data(), oversized.size(), 0,
                                      storage::managementFrames)
                    .saved &&
                !exists(large),
            "a frame above the snap length is refused");
    require(!storage::writePcapRecord(nullptr, beacon.data(), beacon.size(), 0,
                                      storage::managementFrames)
                 .saved,
            "a missing path is refused");
}

class Chain final : public storage::RecordSource {
public:
    std::vector<std::vector<uint8_t>> frames;
    size_t recordCount() const override { return frames.size(); }
    storage::PcapRecord record(size_t index) const override {
        return {frames[index].data(), frames[index].size(), static_cast<uint32_t>(index * 1500000)};
    }
};

uint32_t le32(const std::vector<uint8_t>& file, size_t offset) {
    uint32_t value = 0;
    for (size_t index = 4; index > 0; --index) {
        value = (value << 8U) | file[offset + index - 1];
    }
    return value;
}

void testChainOfFrames(const std::string& directory) {
    Chain chain;
    chain.frames.emplace_back(beacon.begin(), beacon.end());
    chain.frames.emplace_back(300, uint8_t{0}); // Larger than one queue block.
    chain.frames.back()[0] = 0x88;
    chain.frames.emplace_back(10, uint8_t{0});
    chain.frames.back()[0] = 0xd4;
    constexpr uint8_t allTypes = storage::allFrameTypes;

    const std::string path = directory + "/chain.pcap";
    require(storage::writePcapRecords(path.c_str(), chain, allTypes).saved,
            "a chain of admitted frames saves as one file");
    const auto file = readFile(path);
    require(file.size() == storage::pcapFileSize(chain), "the file holds every record");
    size_t offset = storage::pcapFileHeaderSize;
    for (size_t index = 0; index < chain.frames.size(); ++index) {
        const auto& frame = chain.frames[index];
        require(le32(file, offset) == index * 1500000 / 1000000 &&
                    le32(file, offset + 4) == index * 1500000 % 1000000 &&
                    le32(file, offset + 8) == frame.size() &&
                    std::equal(frame.begin(), frame.end(),
                               file.begin() + static_cast<long>(offset + 16)),
                "each record keeps its order, time, and bytes");
        offset += storage::pcapRecordHeaderSize + frame.size();
    }

    const std::string partial = directory + "/partial.pcap";
    require(!storage::writePcapRecords(partial.c_str(), chain, storage::managementFrames).saved &&
                !exists(partial),
            "one frame of an undeclared type refuses the whole chain");
    Chain empty;
    const std::string none = directory + "/none.pcap";
    require(!storage::writePcapRecords(none.c_str(), empty, allTypes).saved && !exists(none),
            "an empty chain creates no file");
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "pass an empty temporary directory");
    const std::string directory = argv[1];
    testBeaconModeMatchesPassiveSession(directory);
    testBeaconWindowCap();
    testQuietBeaconVisit();
    testFrameTypes();
    testWriterKeepsDeclaredTypes(directory);
    testChainOfFrames(directory);
    std::cout << "capture mode tests passed\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
