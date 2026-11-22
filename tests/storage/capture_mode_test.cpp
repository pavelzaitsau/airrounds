#include "../../firmware/esp32/session/beacon_mode.h"
#include "../../firmware/esp32/session/eapol.h"
#include "../../firmware/esp32/session/eapol_mode.h"

#include <cstring>

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

// Builds one raw EAPOL-Key data frame without its FCS, the form onFrame receives. Addr1, Addr2,
// and Addr3 carry distinct patterns so a BSSID test can tell which address the parser read.
// A constructor with defaults, not default member initializers: a C++11 aggregate may have
// neither, and the tests brace-initialize this with zero to seven fields.
struct EapolParams {
    uint8_t fc0;          // Data frame subtype.
    uint8_t fc1;          // DS bits, Order, Protected.
    uint8_t ethertypeHigh;
    uint8_t ethertypeLow;
    uint8_t packetType;   // 3 is EAPOL-Key.
    uint16_t keyInfo;     // Message 1 default: Key Type and Key ACK.
    size_t keyDataLen;    // Extra key-data bytes beyond the 95 fixed EAPOL-Key fields.

    EapolParams(uint8_t frameControl0 = 0x08, uint8_t frameControl1 = 0x00,
                uint8_t ethertype0 = 0x88, uint8_t ethertype1 = 0x8e, uint8_t packet = 3,
                uint16_t info = 0x0088, size_t keyData = 0)
        : fc0(frameControl0), fc1(frameControl1), ethertypeHigh(ethertype0),
          ethertypeLow(ethertype1), packetType(packet), keyInfo(info), keyDataLen(keyData) {}
};

std::vector<uint8_t> eapolFrame(const EapolParams& params) {
    size_t header = 24;
    if (params.fc0 & 0x80U) {
        header += 2;
        if (params.fc1 & 0x80U) {
            header += 4;
        }
    }
    std::vector<uint8_t> frame(header, 0);
    frame[0] = params.fc0;
    frame[1] = params.fc1;
    for (uint8_t index = 0; index < 6; ++index) {
        frame[4 + index] = static_cast<uint8_t>(0xa0 + index);  // Addr1
        frame[10 + index] = static_cast<uint8_t>(0xb0 + index); // Addr2
        frame[16 + index] = static_cast<uint8_t>(0xc0 + index); // Addr3
    }
    const uint8_t llcSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00,
                                0x00, params.ethertypeHigh, params.ethertypeLow};
    frame.insert(frame.end(), std::begin(llcSnap), std::end(llcSnap));
    const size_t keyFrameLen = 95 + params.keyDataLen; // Fixed EAPOL-Key fields plus key data.
    frame.push_back(2);                                            // EAPOL version.
    frame.push_back(params.packetType);                           // EAPOL packet type.
    frame.push_back(static_cast<uint8_t>(keyFrameLen >> 8U));     // Body length, big-endian.
    frame.push_back(static_cast<uint8_t>(keyFrameLen & 0xffU));
    const size_t bodyStart = frame.size();
    frame.resize(bodyStart + keyFrameLen, 0);
    frame[bodyStart] = 2;                                                // Descriptor type.
    frame[bodyStart + 1] = static_cast<uint8_t>(params.keyInfo >> 8U);   // Key Information, BE.
    frame[bodyStart + 2] = static_cast<uint8_t>(params.keyInfo & 0xffU);
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
    const auto eapol = eapolFrame({});
    require(storage::writePcapRecord(admitted.c_str(), eapol.data(), eapol.size(), 0,
                                     storage::dataFrames)
                .saved,
            "a mode admitted for data frames saves an EAPOL-Key frame");

    const std::string nonEapol = directory + "/non_eapol.pcap";
    require(!storage::writePcapRecord(nonEapol.c_str(), data.data(), data.size(), 0,
                                      storage::dataFrames)
                 .saved &&
                !exists(nonEapol),
            "the writer gate refuses a non-EAPOL data frame before it creates the file");

    const auto protectedEapol = eapolFrame({0x08, 0x40}); // Protected bit set on an EAPOL frame.
    const std::string encrypted = directory + "/protected.pcap";
    require(!storage::writePcapRecord(encrypted.c_str(), protectedEapol.data(),
                                      protectedEapol.size(), 0, storage::dataFrames)
                 .saved &&
                !exists(encrypted),
            "the writer gate refuses a Protected data frame");

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
    // A QoS data EAPOL-Key frame with a 26-byte header, over 256 bytes so it spans queue blocks
    // and passes the writer's EAPOL gate.
    const auto qosEapol = eapolFrame({0x88, 0x00, 0x88, 0x8e, 3, 0x0108, 170});
    require(qosEapol.size() > 256, "the QoS EAPOL record spans more than one queue block");
    chain.frames.emplace_back(qosEapol.begin(), qosEapol.end());
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

// Builds an EAPOL-Key handshake message, with the BSSID and the client address placed per the DS
// direction: messages 1 and 3 go AP to client (FromDS), 2 and 4 go client to AP (ToDS).
std::vector<uint8_t> handshakeFrame(uint8_t message, const uint8_t (&bssid)[6],
                                    const uint8_t (&client)[6], size_t keyDataLen = 0) {
    static const uint16_t keyInfo[4] = {0x0088, 0x0108, 0x0388, 0x0308};
    const bool fromDs = message == 1 || message == 3; // AP to client.
    const auto fc1 = static_cast<uint8_t>(fromDs ? 0x02 : 0x01);
    auto frame = eapolFrame({0x08, fc1, 0x88, 0x8e, 3, keyInfo[message - 1], keyDataLen});
    // FromDS: Addr1 is the client (DA), Addr2 is the BSSID (SA). ToDS: Addr1 is the BSSID (RA),
    // Addr2 is the client (SA). Addr3 is the BSSID in both.
    std::memcpy(frame.data() + 4, fromDs ? client : bssid, 6);
    std::memcpy(frame.data() + 10, fromDs ? bssid : client, 6);
    std::memcpy(frame.data() + 16, bssid, 6);
    return frame;
}

void testEapolParser() {
    using session::classifyKeyInfo;
    using session::eapolBssid;
    using session::eapolHeaderLen;
    using session::isEapolKeyFrame;

    // Key Information to handshake message, Key Type bit required.
    require(classifyKeyInfo(0x0088) == 1, "ACK only is message 1");
    require(classifyKeyInfo(0x0108) == 2, "MIC only is message 2");
    require(classifyKeyInfo(0x0388) == 3, "ACK, MIC, and Secure is message 3");
    require(classifyKeyInfo(0x0308) == 4, "MIC and Secure is message 4");
    require(classifyKeyInfo(0x0080) == 0, "a group rekey message 1 with Key Type clear is no match");
    require(classifyKeyInfo(0x0100) == 0, "a group rekey message 2 with Key Type clear is no match");
    require(classifyKeyInfo(0x0188) == 0, "a WPA1 message 3 with Secure clear is no match");
    require(classifyKeyInfo(0x0008) == 0, "the Key Type bit alone is no match");

    // Header length: base, QoS, QoS plus HT Control, and the Order bit without QoS.
    const uint8_t base[2] = {0x08, 0x00};
    const uint8_t qos[2] = {0x88, 0x00};
    const uint8_t qosHtc[2] = {0x88, 0x80};
    const uint8_t orderNoQos[2] = {0x08, 0x80};
    require(eapolHeaderLen(base) == 24, "a data frame has a 24-byte header");
    require(eapolHeaderLen(qos) == 26, "a QoS data frame adds the 2-byte QoS Control");
    require(eapolHeaderLen(qosHtc) == 30, "a QoS data frame with HT Control adds 4 more");
    require(eapolHeaderLen(orderNoQos) == 24, "the Order bit without QoS adds no HT Control");

    // BSSID by the DS bits: one frame per DS form, and the WDS and short-frame drops.
    const auto fromDs = eapolFrame({0x08, 0x02, 0x88, 0x8e, 3, 0x0088, 0});
    const auto toDs = eapolFrame({0x08, 0x01, 0x88, 0x8e, 3, 0x0088, 0});
    const auto sameBss = eapolFrame({0x08, 0x00, 0x88, 0x8e, 3, 0x0088, 0});
    const auto wds = eapolFrame({0x08, 0x03, 0x88, 0x8e, 3, 0x0088, 0});
    require(eapolBssid(sameBss.data(), sameBss.size()) == sameBss.data() + 16, "same-BSS is Addr3");
    require(eapolBssid(toDs.data(), toDs.size()) == toDs.data() + 4, "client to AP is Addr1");
    require(eapolBssid(fromDs.data(), fromDs.size()) == fromDs.data() + 10, "AP to client is Addr2");
    require(eapolBssid(wds.data(), wds.size()) == nullptr, "a WDS frame has no single BSSID");
    require(eapolBssid(sameBss.data(), 23) == nullptr, "a frame shorter than a header is dropped");
    require(eapolBssid(nullptr, 100) == nullptr, "a missing frame is dropped");

    // A valid EAPOL-Key frame and the forms that are not one.
    const auto key = eapolFrame({});
    const auto eapPacket = eapolFrame({0x08, 0x00, 0x88, 0x8e, 0, 0x0088, 0});
    const auto eapolStart = eapolFrame({0x08, 0x00, 0x88, 0x8e, 1, 0x0088, 0});
    const auto wrongEthertype = eapolFrame({0x08, 0x00, 0x88, 0x00, 3, 0x0088, 0});
    require(isEapolKeyFrame(key.data(), key.size()), "a valid EAPOL-Key frame is recognised");
    require(!isEapolKeyFrame(eapPacket.data(), eapPacket.size()), "an EAP packet is not EAPOL-Key");
    require(!isEapolKeyFrame(eapolStart.data(), eapolStart.size()), "EAPOL-Start is not EAPOL-Key");
    require(!isEapolKeyFrame(wrongEthertype.data(), wrongEthertype.size()),
            "a frame without the 0x888e ethertype is not EAPOL-Key");

    // A read past the end of a short frame: truncate at each parse step; every check holds.
    const size_t header = eapolHeaderLen(key.data());
    require(!isEapolKeyFrame(key.data(), 23), "a frame shorter than a header is refused");
    require(!isEapolKeyFrame(key.data(), header), "a frame with no LLC/SNAP is refused");
    require(!isEapolKeyFrame(key.data(), header + 7), "a frame with a partial ethertype is refused");
    require(!isEapolKeyFrame(key.data(), header + 8),
            "a frame with no EAPOL header is refused");
    require(!isEapolKeyFrame(key.data(), header + 9),
            "a frame with no EAPOL packet type is refused");
    require(isEapolKeyFrame(key.data(), header + 10), "the shortest EAPOL-Key frame is recognised");

    // A wrong LLC/SNAP block, or a wrong ethertype high byte, is not an EAPOL-Key frame.
    auto badLlc = key;
    badLlc[header] = 0x00; // Corrupt the first LLC/SNAP byte (AA -> 00).
    require(!isEapolKeyFrame(badLlc.data(), badLlc.size()), "a wrong LLC/SNAP is not EAPOL-Key");
    const auto wrongEtherHigh = eapolFrame({0x08, 0x00, 0x87, 0x8e, 3, 0x0088, 0});
    require(!isEapolKeyFrame(wrongEtherHigh.data(), wrongEtherHigh.size()),
            "a wrong ethertype high byte is not EAPOL-Key");
}

// The writer's second gate keyed off the declared data type: a data-and-management set with a
// non-EAPOL data record is refused, an EAPOL chain passes, and a Beacon set is unaffected.
void testWriterEapolGate(const std::string& directory) {
    constexpr uint8_t dataAndManagement = storage::dataFrames | storage::managementFrames;

    Chain mixed;
    mixed.frames.emplace_back(beacon.begin(), beacon.end()); // Management, unaffected by the gate.
    std::vector<uint8_t> nonEapolData(40, 0);
    nonEapolData[0] = 0x08; // A data frame without the EAPOL ethertype.
    mixed.frames.emplace_back(nonEapolData.begin(), nonEapolData.end());
    const std::string mixedPath = directory + "/mixed_refused.pcap";
    require(!storage::writePcapRecords(mixedPath.c_str(), mixed, dataAndManagement).saved &&
                !exists(mixedPath),
            "a data-and-management set with a non-EAPOL data record is refused");

    Chain eapolChain;
    const auto message1 = eapolFrame({0x08, 0x02, 0x88, 0x8e, 1, 0x0088, 0});
    const auto message2 = eapolFrame({0x08, 0x01, 0x88, 0x8e, 2, 0x0108, 0});
    eapolChain.frames.emplace_back(message1.begin(), message1.end());
    eapolChain.frames.emplace_back(message2.begin(), message2.end());
    const std::string chainPath = directory + "/eapol_chain.pcap";
    require(storage::writePcapRecords(chainPath.c_str(), eapolChain, storage::dataFrames).saved,
            "an EAPOL chain passes the gate");

    Chain beaconSet;
    beaconSet.frames.emplace_back(beacon.begin(), beacon.end());
    const std::string beaconPath = directory + "/beacon_set.pcap";
    require(storage::writePcapRecords(beaconPath.c_str(), beaconSet, storage::managementFrames).saved,
            "a Beacon set is unaffected by the EAPOL gate");

    // A data frame with a well-formed LLC/SNAP but a non-EAPOL ethertype (IPv4 0x0800) is refused
    // by the writer gate, so a mode parser defect cannot save another data frame.
    const auto ipData = eapolFrame({0x08, 0x02, 0x08, 0x00, 3, 0x0088, 0});
    const std::string ipPath = directory + "/ip_data.pcap";
    require(!storage::writePcapRecord(ipPath.c_str(), ipData.data(), ipData.size(), 0,
                                      storage::dataFrames)
                 .saved &&
                !exists(ipPath),
            "the writer gate refuses a data frame whose ethertype is not EAPOL");
}

void testEapolMode(const std::string& directory) {
    using session::EapolMode;
    const uint8_t bssid[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t other[6] = {2, 0, 0, 0, 0, 9};
    const uint8_t clientA[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t clientB[6] = {0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    const Target tgt{{2, 0, 0, 0, 0, 1}, 6};
    constexpr uint64_t budgetUs = 60000000;

    EapolMode mode;
    require(mode.id() == session::CaptureModeId::eapol &&
                mode.needs().frameTypes == storage::dataFrames,
            "EAPOL is mode 01 and needs data frames only");

    // Accept a valid message 2 for the target, and save it.
    require(mode.begin(tgt, budgetUs), "the visit starts");
    const auto message2 = handshakeFrame(2, bssid, clientA);
    require(mode.onFrame(message2.data(), message2.size(), 6, 100000), "a message 2 is accepted");
    require(mode.recordCount() == 1 && mode.finish() == ModeResult::found,
            "one held frame is found");
    const std::string one = directory + "/eapol_one.pcap";
    require(storage::writePcapRecords(one.c_str(), mode, mode.needs().frameTypes).saved,
            "a single EAPOL frame saves");

    // Drop a frame whose BSSID is not the target, and one on another channel.
    mode.reset();
    const auto wrongBssid = handshakeFrame(2, other, clientA);
    require(!mode.onFrame(wrongBssid.data(), wrongBssid.size(), 6, 100000),
            "a frame for another BSSID is dropped");
    require(!mode.onFrame(message2.data(), message2.size(), 7, 100000),
            "a frame on another channel is dropped");

    // Each DS form that carries the target BSSID is accepted; a WDS frame is dropped.
    mode.reset();
    const auto fromDs = handshakeFrame(1, bssid, clientA); // AP to client, BSSID is Addr2.
    const auto toDs = handshakeFrame(2, bssid, clientA);   // client to AP, BSSID is Addr1.
    auto sameBss = eapolFrame({0x08, 0x00, 0x88, 0x8e, 3, 0x0388, 0});
    std::memcpy(sameBss.data() + 16, bssid, 6); // Same-BSS data, BSSID is Addr3.
    auto wds = eapolFrame({0x08, 0x03, 0x88, 0x8e, 2, 0x0108, 0});
    require(mode.onFrame(fromDs.data(), fromDs.size(), 6, 1000), "a FromDS frame is accepted");
    require(mode.onFrame(toDs.data(), toDs.size(), 6, 2000), "a ToDS frame is accepted");
    require(mode.onFrame(sameBss.data(), sameBss.size(), 6, 3000), "a same-BSS frame is accepted");
    require(!mode.onFrame(wds.data(), wds.size(), 6, 4000), "a WDS frame is dropped");

    // Refuse the forms that are not an own-BSSID pairwise EAPOL-Key message.
    mode.reset();
    auto patched = [&](EapolParams params) {
        auto frame = eapolFrame(params);
        std::memcpy(frame.data() + 10, bssid, 6); // FromDS BSSID is Addr2.
        return frame;
    };
    const auto nonKey = patched({0x08, 0x02, 0x88, 0x8e, 0, 0x0088, 0});
    const auto wrongType = patched({0x08, 0x02, 0x88, 0x00, 3, 0x0088, 0});
    const auto groupRekey = patched({0x08, 0x02, 0x88, 0x8e, 3, 0x0080, 0});
    const auto wpa1Three = patched({0x08, 0x02, 0x88, 0x8e, 3, 0x0188, 0});
    const auto protectedFrame = patched({0x08, 0x42, 0x88, 0x8e, 3, 0x0088, 0}); // FromDS + Protected.
    require(!mode.onFrame(nonKey.data(), nonKey.size(), 6, 1000), "a non-Key EAPOL type is refused");
    require(!mode.onFrame(wrongType.data(), wrongType.size(), 6, 1000),
            "a frame without the EAPOL ethertype is refused");
    require(!mode.onFrame(groupRekey.data(), groupRekey.size(), 6, 1000),
            "a group rekey is refused");
    require(!mode.onFrame(wpa1Three.data(), wpa1Three.size(), 6, 1000),
            "a WPA1 message 3 is refused");
    require(!mode.onFrame(protectedFrame.data(), protectedFrame.size(), 6, 1000),
            "a Protected data frame is refused");
    require(!mode.onFrame(beacon.data(), beacon.size(), 6, 1000), "a management frame is refused");
    require(mode.recordCount() == 0 && mode.finish() == ModeResult::not_observed,
            "no refused frame was held");

    // Accept a 512-byte message 3 with a full key-data block; drop a frame one byte over.
    mode.reset();
    const auto full = handshakeFrame(3, bssid, clientA, 381); // 131 fixed bytes + 381 = 512.
    require(full.size() == EapolMode::maxEapolFrameSize, "the frame is exactly the buffer size");
    require(mode.onFrame(full.data(), full.size(), 6, 1000), "a 512-byte message 3 is accepted");
    const auto over = handshakeFrame(3, bssid, clientA, 382); // 513 bytes.
    require(over.size() == EapolMode::maxEapolFrameSize + 1 &&
                !mode.onFrame(over.data(), over.size(), 6, 2000),
            "a frame one byte over the buffer is dropped, not truncated");

    // Hold messages 1 to 4 in capture order, including a repeated message 1 after message 2.
    mode.reset();
    const auto m1 = handshakeFrame(1, bssid, clientA);
    const auto m2 = handshakeFrame(2, bssid, clientA);
    const auto m3 = handshakeFrame(3, bssid, clientA);
    const auto m4 = handshakeFrame(4, bssid, clientA);
    require(mode.onFrame(m1.data(), m1.size(), 6, 100000), "message 1 held");
    require(mode.onFrame(m2.data(), m2.size(), 6, 200000), "message 2 held");
    require(mode.onFrame(m1.data(), m1.size(), 6, 300000), "a repeated message 1 overwrites");
    require(mode.onFrame(m3.data(), m3.size(), 6, 400000), "message 3 held");
    require(mode.onFrame(m4.data(), m4.size(), 6, 500000), "message 4 held");
    require(mode.recordCount() == 4, "all four messages are held");
    const uint32_t expectedOrder[4] = {200000, 300000, 400000, 500000};
    for (size_t index = 0; index < 4; ++index) {
        require(mode.record(index).timestampUs == expectedOrder[index],
                "records come in capture order, the repeated message 1 after message 2");
    }
    const std::string chain = directory + "/eapol_full.pcap";
    require(storage::writePcapRecords(chain.c_str(), mode, mode.needs().frameTypes).saved,
            "the writer saves the chain as one file");
    const auto file = readFile(chain);
    require(file.size() == storage::pcapFileSize(mode), "the file holds every record");
    uint32_t previous = 0;
    size_t offset = storage::pcapFileHeaderSize;
    for (size_t index = 0; index < 4; ++index) {
        const uint32_t seconds = le32(file, offset);
        const uint32_t micros = le32(file, offset + 4);
        const uint32_t stamp = seconds * 1000000U + micros;
        require(stamp >= previous, "the PCAP timestamps are monotonic");
        previous = stamp;
        offset += storage::pcapRecordHeaderSize + le32(file, offset + 8);
    }

    // Stop early on messages 3 and 4 from one client; keep listening on a cross-client pair.
    mode.reset();
    require(mode.onFrame(m3.data(), m3.size(), 6, 1000000) &&
                mode.onFrame(m4.data(), m4.size(), 6, 2000000),
            "messages 3 and 4 from one client are held");
    require(mode.done(2000000), "the visit stops early on a same-client 3-and-4 pair");

    mode.reset();
    const auto m4b = handshakeFrame(4, bssid, clientB);
    require(mode.onFrame(m3.data(), m3.size(), 6, 1000000) &&
                mode.onFrame(m4b.data(), m4b.size(), 6, 2000000),
            "message 3 to client A and message 4 from client B are held");
    require(!mode.done(2000000), "a cross-client 3-and-4 pair does not stop early");
    require(!mode.done(56999999) && mode.done(57000000),
            "without a join the visit stops 3 s before the share ends");

    // With only message 1 and time still left, the visit keeps listening (no early stop).
    mode.reset();
    require(mode.onFrame(m1.data(), m1.size(), 6, 1000), "message 1 held for the keep-listening check");
    require(!mode.done(2000000), "with only message 1 and budget left the visit keeps listening");

    // A frame cut off before the key-info bytes is dropped, not read past its end.
    mode.reset();
    const size_t shortLen = session::eapolHeaderLen(m1.data()) + 8 + 5;
    require(shortLen < m1.size() && !mode.onFrame(m1.data(), shortLen, 6, 1000),
            "a frame cut before the key-info bytes is dropped");

    // A lone message 1 is a success; an empty visit is not observed.
    mode.reset();
    require(mode.onFrame(m1.data(), m1.size(), 6, 1000), "a lone message 1 is held");
    require(mode.finish() == ModeResult::found, "a lone message 1 maps to found");
    mode.reset();
    require(mode.finish() == ModeResult::not_observed && mode.recordCount() == 0,
            "an empty visit is not observed");

    // A complete one-client handshake to eapol.pcap, for the Mac-check gate in
    // tools/quality/check.py. The sealing writer also writes eapol.pcap.ok.
    mode.reset();
    require(mode.onFrame(m1.data(), m1.size(), 6, 100000) &&
                mode.onFrame(m2.data(), m2.size(), 6, 200000) &&
                mode.onFrame(m3.data(), m3.size(), 6, 300000) &&
                mode.onFrame(m4.data(), m4.size(), 6, 400000),
            "a full handshake is held for eapol.pcap");
    require(storage::writePcapRecords((directory + "/eapol.pcap").c_str(), mode,
                                      mode.needs().frameTypes)
                .saved,
            "the handshake saves to eapol.pcap");
}

// A1: the round-scoped transmit budget, the mode context, and the default poll().
void testTransmitContext() {
    using session::ModeContext;
    using session::TransmitBudget;
    using session::Transmitter;

    TransmitBudget budget;
    require(budget.remaining() == 0, "an unseeded budget is empty");
    require(!budget.tryConsume(), "an unseeded budget transmits nothing");
    budget.seed(3);
    require(budget.remaining() == 3, "seed sets the frame count");
    require(budget.tryConsume() && budget.tryConsume() && budget.tryConsume(),
            "the budget spends its three frames");
    require(!budget.tryConsume(), "a spent budget refuses the next frame");
    require(budget.remaining() == 0, "a spent budget is empty");
    budget.seed(2);
    require(budget.remaining() == 2, "a re-seed resets the budget");

    struct Counter final : Transmitter {
        unsigned sends = 0;
        bool result = true;
        bool send(const uint8_t*, size_t) override {
            ++sends;
            return result;
        }
    };
    const uint8_t frame[4] = {0xc0, 0, 0, 7};

    ModeContext inert{nullptr, &budget};
    require(!inert.transmit(frame, sizeof(frame)) && budget.remaining() == 2,
            "a context without a transmitter never transmits and spends no budget");

    Counter counter;
    ModeContext context{&counter, &budget};
    require(context.transmit(frame, sizeof(frame)) && context.transmit(frame, sizeof(frame)),
            "the context transmits up to the budget");
    require(!context.transmit(frame, sizeof(frame)) && counter.sends == 2,
            "the cap refuses the frame beyond the budget and never reaches the transmitter");

    budget.seed(1);
    counter.result = false;
    require(!context.transmit(frame, sizeof(frame)), "a failed send returns false");
    require(counter.sends == 3 && budget.remaining() == 0,
            "a failed send still counts against the cap");
    require(!context.transmit(frame, sizeof(frame)) && counter.sends == 3,
            "the spent budget stops further transmits");

    ModeContext noBudget{&counter, nullptr};
    require(!noBudget.transmit(frame, sizeof(frame)) && counter.sends == 3,
            "a context without a budget never transmits");

    // The default poll() does nothing: a passive mode neither transmits nor changes state.
    BeaconMode passive;
    passive.reset();
    require(passive.begin(target, 10000000), "the passive visit starts");
    TransmitBudget pollBudget;
    pollBudget.seed(5);
    Counter pollCounter;
    ModeContext pollContext{&pollCounter, &pollBudget};
    passive.poll(1000, pollContext);
    require(pollCounter.sends == 0 && pollBudget.remaining() == 5,
            "the default poll() transmits nothing");
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "pass an empty temporary directory");
    const std::string directory = argv[1];
    testTransmitContext();
    testBeaconModeMatchesPassiveSession(directory);
    testBeaconWindowCap();
    testQuietBeaconVisit();
    testFrameTypes();
    testWriterKeepsDeclaredTypes(directory);
    testChainOfFrames(directory);
    testEapolParser();
    testWriterEapolGate(directory);
    testEapolMode(directory);
    std::cout << "capture mode tests passed\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
