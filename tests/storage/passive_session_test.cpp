#include "../../firmware/esp32/session/passive_session.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace airrounds;
using session::PassiveSession;
using session::StartResult;
using session::Target;
using session::VisitState;

namespace {

constexpr Target target{{2, 0, 0, 0, 0, 1}, 6};
constexpr Target other{{2, 0, 0, 0, 0, 2}, 6};
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

void testBeaconDiscovery() {
    session::BeaconDiscovery discovery("Lab", 3);
    auto unrelated = beacon;
    unrelated[38] = 'X';
    discovery.receive(unrelated.data(), unrelated.size(), target.channel);
    discovery.receive(beacon.data(), beacon.size(), 11);
    require(discovery.target().channel == 0, "discovery ignores another SSID or channel");

    discovery.receive(beacon.data(), beacon.size(), target.channel);
    discovery.receive(beacon.data(), beacon.size(), target.channel);
    require(!discovery.ambiguous() && discovery.target().channel == target.channel &&
                memcmp(discovery.target().bssid, target.bssid, sizeof(target.bssid)) == 0,
            "discovery selects the BSSID and channel of one matching access point");

    auto sameName = beacon;
    sameName[15] = other.bssid[5];
    sameName[21] = other.bssid[5];
    discovery.receive(sameName.data(), sameName.size(), target.channel);
    require(discovery.ambiguous(), "discovery detects two access points with the same SSID");
}

void testEmptyList(const std::string& directory) {
    const auto path = directory + "/empty.pcap";
    PassiveSession visit;
    require(visit.start(nullptr, 0) == StartResult::empty_list, "reject an empty list");
    require(visit.start(&target, 0) == StartResult::empty_list, "ignore targets beyond the list");
    visit.receive(beacon.data(), beacon.size(), target.channel, 0);
    require(!visit.hasSample(), "an empty list cannot admit a beacon");
    require(visit.finish(path.c_str()) == VisitState::idle, "empty list leaves visit idle");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::missing,
            "empty list leaves no file or receipt");
    require(visit.start(nullptr, 1) == StartResult::invalid_target, "reject missing target data");
    const Target multiple[]{target, other};
    require(visit.start(multiple, 2) == StartResult::invalid_target, "reject multiple targets");
    const Target wrongChannel{{2, 0, 0, 0, 0, 1}, 0};
    require(visit.start(&wrongChannel, 1) == StartResult::invalid_target, "reject channel zero");
}

void testFilteredCapture(const std::string& directory) {
    const auto path = directory + "/passive.pcap";
    auto selected = target;
    PassiveSession visit;
    require(visit.start(&selected, 1) == StartResult::started, "start on one target");
    selected = other;
    selected.channel = 11;
    require(visit.channel() == target.channel, "keep a copy of the fixed channel");
    require(visit.start(&other, 1) == StartResult::busy, "keep target fixed during the visit");

    auto neighbor = beacon;
    neighbor[21] = other.bssid[5];
    neighbor[15] = other.bssid[5];
    visit.receive(neighbor.data(), neighbor.size(), target.channel, 1);
    visit.receive(beacon.data(), beacon.size(), 11, 1);

    auto wrongAdvertisement = beacon;
    wrongAdvertisement.back() = 11;
    visit.receive(wrongAdvertisement.data(), wrongAdvertisement.size(), target.channel, 1);
    visit.receive(beacon.data(), beacon.size() - 1, target.channel, 1);

    auto data = beacon;
    data[0] = 0x08;
    visit.receive(data.data(), data.size(), target.channel, 1);
    require(!visit.hasSample(), "reject off-target, off-channel, malformed and data frames");

    visit.receive(beacon.data(), beacon.size(), target.channel, PassiveSession::windowUs);
    require(!visit.hasSample(), "reject a frame at the deadline");
    visit.receive(beacon.data(), beacon.size(), target.channel, 42);
    auto repeat = beacon;
    repeat[38] = 'X';
    visit.receive(repeat.data(), repeat.size(), target.channel, 43);
    require(visit.hasSample(), "accept a matching beacon");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::missing,
            "receiving the beacon does not save a file");
    require(visit.finish(path.c_str()) == VisitState::saved, "save only after T2 verification");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::saved,
            "reopen the sealed capture independently");
    require(visit.finish(path.c_str()) == VisitState::saved, "finish returns the same result");

    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), {}};
    const uint8_t header[]{
        0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  9, 0, 0,
        105,  0,    0,    0,    0, 0, 0, 0, 42, 0, 0, 0, 47, 0, 0, 0, 47, 0, 0, 0,
    };
    std::vector<uint8_t> expected{std::begin(header), std::end(header)};
    expected.insert(expected.end(), beacon.begin(), beacon.end());
    require(saved == expected, "PCAP contains exactly the first matching beacon");
    require(visit.savedPcapSize() == expected.size(), "report the verified file length");
}

void testCaptureAcrossBlocks(const std::string& directory) {
    const auto path = directory + "/large-beacon.pcap";
    std::vector<uint8_t> large(beacon.begin(), beacon.end());
    large.push_back(221);
    large.push_back(255);
    large.insert(large.end(), 255, 0xab);

    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a large-beacon visit");
    visit.receive(large.data(), large.size(), target.channel, 1234567);
    require(visit.savedPcapSize() == 0, "report no file length until save succeeds");
    require(visit.finish(path.c_str()) == VisitState::saved, "save a beacon across blocks");
    require(visit.savedPcapSize() == 40 + large.size(), "count all PCAP bytes");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::saved,
            "verify the closed large-beacon PCAP and receipt");

    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), {}};
    require(saved.size() == visit.savedPcapSize(), "write the expected number of bytes");
    require(storage::pcap::readNumber(saved.data() + 24, 4, true) == 1 &&
                storage::pcap::readNumber(saved.data() + 28, 4, true) == 234567,
            "encode seconds and microseconds in the PCAP timestamp");
    require(std::equal(large.begin(), large.end(), saved.begin() + 40),
            "preserve the beacon across the block boundary");
}

void testQuietAndWriteFailure(const std::string& directory) {
    const auto quietPath = directory + "/quiet.pcap";
    PassiveSession quiet;
    require(quiet.start(&target, 1) == StartResult::started, "start quiet visit");
    require(quiet.finish(quietPath.c_str()) == VisitState::not_observed,
            "a quiet visit ends without a saved result");
    quiet.receive(beacon.data(), beacon.size(), target.channel, 0);
    require(!quiet.hasSample(), "receive is inactive after finish");
    require(storage::recoverPcap(quietPath.c_str()) == storage::CaptureState::missing,
            "quiet visit leaves no PCAP");

    PassiveSession failed;
    require(failed.start(&target, 1) == StartResult::started, "start write failure visit");
    failed.receive(beacon.data(), beacon.size(), target.channel, 0);
    require(failed.finish((directory + "/missing/capture.pcap").c_str()) ==
                VisitState::storage_error,
            "a matching frame cannot turn an open failure into saved");
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "pass a test directory");
    testBeaconDiscovery();
    testEmptyList(argv[1]);
    testFilteredCapture(argv[1]);
    testCaptureAcrossBlocks(argv[1]);
    testQuietAndWriteFailure(argv[1]);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
