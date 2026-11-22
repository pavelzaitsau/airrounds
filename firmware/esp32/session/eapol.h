// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Clean-room EAPOL-Key parser, written from IEEE 802.11-2020 clause 9 (the MAC header) and
// clause 12.7 (the EAPOL-Key frame and the WPA 4-way handshake). It copies no code from any other
// project. See docs/t11.md, "Clean-room parser".
//
// Every function reads a frame without its FCS, the form onFrame receives. They allocate nothing
// and have no side effect. Every size check precedes the read it guards.

namespace airrounds {
namespace session {

// The shortest 802.11 data frame header, the three-address form.
constexpr size_t eapolBaseHeaderLen = 24;
// LLC/SNAP (6 bytes: AA AA 03 00 00 00) plus the 2-byte ethertype.
constexpr size_t llcSnapLen = 8;
// The IEEE 802.1X ethertype that marks an EAPOL frame.
constexpr uint8_t eapolEthertypeHigh = 0x88;
constexpr uint8_t eapolEthertypeLow = 0x8e;
// EAPOL packet type 3 is EAPOL-Key; types 0 (EAP-Packet) and 1 (EAPOL-Start) are not.
constexpr uint8_t eapolKeyPacketType = 3;

// Key Information flags, IEEE 802.11-2020 Figure 12-34, read big-endian from the wire.
constexpr uint16_t keyInfoKeyType = 0x0008; // 1 pairwise, 0 group.
constexpr uint16_t keyInfoKeyAck = 0x0080;
constexpr uint16_t keyInfoKeyMic = 0x0100;
constexpr uint16_t keyInfoSecure = 0x0200;

// The BSSID for a frame by its ToDS/FromDS bits, or nullptr for a 4-address WDS frame (no single
// BSSID) or a frame shorter than a 24-byte header.
inline const uint8_t* eapolBssid(const uint8_t* frame, size_t size) {
    if (frame == nullptr || size < eapolBaseHeaderLen) {
        return nullptr;
    }
    switch (frame[1] & 0x03U) {
    case 0x00U: // ToDS 0, FromDS 0: same-BSS data, BSSID is Addr3.
        return frame + 16;
    case 0x01U: // ToDS 1, FromDS 0: client to AP (messages 2 and 4), BSSID is Addr1.
        return frame + 4;
    case 0x02U: // ToDS 0, FromDS 1: AP to client (messages 1 and 3), BSSID is Addr2.
        return frame + 10;
    default: // 0x03: 4-address WDS, no single BSSID.
        return nullptr;
    }
}

// The 802.11 MAC header length: 24, plus 2 for a QoS data subtype, plus 4 for HT Control. The
// caller guarantees a full 24-byte header before it calls this.
inline size_t eapolHeaderLen(const uint8_t* frame) {
    size_t length = eapolBaseHeaderLen;
    // A QoS data subtype sets bit 7 of the frame-control subtype field (frame[0] & 0x80).
    if ((frame[0] & 0x80U) != 0) {
        length += 2; // QoS Control.
        // The Order bit means HT Control only on a QoS data frame; on a non-QoS data frame it is
        // the legacy strictly-ordered flag and adds no field.
        if ((frame[1] & 0x80U) != 0) {
            length += 4; // HT Control.
        }
    }
    return length;
}

// True when the frame carries an LLC/SNAP header, the ethertype 0x88 0x8e, and EAPOL packet
// type 3 (Key). It reads no further into the EAPOL-Key body.
inline bool isEapolKeyFrame(const uint8_t* frame, size_t size) {
    if (frame == nullptr || size < eapolBaseHeaderLen) {
        return false;
    }
    const size_t header = eapolHeaderLen(frame);
    if (size < header + llcSnapLen) {
        return false;
    }
    static const uint8_t llcSnap[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
    if (memcmp(frame + header, llcSnap, sizeof(llcSnap)) != 0 ||
        frame[header + 6] != eapolEthertypeHigh || frame[header + 7] != eapolEthertypeLow) {
        return false;
    }
    // The EAPOL header (version, packet type, body length) follows the ethertype.
    if (size < header + llcSnapLen + 2) {
        return false;
    }
    return frame[header + llcSnapLen + 1] == eapolKeyPacketType;
}

// The handshake message 1 to 4, or 0 for no match. Returns 0 when the Key Type bit is clear,
// which covers a group rekey, and 0 for a WPA1 message 3 (Secure clear).
inline uint8_t classifyKeyInfo(uint16_t keyInfo) {
    if ((keyInfo & keyInfoKeyType) == 0) {
        return 0;
    }
    const bool ack = (keyInfo & keyInfoKeyAck) != 0;
    const bool mic = (keyInfo & keyInfoKeyMic) != 0;
    const bool secure = (keyInfo & keyInfoSecure) != 0;
    if (ack && !mic && !secure) {
        return 1;
    }
    if (!ack && mic && !secure) {
        return 2;
    }
    if (ack && mic && secure) {
        return 3;
    }
    if (!ack && mic && secure) {
        return 4;
    }
    return 0;
}

} // namespace session
} // namespace airrounds
