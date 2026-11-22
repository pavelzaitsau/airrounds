// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace airrounds {
namespace active {

// The fixed size of an IEEE 802.11 deauthentication frame: a 24-byte MAC header and a 2-byte
// reason code. The frame carries no information elements.
constexpr size_t deauthFrameSize = 26;

// Build one unicast deauthentication frame, from the IEEE 802.11 standard. The frame tells one
// client that the access point has deauthenticated it, so the client leaves and rejoins. The access
// point address is the source, because a deauthentication is an access-point-to-client frame.
//
// out receives the frame; capacity must be at least deauthFrameSize. client is the one target
// client address; bssid is the access point. reasonCode is the 802.11 reason, little-endian on the
// wire. Returns deauthFrameSize, or 0 when the buffer is too small or a pointer is null. The
// function is pure: it touches no radio and allocates nothing, so a host test checks the bytes.
inline size_t buildDeauthFrame(uint8_t* out, size_t capacity, const uint8_t client[6],
                               const uint8_t bssid[6], uint16_t reasonCode) {
    if (out == nullptr || client == nullptr || bssid == nullptr || capacity < deauthFrameSize) {
        return 0;
    }
    memset(out, 0, deauthFrameSize);
    out[0] = 0xC0; // Management frame, subtype 12: deauthentication.
    out[1] = 0x00; // A management frame has the ToDS and FromDS bits clear.
    // out[2..3]: the duration stays 0.
    memcpy(out + 4, client, 6);  // Address 1: the destination, the target client.
    memcpy(out + 10, bssid, 6);  // Address 2: the source, the access point.
    memcpy(out + 16, bssid, 6);  // Address 3: the BSSID.
    // out[22..23]: the sequence control stays 0; the radio sets it.
    out[24] = static_cast<uint8_t>(reasonCode & 0xFFU); // Reason code, little-endian.
    out[25] = static_cast<uint8_t>((reasonCode >> 8U) & 0xFFU);
    return deauthFrameSize;
}

// The default reason: class-3 frame received from a nonassociated station. A client that receives
// it reassociates, which produces the handshake the active mode captures.
constexpr uint16_t deauthReasonClass3 = 7;

} // namespace active
} // namespace airrounds
