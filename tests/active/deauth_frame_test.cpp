// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../../firmware/active/deauth_frame.h"

int main() {
    using airrounds::active::buildDeauthFrame;
    using airrounds::active::deauthFrameSize;
    using airrounds::active::deauthReasonClass3;

    const uint8_t client[6] = {0x12, 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t bssid[6] = {0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5};
    uint8_t frame[64];

    const size_t size = buildDeauthFrame(frame, sizeof(frame), client, bssid, deauthReasonClass3);
    assert(size == deauthFrameSize && size == 26);
    assert(frame[0] == 0xC0 && frame[1] == 0x00); // Deauthentication management frame, no DS flags.
    assert(frame[2] == 0 && frame[3] == 0);       // Duration.
    assert(std::memcmp(frame + 4, client, 6) == 0);  // Address 1: the client.
    assert(std::memcmp(frame + 10, bssid, 6) == 0);  // Address 2: the access point.
    assert(std::memcmp(frame + 16, bssid, 6) == 0);  // Address 3: the BSSID.
    assert(frame[22] == 0 && frame[23] == 0);     // Sequence control.
    assert(frame[24] == 7 && frame[25] == 0);     // Reason 7, little-endian.

    // A different reason code is little-endian across both bytes.
    assert(buildDeauthFrame(frame, sizeof(frame), client, bssid, 0x1234) == deauthFrameSize);
    assert(frame[24] == 0x34 && frame[25] == 0x12);

    // The frame is unicast: Address 1 is an individual address, never a group address.
    assert((frame[4] & 0x01U) == 0);

    // Guards: a short buffer and any null pointer return 0.
    uint8_t small[deauthFrameSize - 1];
    assert(buildDeauthFrame(small, sizeof(small), client, bssid, 1) == 0);
    assert(buildDeauthFrame(nullptr, 64, client, bssid, 1) == 0);
    assert(buildDeauthFrame(frame, sizeof(frame), nullptr, bssid, 1) == 0);
    assert(buildDeauthFrame(frame, sizeof(frame), client, nullptr, 1) == 0);

    std::cout << "deauth frame builder passed\n";
    return 0;
}
