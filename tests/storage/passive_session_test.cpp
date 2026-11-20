#include "../../firmware/esp32/session/passive_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <csignal>
#include <cstring>
#include <dirent.h>
#include <sys/uio.h>
#include <unistd.h>

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

void require(bool condition, const std::string& message) {
    require(condition, message.c_str());
}

// Limits for the write() calls made by this binary; the defaults pass every byte.
struct WriteFault {
    size_t chunk = SIZE_MAX;  // Largest byte count one call accepts.
    size_t budget = SIZE_MAX; // Bytes accepted before every later call fails with EIO.
    size_t shortCalls = 0;    // Calls that returned fewer bytes than requested.
};

WriteFault writeFault;

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
    writeFault.chunk = SIZE_MAX; // Let the report through the write() hook below.
    writeFault.budget = SIZE_MAX;
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

} // namespace

// PassiveSession saves through SdFile, which calls write(). Defining write() here replaces the
// C library function for this binary, so a test can shorten or fail one save's writes without
// a file seam in the production code.
extern "C" ssize_t write(int descriptor, const void* data, size_t size) {
    if (writeFault.budget == 0) {
        errno = EIO; // NOLINT(bugprone-signal-handler): the watchdog resets the budget first.
        return -1;
    }
    size_t count = size < writeFault.chunk ? size : writeFault.chunk;
    count = count < writeFault.budget ? count : writeFault.budget;
    iovec part{const_cast<void*>(data), count};
    // NOLINTNEXTLINE(bugprone-signal-handler): one system call, safe for the watchdog report.
    const ssize_t written = writev(descriptor, &part, 1);
    if (written > 0 && writeFault.budget != SIZE_MAX) {
        writeFault.budget -= static_cast<size_t>(written);
    }
    if (written >= 0 && static_cast<size_t>(written) < size) {
        ++writeFault.shortCalls;
    }
    return written;
}

namespace {

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
    discovery.receive(beacon.data(), beacon.size(), target.channel);
    require(discovery.ambiguous(), "a later beacon does not clear the ambiguity");

    session::BeaconDiscovery prefix("La", 2);
    prefix.receive(beacon.data(), beacon.size(), target.channel);
    require(prefix.target().channel == 0, "discovery needs the whole SSID, not a prefix");

    session::BeaconDiscovery moved("Lab", 3);
    auto otherChannel = beacon;
    otherChannel.back() = 11;
    moved.receive(beacon.data(), beacon.size(), target.channel);
    moved.receive(otherChannel.data(), otherChannel.size(), 11);
    require(moved.ambiguous(), "discovery detects one BSSID on two channels");
}

void testEmptyList(const std::string& directory) {
    const auto path = directory + "/empty.pcap";
    PassiveSession visit;
    require(visit.start(nullptr, 0) == StartResult::empty_list, "reject an empty list");
    require(visit.start(&target, 0) == StartResult::empty_list,
            "a zero count is an empty list even with target data");
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

    visit.receive(beacon.data(), 41, target.channel, 1); // Missing Supported Rates.
    auto reversed = beacon;
    reversed[36] = 1;
    reversed[41] = 0;
    visit.receive(reversed.data(), reversed.size(), target.channel, 1);
    auto emptyRates = beacon;
    emptyRates[42] = 0;
    visit.receive(emptyRates.data(), 43, target.channel, 1);

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
    require(visit.finish(path.c_str()) == VisitState::saved, "save the accepted beacon");
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
    require(visit.captureStatistics().bytesWritten == expected.size() &&
                visit.captureStatistics().queueOverflows == 0,
            "report the bytes storage accepted");
}

void testObservationWindow(const std::string& directory) {
    constexpr uint64_t base = 5000000;
    constexpr uint64_t tail = 1000000;
    constexpr uint64_t limit = 7000000;
    PassiveSession visit;
    require(visit.startAdaptive(target, limit) == StartResult::started, "start an adaptive visit");

    visit.receive(beacon.data(), beacon.size(), target.channel, base - 1);
    require(visit.hasSample() && !visit.observationDone(base),
            "a late first beacon extends the visit");

    auto repeat = beacon;
    repeat[22] = 0xf0; // Sequence number.
    repeat[24] = 123;  // Beacon timestamp.
    repeat[41] = 0x84;
    visit.receive(repeat.data(), repeat.size(), target.channel, base + tail - 2);
    require(visit.observationDone(base + tail - 1),
            "sequence, timestamp and rate changes do not extend the visit");

    visit.reset();
    require(visit.startAdaptive(target, limit) == StartResult::started,
            "reset the novelty history");
    visit.receive(beacon.data(), beacon.size(), target.channel, 1);
    require(!visit.observationDone(base - 1), "an early beacon leaves the base window in place");
    auto renamed = beacon;
    renamed[38] = 'X';
    visit.receive(renamed.data(), renamed.size(), target.channel, base - 1);
    require(!visit.observationDone(base), "a new SSID extends the visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, base + tail - 2);
    require(visit.observationDone(base + tail - 1), "a return to an older value does not extend");

    const auto path = directory + "/adaptive.pcap";
    require(visit.finish(path.c_str()) == VisitState::saved, "save an adaptive visit");
    std::ifstream input(path, std::ios::binary);
    input.seekg(40);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), {}};
    require(saved.size() == beacon.size() &&
                std::equal(beacon.begin(), beacon.end(), saved.begin()),
            "a later observation does not replace the first beacon");

    visit.reset();
    require(visit.startAdaptive(target, limit) == StartResult::started,
            "start a visit for changes");
    visit.receive(beacon.data(), beacon.size(), target.channel, base - 1);
    auto rescheduled = beacon;
    rescheduled[32] = 102;
    visit.receive(rescheduled.data(), rescheduled.size(), target.channel, base + tail - 2);
    require(!visit.observationDone(base + tail), "a new beacon interval extends the visit");
    auto capabilities = rescheduled;
    capabilities[34] = 0x11;
    visit.receive(capabilities.data(), capabilities.size(), target.channel, limit - 10);
    require(!visit.observationDone(limit - 1), "a new capability can extend to the hard limit");
    require(visit.observationDone(limit), "continued changes cannot exceed the hard limit");
}

void testObservationBounds() {
    constexpr uint64_t base = 5000000;
    PassiveSession visit;
    require(visit.startAdaptive(target, 3000000) == StartResult::started,
            "start a shortened visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, 2999999);
    require(visit.hasSample() && visit.observationDone(3000000), "respect the shorter round share");

    visit.reset();
    require(visit.startAdaptive(target, base + 2000000) == StartResult::started,
            "start a quiet visit");
    auto neighbor = beacon;
    neighbor[15] = other.bssid[5];
    neighbor[21] = other.bssid[5];
    visit.receive(neighbor.data(), neighbor.size(), target.channel, base - 2);
    visit.receive(beacon.data(), beacon.size() - 1, target.channel, base - 2);
    visit.receive(beacon.data(), beacon.size(), 11, base - 2);
    visit.receive(beacon.data(), beacon.size(), target.channel, base);
    require(!visit.hasSample() && visit.observationDone(base),
            "ignore invalid, off-target and late frames");

    visit.reset();
    require(visit.startAdaptive(target, base + 2000000) == StartResult::started,
            "start a busy visit");
    auto changes = beacon;
    for (uint8_t interval = 100; interval < 104; ++interval) {
        changes[32] = interval;
        visit.receive(changes.data(), changes.size(), target.channel, interval);
    }
    changes[32] = 104;
    visit.receive(changes.data(), changes.size(), target.channel, base - 2);
    visit.receive(beacon.data(), beacon.size(), target.channel, base - 1);
    require(visit.observationDone(base),
            "a full history does not forget repeats or extend further");
}

// The fixture plus one vendor element, so the beacon spans two queue blocks.
std::vector<uint8_t> largeBeacon() {
    std::vector<uint8_t> large(beacon.begin(), beacon.end());
    large.push_back(221);
    large.push_back(255);
    large.insert(large.end(), 255, 0xab);
    return large;
}

void testCaptureAcrossBlocks(const std::string& directory) {
    const auto path = directory + "/large-beacon.pcap";
    const auto large = largeBeacon();
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

// Pads the fixture with vendor elements until the frame has exactly `size` bytes.
std::vector<uint8_t> paddedBeacon(size_t size) {
    std::vector<uint8_t> frame(beacon.begin(), beacon.end());
    while (frame.size() < size) {
        const size_t remaining = size - frame.size();
        size_t length = remaining - 2 < 255 ? remaining - 2 : 255;
        if (remaining - (length + 2) == 1) {
            --length;
        }
        frame.push_back(221);
        frame.push_back(static_cast<uint8_t>(length));
        frame.insert(frame.end(), length, 0xab);
    }
    return frame;
}

void testBeaconSizeLimit(const std::string& directory) {
    const auto oversized = paddedBeacon(session::maxBeaconSize + 1);
    require(oversized.size() == session::maxBeaconSize + 1,
            "build a frame one byte over the limit");
    PassiveSession rejected;
    require(rejected.start(&target, 1) == StartResult::started, "start an oversized-frame visit");
    require(!rejected.receive(oversized.data(), oversized.size(), target.channel, 1) &&
                !rejected.hasSample(),
            "reject a frame larger than the sample buffer");

    const auto path = directory + "/largest-beacon.pcap";
    const auto largest = paddedBeacon(session::maxBeaconSize);
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a largest-frame visit");
    require(visit.receive(largest.data(), largest.size(), target.channel, 1) && visit.hasSample(),
            "accept a frame that fills the sample buffer");
    require(visit.finish(path.c_str()) == VisitState::saved &&
                visit.savedPcapSize() == 40 + session::maxBeaconSize,
            "save the largest accepted frame");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::saved,
            "verify the largest-frame PCAP and receipt");
}

void testAdaptiveCap() {
    constexpr uint64_t base = 5000000;
    constexpr uint64_t tail = 1000000;
    constexpr uint64_t cap = PassiveSession::maxAdaptiveWindowUs;
    PassiveSession visit;
    require(visit.startAdaptive(target, 10000000) == StartResult::started,
            "start a visit with a 10 s round share");
    auto changing = beacon;
    require(visit.receive(changing.data(), changing.size(), target.channel, base - 1),
            "accept a late first beacon");
    changing[32] = 101; // A new beacon interval is a new observation.
    require(visit.receive(changing.data(), changing.size(), target.channel, base + tail - 2),
            "accept a changed beacon in the extension");
    changing[32] = 102;
    require(visit.receive(changing.data(), changing.size(), target.channel, cap - 3),
            "accept a changed beacon just before the cap");
    require(!visit.observationDone(cap - 1) && visit.observationDone(cap),
            "a larger round share still stops listening at the 7 s cap");
}

using Frame = std::vector<uint8_t>;

Frame fixture() {
    return Frame(beacon.begin(), beacon.end());
}

Frame withByte(size_t offset, uint8_t value) {
    auto frame = fixture();
    frame[offset] = value;
    return frame;
}

// Sets the transmitter and BSSID together, so only the address value differs.
Frame withBssid(const std::array<uint8_t, 6>& bssid) {
    auto frame = fixture();
    std::copy(bssid.begin(), bssid.end(), frame.begin() + 10);
    std::copy(bssid.begin(), bssid.end(), frame.begin() + 16);
    return frame;
}

Frame withSsidSize(size_t size) {
    auto frame = fixture();
    frame[37] = static_cast<uint8_t>(size);
    frame.erase(frame.begin() + 38, frame.begin() + 41);
    frame.insert(frame.begin() + 38, size, 'x');
    return frame;
}

Frame withRateCount(size_t count) {
    auto frame = fixture();
    frame[42] = static_cast<uint8_t>(count);
    frame.erase(frame.begin() + 43);
    frame.insert(frame.begin() + 43, count, 0x82);
    return frame;
}

// Appends an HT Operation element whose primary channel matches the fixture.
Frame withHtOperation(size_t length) {
    auto frame = fixture();
    frame.push_back(61);
    frame.push_back(static_cast<uint8_t>(length));
    frame.push_back(target.channel);
    frame.insert(frame.end(), length - 1, 0);
    return frame;
}

Frame withAppended(std::initializer_list<uint8_t> element) {
    auto frame = fixture();
    frame.insert(frame.end(), element);
    return frame;
}

bool sessionAccepts(const Frame& frame) {
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a filter visit");
    return visit.receive(frame.data(), frame.size(), target.channel, 1) && visit.hasSample();
}

bool parserAccepts(const Frame& frame) {
    session::BeaconView view;
    return session::parseBeacon(target.channel, frame.data(), frame.size(), view);
}

void testAcceptedBeaconShapes() {
    const std::vector<std::pair<const char*, Frame>> cases{
        {"accept the unmodified fixture", fixture()},
        {"accept a hidden SSID", withSsidSize(0)},
        {"accept a 32-byte SSID", withSsidSize(32)},
        {"accept eight Supported Rates", withRateCount(8)},
        {"accept a 22-byte HT Operation on the visit channel", withHtOperation(22)},
    };
    for (const auto& item : cases) {
        require(parserAccepts(item.second) && sessionAccepts(item.second), item.first);
    }
}

struct RejectedBeacon {
    const char* description;
    Frame frame;
    bool reachesFilter; // False when the visit rejects the BSSID before it parses the frame.
};

void testRejectedBeaconShapes() {
    auto dsLength = withAppended({0});
    dsLength[45] = 2;
    auto htChannel = withHtOperation(22);
    htChannel[49] = 11;

    const std::vector<RejectedBeacon> cases{
        {"reject a beacon sent to the distribution system", withByte(1, 0x01), true},
        {"reject a beacon from the distribution system", withByte(1, 0x02), true},
        {"reject a beacon with an HT Control field", withByte(1, 0x80), true},
        {"reject a beacon with more fragments", withByte(1, 0x04), true},
        {"reject a protected beacon", withByte(1, 0x40), true},
        {"reject a beacon fragment", withByte(22, 0x01), true},
        {"reject a beacon with a unicast destination", withByte(4, 0x02), true},
        {"reject a multicast BSSID", withBssid({{3, 0, 0, 0, 0, 1}}), false},
        {"reject a transmitter that differs from the BSSID", withByte(15, 2), true},
        {"reject a first element that is not an SSID", withByte(36, 221), true},
        {"reject a 33-byte SSID", withSsidSize(33), true},
        {"reject a second element that is not Supported Rates", withByte(41, 50), true},
        {"reject empty Supported Rates", withRateCount(0), true},
        {"reject nine Supported Rates", withRateCount(9), true},
        {"reject a second SSID element", withAppended({0, 3, 'L', 'a', 'b'}), true},
        {"reject a DS Parameter element of two bytes", dsLength, true},
        {"reject a DS Parameter channel that differs from the visit", withByte(46, 11), true},
        {"reject a 21-byte HT Operation element", withHtOperation(21), true},
        {"reject an HT Operation channel that differs from the visit", htChannel, true},
        {"reject an all-zero BSSID", withBssid({{0, 0, 0, 0, 0, 0}}), false},
        {"reject a lone byte after the last element", withAppended({221}), true},
    };
    for (const auto& item : cases) {
        require(!parserAccepts(item.frame), item.description);
        require(!item.reachesFilter || !sessionAccepts(item.frame), item.description);
    }
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

void testResetAfterSave(const std::string& directory) {
    const auto first = directory + "/before-reset.pcap";
    const auto second = directory + "/after-reset.pcap";
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start first visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, 0);
    require(visit.finish(first.c_str()) == VisitState::saved, "save first visit");

    visit.reset();
    require(!visit.hasSample() && visit.savedPcapSize() == 0 && visit.channel() == 0 &&
                visit.captureStatistics().bytesWritten == 0,
            "reset the previous result, target, and statistics");
    require(visit.start(&other, 1) == StartResult::started, "start a new visit on another target");
    visit.receive(beacon.data(), beacon.size(), other.channel, 0);
    require(visit.finish(second.c_str()) == VisitState::not_observed, "discard the old sample");
    require(storage::recoverPcap(first.c_str()) == storage::CaptureState::saved,
            "keep the original capture");
    require(storage::recoverPcap(second.c_str()) == storage::CaptureState::missing,
            "leave the quiet visit without a capture");
}

void testReceivedChannel() {
    // Without a DS Parameter element, only the received channel ties the beacon to the visit.
    const std::vector<uint8_t> noDs(beacon.begin(), beacon.end() - 3);
    PassiveSession offChannel;
    require(offChannel.start(&target, 1) == StartResult::started, "start the channel visit");
    require(!offChannel.receive(noDs.data(), noDs.size(), 11, 1) && !offChannel.hasSample(),
            "reject a beacon received on another channel");
    require(offChannel.receive(noDs.data(), noDs.size(), target.channel, 1),
            "accept the same beacon on the visit channel");
}

void testFrameBounds() {
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start the bounds visit");
    require(!visit.receive(nullptr, beacon.size(), target.channel, 1), "reject a missing frame");
    const std::vector<uint8_t> stub(beacon.begin(), beacon.begin() + 21);
    require(!visit.receive(stub.data(), stub.size(), target.channel, 1),
            "reject a frame too short to hold a BSSID");
    require(!visit.hasSample(), "rejected frames leave no sample");
}

void testFixedWindowStops() {
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a fixed visit");
    require(!visit.observationDone(1), "a fixed visit listens before the first beacon");
    require(visit.receive(beacon.data(), beacon.size(), target.channel, 1),
            "accept the first beacon");
    require(visit.observationDone(2) &&
                !visit.receive(beacon.data(), beacon.size(), target.channel, 2),
            "a fixed visit stops after the first beacon");
}

void testResetRestoresWindow() {
    PassiveSession visit;
    require(visit.startAdaptive(target, 3000000) == StartResult::started, "start a short visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, 1);
    visit.reset();
    require(visit.start(&target, 1) == StartResult::started, "start a fixed visit after reset");
    require(!visit.observationDone(4000000), "reset restores the 5 s window");
    require(visit.receive(beacon.data(), beacon.size(), target.channel, 4000000) &&
                visit.observationDone(4000001),
            "reset ends adaptive listening");

    constexpr uint64_t base = 5000000;
    visit.reset();
    require(visit.startAdaptive(target, 7000000) == StartResult::started, "start a novelty visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, base - 1);
    visit.reset();
    require(visit.startAdaptive(target, 7000000) == StartResult::started, "restart after reset");
    visit.receive(beacon.data(), beacon.size(), target.channel, base - 1);
    require(!visit.observationDone(base), "reset forgets earlier observations");
}

void testShorterSsidExtends() {
    constexpr uint64_t base = 5000000;
    auto shorter = beacon; // "La" is a prefix of the fixture's "Lab".
    shorter[37] = 2;
    std::vector<uint8_t> prefix(shorter.begin(), shorter.begin() + 40);
    prefix.insert(prefix.end(), shorter.begin() + 41, shorter.end());
    PassiveSession visit;
    require(visit.startAdaptive(target, 7000000) == StartResult::started, "start an SSID visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, 1);
    require(visit.receive(prefix.data(), prefix.size(), target.channel, base - 1),
            "accept the shorter SSID");
    require(!visit.observationDone(base), "a shorter SSID is a new observation");
}

void testShortWrites(const std::string& directory) {
    const auto path = directory + "/short-writes.pcap";
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a short-write visit");
    visit.receive(beacon.data(), beacon.size(), target.channel, 42);
    writeFault.chunk = 7;
    const auto state = visit.finish(path.c_str());
    const size_t shortCalls = writeFault.shortCalls;
    writeFault = {};
    // 40 header bytes, 47 beacon bytes and a 20-byte receipt need at least 13 short calls.
    require(shortCalls >= 13, "the write() hook shortened the save's writes");
    require(state == VisitState::saved, "retry short writes until the capture is saved");
    require(storage::recoverPcap(path.c_str()) == storage::CaptureState::saved &&
                visit.captureStatistics().bytesWritten == 40 + beacon.size(),
            "short writes still save every byte once");
    std::ifstream input(path, std::ios::binary);
    input.seekg(40);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), {}};
    require(saved == std::vector<uint8_t>(beacon.begin(), beacon.end()),
            "short writes keep the beacon bytes in order");
}

// Saves `frame` while the file accepts only `budget` bytes.
VisitState saveWithBudget(const std::string& path, const std::vector<uint8_t>& frame, size_t budget,
                          PassiveSession& visit) {
    require(visit.start(&target, 1) == StartResult::started, "start a failing visit");
    visit.receive(frame.data(), frame.size(), target.channel, 42);
    writeFault.budget = budget;
    const auto state = visit.finish(path.c_str());
    writeFault = {};
    return state;
}

void testWriteErrors(const std::string& directory) {
    const auto large = largeBeacon();
    const std::vector<uint8_t> small(beacon.begin(), beacon.end());
    const struct {
        const char* name;
        const std::vector<uint8_t>& frame;
        size_t budget;
        const char* message;
    } cases[]{
        {"/header-error.pcap", small, 10, "a header write error fails the save"},
        {"/block-error.pcap", large, 140, "a beacon write error fails the save"},
        {"/receipt-error.pcap", small, 40 + beacon.size(), "a receipt write error fails the save"},
    };
    for (const auto& item : cases) {
        const auto path = directory + item.name;
        PassiveSession visit;
        const std::string name = std::string(" (") + (item.name + 1) + ")";
        require(saveWithBudget(path, item.frame, item.budget, visit) == VisitState::storage_error,
                item.message + name);
        require(visit.captureStatistics().bytesWritten == item.budget &&
                    visit.captureStatistics().queueOverflows == 0 && visit.savedPcapSize() == 0,
                "report the bytes accepted before the error" + name);
        require(storage::recoverPcap(path.c_str()) == storage::CaptureState::incomplete,
                "a failed save is not saved on restart" + name);
    }
}

void testAdaptiveStartWhileBusy() {
    PassiveSession visit;
    require(visit.start(&target, 1) == StartResult::started, "start a fixed visit");
    require(visit.startAdaptive(target, 3000000) == StartResult::busy,
            "an adaptive start cannot replace a running visit");
    require(!visit.observationDone(4000000), "a refused start keeps the 5 s window");
    visit.receive(beacon.data(), beacon.size(), target.channel, 1);
    require(visit.observationDone(2), "a refused start keeps the fixed visit");
}

void testFirstBeaconKept(const std::string& directory) {
    const auto path = directory + "/first-beacon.pcap";
    auto renamed = beacon;
    renamed[38] = 'X';
    PassiveSession visit;
    require(visit.startAdaptive(target, 7000000) == StartResult::started, "start a long visit");
    require(visit.receive(beacon.data(), beacon.size(), target.channel, 1) &&
                visit.receive(renamed.data(), renamed.size(), target.channel, 2),
            "accept two different beacons");
    require(visit.finish(path.c_str()) == VisitState::saved, "save the long visit");
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(input), {}};
    require(saved.size() == 40 + beacon.size() &&
                storage::pcap::readNumber(saved.data() + 28, 4, true) == 1 &&
                std::equal(beacon.begin(), beacon.end(), saved.begin() + 40),
            "save the first beacon and its time, not a later one");
}

void testTargetRules() {
    using session::validTarget;
    require(validTarget({{2, 0, 0, 0, 0, 1}, 1}) && validTarget({{2, 0, 0, 0, 0, 1}, 13}),
            "accept 2.4 GHz channels 1 to 13");
    require(!validTarget({{2, 0, 0, 0, 0, 1}, 0}) && !validTarget({{2, 0, 0, 0, 0, 1}, 14}),
            "reject channels outside 1 to 13");
    require(!validTarget({{3, 0, 0, 0, 0, 1}, 6}) && !validTarget({{0, 0, 0, 0, 0, 0}, 6}),
            "reject a multicast or all-zero BSSID");

    session::Profile profile;
    profile.removeLast();
    require(profile.size() == 0, "removing from an empty profile keeps it empty");
    for (uint8_t last = 1; last <= session::Profile::capacity; ++last) {
        require(profile.add({{2, 0, 0, 0, 0, last}, 6}), "add up to five targets");
    }
    require(!profile.add({{2, 0, 0, 0, 0, 9}, 6}) && profile.size() == session::Profile::capacity,
            "refuse a sixth target");

    session::Profile shorter;
    require(shorter.add(target), "add one target");
    session::Profile longer;
    require(longer.add(target) && longer.add(other), "add two targets");
    require(!shorter.matches(longer) && !longer.matches(shorter),
            "profiles of different lengths differ");
}

} // namespace

int main(int argc, char** argv) try {
    signal(SIGALRM, reportStuckGroup);
    requireEmptyDirectory(argc, argv);
    runGroup("testBeaconDiscovery", testBeaconDiscovery);
    runGroup("testEmptyList", [&] { testEmptyList(argv[1]); });
    runGroup("testFilteredCapture", [&] { testFilteredCapture(argv[1]); });
    runGroup("testObservationWindow", [&] { testObservationWindow(argv[1]); });
    runGroup("testObservationBounds", testObservationBounds);
    runGroup("testCaptureAcrossBlocks", [&] { testCaptureAcrossBlocks(argv[1]); });
    runGroup("testBeaconSizeLimit", [&] { testBeaconSizeLimit(argv[1]); });
    runGroup("testAdaptiveCap", testAdaptiveCap);
    runGroup("testAcceptedBeaconShapes", testAcceptedBeaconShapes);
    runGroup("testRejectedBeaconShapes", testRejectedBeaconShapes);
    runGroup("testQuietAndWriteFailure", [&] { testQuietAndWriteFailure(argv[1]); });
    runGroup("testResetAfterSave", [&] { testResetAfterSave(argv[1]); });
    runGroup("testReceivedChannel", testReceivedChannel);
    runGroup("testFrameBounds", testFrameBounds);
    runGroup("testFixedWindowStops", testFixedWindowStops);
    runGroup("testResetRestoresWindow", testResetRestoresWindow);
    runGroup("testShorterSsidExtends", testShorterSsidExtends);
    runGroup("testShortWrites", [&] { testShortWrites(argv[1]); });
    runGroup("testWriteErrors", [&] { testWriteErrors(argv[1]); });
    runGroup("testAdaptiveStartWhileBusy", testAdaptiveStartWhileBusy);
    runGroup("testFirstBeaconKept", [&] { testFirstBeaconKept(argv[1]); });
    runGroup("testTargetRules", testTargetRules);
    std::cout << "passive session: " << groupsPassed << " groups passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
