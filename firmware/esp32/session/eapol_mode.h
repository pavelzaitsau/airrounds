// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "capture_mode.h"
#include "eapol.h"

namespace airrounds {
namespace session {

// Records the EAPOL-Key frames of the WPA 4-way handshake for the round's target access point.
// Receive-only: it waits for a client to join on its own and records the handshake the join
// produces. It never transmits, decrypts, or derives a key. See docs/t11.md and
// docs/capture-modes-design.md, "EAPOL mode".
class EapolMode final : public CaptureMode {
public:
    // One EAPOL-Key frame with an encrypted key-data block. The 802.11 header (up to 30 bytes),
    // the LLC/SNAP header (8), the EAPOL header (4), and the fixed EAPOL-Key fields (95) take 137
    // bytes, so 512 leaves 375 for key data. 512 is below the snap length, so the writer never
    // truncates a buffered frame.
    static constexpr size_t maxEapolFrameSize = 512;
    static constexpr size_t messageCount = 4; // Messages 1 to 4.
    // The point's budget holds this reserve for radio setup and the save, as Beacon's 10 s holds 3.
    static constexpr uint64_t saveReserveUs = 3000000;

    static_assert(maxEapolFrameSize <= storage::pcapSnapLength,
                  "a buffered frame must fit the snap length so the writer never truncates it");

    CaptureModeId id() const override { return CaptureModeId::eapol; }
    RadioNeeds needs() const override { return {storage::dataFrames}; }

    void reset() override {
        held_ = 0;
        for (size_t index = 0; index < messageCount; ++index) {
            sizes_[index] = 0;
            timestamps_[index] = 0;
        }
    }

    bool begin(const Target& target, uint64_t budgetUs) override {
        target_ = target;
        budgetUs_ = budgetUs;
        reset();
        return true;
    }

    // Runs in the receive callback under the receive lock. Rejects before it parses, because the
    // data filter delivers every data frame on the channel. Every size check precedes its read.
    bool onFrame(const uint8_t* frame, size_t size, uint8_t channel, uint64_t elapsedUs) override {
        if (channel != target_.channel || size < eapolBaseHeaderLen) {
            return false;
        }
        if ((frame[0] & 0x0cU) != 0x08U) { // Data frames only.
            return false;
        }
        if ((frame[1] & 0x40U) != 0) { // Protected: the body is encrypted, do not parse it.
            return false;
        }
        const uint8_t* bssid = eapolBssid(frame, size); // DS-bit address, null for WDS.
        if (bssid == nullptr || memcmp(bssid, target_.bssid, sizeof(target_.bssid)) != 0) {
            return false; // Reject another BSSID with one 6-byte compare, before the EAPOL parse.
        }
        if (!isEapolKeyFrame(frame, size)) {
            return false;
        }
        const size_t eapol = eapolHeaderLen(frame) + llcSnapLen; // EAPOL header start.
        if (size < eapol + 7) { // version, type, length, descriptor, and the 2 key-info bytes.
            return false;
        }
        const auto keyInfo =
            static_cast<uint16_t>((frame[eapol + 5] << 8U) | frame[eapol + 6]);
        const uint8_t message = classifyKeyInfo(keyInfo); // 1..4; 0 for group, WPA1 3, or no match.
        if (message == 0 || size > maxEapolFrameSize) { // Never truncate a record.
            return false;
        }
        const size_t slot = message - 1U;
        memcpy(frames_[slot], frame, size); // Keep the latest frame per message.
        sizes_[slot] = static_cast<uint16_t>(size);
        timestamps_[slot] = static_cast<uint32_t>(elapsedUs);
        held_ = static_cast<uint8_t>(held_ | (1U << slot));
        return true;
    }

    // True once the reserve leaves no listening time, or once messages 3 and 4 from one client are
    // held: a complete handshake needs no more time. The early stop is a timing optimisation, not
    // the success rule.
    bool done(uint64_t elapsedUs) const override {
        if (elapsedUs + saveReserveUs >= budgetUs_) {
            return true;
        }
        constexpr uint8_t handshakePair = (1U << 2U) | (1U << 3U); // Messages 3 and 4.
        if ((held_ & handshakePair) == handshakePair) {
            // Message 3 (AP to client) client is Addr1; message 4 (client to AP) client is Addr2.
            return memcmp(frames_[2] + 4, frames_[3] + 10, 6) == 0;
        }
        return false;
    }

    // True when the held frames form a set an attacker could use offline: messages 1 and 2 from one
    // client, messages 3 and 4 from one client, or a message 1 that carries a PMKID. The active mode
    // finishes a visit early on this, so a round of several networks moves on once a network yields a
    // crackable capture. The passive mode does not call it.
    bool hasCrackableSet() const {
        constexpr uint8_t pair12 = (1U << 0U) | (1U << 1U);
        constexpr uint8_t pair34 = (1U << 2U) | (1U << 3U);
        if ((held_ & pair34) == pair34 && memcmp(frames_[2] + 4, frames_[3] + 10, 6) == 0) {
            return true; // Message 3 client is Addr1; message 4 client is Addr2.
        }
        if ((held_ & pair12) == pair12 && memcmp(frames_[0] + 4, frames_[1] + 10, 6) == 0) {
            return true; // Message 1 client is Addr1; message 2 client is Addr2.
        }
        return (held_ & (1U << 0U)) != 0 && hasPmkidKde(frames_[0], sizes_[0]);
    }

    ModeResult finish() override { return held_ != 0 ? ModeResult::found : ModeResult::not_observed; }

    size_t recordCount() const override {
        size_t count = 0;
        for (size_t bit = 0; bit < messageCount; ++bit) {
            if ((held_ & (1U << bit)) != 0) {
                ++count;
            }
        }
        return count;
    }

    // The index-th held frame in capture order, sorted by timestamp, so the PCAP timestamps stay
    // monotonic even when a repeated message arrives out of message order.
    storage::PcapRecord record(size_t index) const override {
        uint8_t order[messageCount];
        sortedHeld(order);
        const uint8_t slot = order[index];
        return {frames_[slot], sizes_[slot], timestamps_[slot]};
    }

private:
    // Fills order with the held message slots, ascending by timestamp; returns the count. A fixed
    // four-element insertion sort that allocates nothing.
    size_t sortedHeld(uint8_t (&order)[messageCount]) const {
        size_t count = 0;
        for (uint8_t bit = 0; bit < messageCount; ++bit) {
            if ((held_ & (1U << bit)) != 0) {
                order[count++] = bit;
            }
        }
        for (size_t i = 1; i < count; ++i) {
            const uint8_t key = order[i];
            size_t j = i;
            while (j > 0 && timestamps_[order[j - 1]] > timestamps_[key]) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = key;
        }
        return count;
    }

    uint8_t frames_[messageCount][maxEapolFrameSize]{};
    uint16_t sizes_[messageCount]{};
    uint32_t timestamps_[messageCount]{};
    uint8_t held_ = 0;
    Target target_{};
    uint64_t budgetUs_ = 0;
};

} // namespace session
} // namespace airrounds
