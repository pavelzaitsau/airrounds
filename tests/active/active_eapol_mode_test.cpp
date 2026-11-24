// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

#include "../../firmware/active/active_eapol_mode.h"

namespace {

using airrounds::active::ActiveEapolMode;
using airrounds::session::CaptureModeId;
using airrounds::session::ModeContext;
using airrounds::session::ModeResult;
using airrounds::session::Target;
using airrounds::session::TransmitBudget;
using airrounds::session::Transmitter;

constexpr Target target{{2, 0, 0, 0, 0, 1}, 6};
const uint8_t bssid[6] = {2, 0, 0, 0, 0, 1};
const uint8_t client[6] = {0x12, 0x22, 0x33, 0x44, 0x55, 0x66};

// A data frame, without its FCS, in the form onFrame receives. fc1 carries the DS bits.
std::vector<uint8_t> dataFrame(uint8_t fc1, const uint8_t addr1[6], const uint8_t addr2[6]) {
    std::vector<uint8_t> frame(30, 0);
    frame[0] = 0x08;
    frame[1] = fc1;
    std::memcpy(frame.data() + 4, addr1, 6);
    std::memcpy(frame.data() + 10, addr2, 6);
    std::memcpy(frame.data() + 16, bssid, 6);
    return frame;
}

// An uplink data frame (ToDS, not EAPOL) from a client to the access point: Addr1 is the BSSID,
// Addr2 is the client. It is how the mode learns its target.
std::vector<uint8_t> uplink(const uint8_t transmitter[6]) {
    return dataFrame(0x01, bssid, transmitter);
}

// An EAPOL-Key handshake message, placed per DS direction: 1 and 3 go AP to client (FromDS), 2 and
// 4 go client to AP (ToDS). keyData appends after the 95 fixed fields.
std::vector<uint8_t> handshake(int message, const uint8_t peer[6],
                               const std::vector<uint8_t>& keyData = {}) {
    static const uint16_t keyInfo[4] = {0x0088, 0x0108, 0x0388, 0x0308};
    const bool fromDs = message == 1 || message == 3;
    // A 24-byte header so the LLC/SNAP follows immediately, as a real EAPOL data frame.
    std::vector<uint8_t> frame(24, 0);
    frame[0] = 0x08;
    frame[1] = static_cast<uint8_t>(fromDs ? 0x02 : 0x01);
    std::memcpy(frame.data() + 4, fromDs ? peer : bssid, 6);  // FromDS Addr1 client, else BSSID.
    std::memcpy(frame.data() + 10, fromDs ? bssid : peer, 6); // FromDS Addr2 BSSID, else client.
    std::memcpy(frame.data() + 16, bssid, 6);
    const uint8_t llcSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};
    frame.insert(frame.end(), std::begin(llcSnap), std::end(llcSnap));
    const size_t bodyLen = 95 + keyData.size();
    frame.push_back(2); // EAPOL version.
    frame.push_back(3); // EAPOL-Key.
    frame.push_back(static_cast<uint8_t>(bodyLen >> 8U));
    frame.push_back(static_cast<uint8_t>(bodyLen & 0xffU));
    std::vector<uint8_t> body(95, 0);
    body[0] = 2; // Descriptor type.
    body[1] = static_cast<uint8_t>(keyInfo[message - 1] >> 8U);
    body[2] = static_cast<uint8_t>(keyInfo[message - 1] & 0xffU);
    body[93] = static_cast<uint8_t>(keyData.size() >> 8U);
    body[94] = static_cast<uint8_t>(keyData.size() & 0xffU);
    frame.insert(frame.end(), body.begin(), body.end());
    frame.insert(frame.end(), keyData.begin(), keyData.end());
    return frame;
}

std::vector<uint8_t> pmkidKeyData() {
    std::vector<uint8_t> kde = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04}; // PMKID KDE, length 20.
    for (uint8_t index = 0; index < 16; ++index) {
        kde.push_back(index);
    }
    return kde;
}

struct Capture final : Transmitter {
    std::vector<std::vector<uint8_t>> frames;
    bool result = true;
    bool send(const uint8_t* frame, size_t len) override {
        frames.emplace_back(frame, frame + len);
        return result;
    }
};

ModeContext contextFor(Capture& capture, TransmitBudget& budget) {
    return ModeContext{&capture, &budget};
}

void testIdentity() {
    ActiveEapolMode mode;
    assert(mode.id() == CaptureModeId::active);
    assert(mode.needs().frameTypes == airrounds::storage::dataFrames);
}

void testTargetLearning() {
    ActiveEapolMode mode;
    assert(mode.begin(target, 60000000));
    assert(!mode.hasTarget());

    // An uplink frame from the client teaches the target.
    const auto frame = uplink(client);
    mode.onFrame(frame.data(), frame.size(), 6, 1000);
    assert(mode.hasTarget() && std::memcmp(mode.targetClient(), client, 6) == 0);

    // A downlink frame (FromDS) never teaches the target.
    mode.reset();
    assert(mode.begin(target, 60000000));
    const auto downlink = dataFrame(0x02, client, bssid);
    mode.onFrame(downlink.data(), downlink.size(), 6, 1000);
    assert(!mode.hasTarget());

    // A group address, the BSSID itself, a zero address, a wrong BSSID, and a wrong channel are all
    // rejected as the target.
    const uint8_t group[6] = {0x01, 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t elsewhere[6] = {2, 0, 0, 0, 0, 9};
    const std::vector<uint8_t> rejects[] = {uplink(group), uplink(bssid), uplink(zero),
                                            dataFrame(0x01, elsewhere, client)};
    for (const auto& reject : rejects) {
        mode.reset();
        assert(mode.begin(target, 60000000));
        mode.onFrame(reject.data(), reject.size(), 6, 1000);
        assert(!mode.hasTarget());
    }
    mode.reset();
    assert(mode.begin(target, 60000000));
    const auto onChannel = uplink(client);
    mode.onFrame(onChannel.data(), onChannel.size(), 7, 1000); // Wrong channel.
    assert(!mode.hasTarget());
}

void testBurst() {
    ActiveEapolMode mode;
    Capture capture;
    TransmitBudget budget;
    budget.seed(30);
    ModeContext context = contextFor(capture, budget);

    assert(mode.begin(target, 60000000));
    // No target yet: poll transmits nothing.
    mode.poll(1000, context);
    assert(capture.frames.empty());

    const auto learn = uplink(client);
    mode.onFrame(learn.data(), learn.size(), 6, 1000);

    // The first burst: three frames, 100 ms apart.
    mode.poll(1000, context);
    assert(capture.frames.size() == 1);
    mode.poll(1000, context); // Too soon for the next frame.
    assert(capture.frames.size() == 1);
    mode.poll(101000, context);
    assert(capture.frames.size() == 2);
    mode.poll(201000, context);
    assert(capture.frames.size() == 3);

    // The retry burst waits 5 s, then sends three more.
    mode.poll(300000, context);
    assert(capture.frames.size() == 3);
    mode.poll(201000 + 5000000, context);
    mode.poll(201000 + 5100000, context);
    mode.poll(201000 + 5200000, context);
    assert(capture.frames.size() == 6);

    // No third burst: the cap is two bursts.
    mode.poll(201000 + 20000000, context);
    assert(capture.frames.size() == 6 && mode.transmittedFrames() == 6);

    // The transmitted frame is a unicast deauthentication from the access point to the client.
    const auto& first = capture.frames.front();
    assert(first.size() == airrounds::active::deauthFrameSize);
    assert(first[0] == 0xC0 && first[1] == 0x00);
    assert(std::memcmp(first.data() + 4, client, 6) == 0); // Address 1: the client.
    assert(std::memcmp(first.data() + 10, bssid, 6) == 0); // Address 2: the access point.
    assert(first[24] == 7 && first[25] == 0);              // Reason 7, little-endian.
}

void testCaptureBeforeTransmit() {
    ActiveEapolMode mode;
    Capture capture;
    TransmitBudget budget;
    budget.seed(30);
    ModeContext context = contextFor(capture, budget);

    // A handshake already in flight before any burst: message 2 both teaches the target and is
    // captured, so the mode never transmits.
    assert(mode.begin(target, 60000000));
    const auto message2 = handshake(2, client);
    assert(mode.onFrame(message2.data(), message2.size(), 6, 1000));
    mode.poll(1000, context);
    assert(capture.frames.empty() && mode.hasTarget());
    // A latched-but-never-transmitted visit records a zero audit: no frames, no reason, no client.
    assert(mode.transmittedFrames() == 0 && mode.transmitReason() == 0 &&
           mode.transmitTarget() == nullptr);
}

void testAutoStopMidBurst() {
    ActiveEapolMode mode;
    Capture capture;
    TransmitBudget budget;
    budget.seed(30);
    ModeContext context = contextFor(capture, budget);

    assert(mode.begin(target, 60000000));
    const auto learn = uplink(client);
    mode.onFrame(learn.data(), learn.size(), 6, 1000);
    mode.poll(1000, context);
    assert(capture.frames.size() == 1);

    // A captured EAPOL-Key stops the transmit at once.
    const auto message2 = handshake(2, client);
    assert(mode.onFrame(message2.data(), message2.size(), 6, 1100));
    mode.poll(101000, context);
    mode.poll(201000, context);
    assert(capture.frames.size() == 1);
}

void testEarlyFinish() {
    const uint8_t other[6] = {0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};

    // Messages 1 and 2 from one client finish the visit early.
    {
        ActiveEapolMode mode;
        assert(mode.begin(target, 60000000));
        const auto m1 = handshake(1, client);
        const auto m2 = handshake(2, client);
        assert(mode.onFrame(m1.data(), m1.size(), 6, 1000));
        assert(mode.onFrame(m2.data(), m2.size(), 6, 2000));
        assert(mode.done(3000) && mode.recordCount() == 2);
    }
    // Message 1 alone, with budget left, keeps listening.
    {
        ActiveEapolMode mode;
        assert(mode.begin(target, 60000000));
        const auto m1 = handshake(1, client);
        assert(mode.onFrame(m1.data(), m1.size(), 6, 1000));
        assert(!mode.done(3000));
    }
    // Message 1 carrying a PMKID finishes early on its own.
    {
        ActiveEapolMode mode;
        assert(mode.begin(target, 60000000));
        const auto m1 = handshake(1, client, pmkidKeyData());
        assert(mode.onFrame(m1.data(), m1.size(), 6, 1000));
        assert(mode.done(3000));
    }
    // A cross-client message 1 and message 2 is not a crackable pair.
    {
        ActiveEapolMode mode;
        assert(mode.begin(target, 60000000));
        const auto m1 = handshake(1, client);
        const auto m2 = handshake(2, other);
        assert(mode.onFrame(m1.data(), m1.size(), 6, 1000));
        assert(mode.onFrame(m2.data(), m2.size(), 6, 2000));
        assert(!mode.done(3000));
    }
    // Messages 3 and 4 from one client still finish early, as the passive mode does.
    {
        ActiveEapolMode mode;
        assert(mode.begin(target, 60000000));
        const auto m3 = handshake(3, client);
        const auto m4 = handshake(4, client);
        assert(mode.onFrame(m3.data(), m3.size(), 6, 1000));
        assert(mode.onFrame(m4.data(), m4.size(), 6, 2000));
        assert(mode.done(3000));
    }
}

void testResultDelegates() {
    ActiveEapolMode mode;
    assert(mode.begin(target, 60000000));
    assert(mode.finish() == ModeResult::not_observed);
    mode.reset();
    assert(mode.begin(target, 60000000));
    const auto m2 = handshake(2, client);
    assert(mode.onFrame(m2.data(), m2.size(), 6, 1000));
    assert(mode.finish() == ModeResult::found && mode.recordCount() == 1);
}

} // namespace

int main() {
    testIdentity();
    testTargetLearning();
    testBurst();
    testCaptureBeforeTransmit();
    testAutoStopMidBurst();
    testEarlyFinish();
    testResultDelegates();
    std::cout << "active EAPOL mode tests passed\n";
    return 0;
}
