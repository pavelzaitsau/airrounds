#pragma once

#include <atomic>

#include "pcap_capture.h"
#include "target.h"

namespace airrounds {
namespace session {

enum class StartResult { started, empty_list, invalid_target, busy };
enum class VisitState { idle, observing, not_observed, saved, storage_error };

struct BeaconView {
    const uint8_t* bssid = nullptr;
    const uint8_t* ssid = nullptr;
    size_t ssidSize = 0;
};

constexpr size_t maxBeaconSize = 2304;

inline bool parseBeacon(uint8_t channel, const uint8_t* frame, size_t size, BeaconView& beacon) {
    constexpr size_t fixedBeaconSize = 36;
    constexpr uint8_t broadcast[6]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (frame == nullptr || size < fixedBeaconSize || size > maxBeaconSize || frame[0] != 0x80 ||
        (frame[1] & 0xc7U) != 0 || (frame[22] & 0x0fU) != 0 ||
        memcmp(frame + 4, broadcast, sizeof(broadcast)) != 0 || (frame[16] & 1U) != 0 ||
        memcmp(frame + 10, frame + 16, 6) != 0) {
        return false;
    }

    BeaconView found;
    for (size_t offset = fixedBeaconSize; offset < size;) {
        if (size - offset < 2) {
            return false;
        }
        const uint8_t id = frame[offset++];
        const size_t length = frame[offset++];
        if (length > size - offset) {
            return false;
        }
        if (id == 0) {
            if (found.ssid != nullptr || length > 32) {
                return false;
            }
            found.ssid = frame + offset;
            found.ssidSize = length;
        }
        if ((id == 3 && length != 1) || (id == 61 && length != 22) ||
            ((id == 3 || id == 61) && frame[offset] != channel)) {
            return false;
        }
        offset += length;
    }
    const uint8_t zeroAddress[6]{};
    if (found.ssid == nullptr || memcmp(frame + 16, zeroAddress, sizeof(zeroAddress)) == 0) {
        return false;
    }
    found.bssid = frame + 16;
    beacon = found;
    return true;
}

class BeaconDiscovery {
public:
    BeaconDiscovery(const char* name, size_t size) : name_(name), nameSize_(size) {}

    void receive(const uint8_t* frame, size_t size, uint8_t channel) {
        BeaconView beacon;
        if (ambiguous_ || !parseBeacon(channel, frame, size, beacon) ||
            beacon.ssidSize != nameSize_ || memcmp(beacon.ssid, name_, nameSize_) != 0) {
            return;
        }
        if (target_.channel == 0) {
            memcpy(target_.bssid, beacon.bssid, sizeof(target_.bssid));
            target_.channel = channel;
            return;
        }
        ambiguous_ = target_.channel != channel ||
                     memcmp(target_.bssid, beacon.bssid, sizeof(target_.bssid)) != 0;
    }

    bool ambiguous() const { return ambiguous_; }
    const Target& target() const { return target_; }

private:
    const char* name_;
    size_t nameSize_;
    Target target_{};
    bool ambiguous_ = false;
};

class PassiveSession {
public:
    enum : uint32_t { windowUs = 5000000, maxFrameSize = maxBeaconSize };

    StartResult start(const Target* targets, size_t count) {
        if (state_ != VisitState::idle) {
            return StartResult::busy;
        }
        if (count == 0) {
            return StartResult::empty_list;
        }
        if (targets == nullptr || count != 1 || !validTarget(*targets)) {
            return StartResult::invalid_target;
        }
        target_ = *targets;
        state_ = VisitState::observing;
        return StartResult::started;
    }

    // One radio task calls receive. Stop that task's delivery before calling finish.
    void receive(const uint8_t* frame, size_t size, uint8_t receivedChannel, uint64_t elapsedUs) {
        BeaconView beacon;
        if (state_ != VisitState::observing || hasSample() || receivedChannel != target_.channel ||
            elapsedUs >= windowUs || !parseBeacon(receivedChannel, frame, size, beacon) ||
            memcmp(beacon.bssid, target_.bssid, sizeof(target_.bssid)) != 0) {
            return;
        }
        memcpy(frame_, frame, size);
        frameSize_ = size;
        timestampUs_ = static_cast<uint32_t>(elapsedUs);
        hasSample_.store(true, std::memory_order_release);
    }

    // Reset only after the radio detaches and any save has finished.
    void reset() {
        target_ = {};
        frameSize_ = 0;
        timestampUs_ = 0;
        hasSample_.store(false, std::memory_order_relaxed);
        state_ = VisitState::idle;
    }

    uint8_t channel() const { return target_.channel; }

    bool hasSample() const { return hasSample_.load(std::memory_order_acquire); }

    VisitState finish(const char* path) {
        if (state_ == VisitState::observing) {
            state_ = hasSample() ? save(path) : VisitState::not_observed;
        }
        return state_;
    }

    size_t savedPcapSize() const {
        return state_ == VisitState::saved ? pcapHeaderSize + recordHeaderSize + frameSize_ : 0;
    }

private:
    static constexpr size_t pcapHeaderSize = 24;
    static constexpr size_t recordHeaderSize = 16;

    static void writeLe32(uint8_t* field, uint32_t value) {
        for (size_t index = 0; index < sizeof(value); ++index) {
            field[index] = static_cast<uint8_t>(value);
            value >>= 8U;
        }
    }

    VisitState save(const char* path) const {
        constexpr size_t blockSize = 256;
        using storage::EnqueueStatus;
        using storage::WriteStatus;
        storage::PcapCapture<1, blockSize> capture(path);
        if (!capture.open()) {
            return VisitState::storage_error;
        }

        // Classic PCAP 2.4 with one raw IEEE 802.11 record and microsecond timestamps.
        uint8_t header[pcapHeaderSize + recordHeaderSize]{0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4};
        writeLe32(header + 16, maxFrameSize);
        writeLe32(header + 20, 105);
        writeLe32(header + 24, timestampUs_ / 1000000U);
        writeLe32(header + 28, timestampUs_ % 1000000U);
        const auto length = static_cast<uint32_t>(frameSize_);
        writeLe32(header + 32, length);
        writeLe32(header + 36, length);

        if (capture.enqueue(header, sizeof(header)) != EnqueueStatus::queued ||
            !drainBlock(capture)) {
            return VisitState::storage_error;
        }
        for (size_t offset = 0; offset < frameSize_;) {
            const size_t remaining = frameSize_ - offset;
            const size_t count = remaining < blockSize ? remaining : blockSize;
            if (capture.enqueue(frame_ + offset, count) != EnqueueStatus::queued ||
                !drainBlock(capture)) {
                return VisitState::storage_error;
            }
            offset += count;
        }
        return capture.finish() == storage::CaptureState::saved ? VisitState::saved
                                                                : VisitState::storage_error;
    }

    template <typename Capture> static bool drainBlock(Capture& capture) {
        auto result = capture.drainOne();
        while (result == storage::WriteStatus::partial) {
            result = capture.drainOne();
        }
        return result == storage::WriteStatus::complete;
    }

    Target target_{};
    VisitState state_ = VisitState::idle;
    std::atomic<bool> hasSample_{false};
    uint8_t frame_[maxFrameSize]{};
    size_t frameSize_ = 0;
    uint32_t timestampUs_ = 0;
};

} // namespace session
} // namespace airrounds
