#pragma once

#include <atomic>

#include "pcap_capture.h"
#include "target.h"

namespace airrounds {
namespace session {

enum class StartResult { started, empty_list, invalid_target, busy };
enum class VisitState { idle, observing, not_observed, saved, storage_error };

struct CaptureStatistics {
    uint64_t bytesWritten;
    uint32_t queueOverflows;
};

struct BeaconView {
    const uint8_t* bssid = nullptr;
    const uint8_t* ssid = nullptr;
    size_t ssidSize = 0;
};

constexpr size_t maxBeaconSize = 2304;

inline bool parseBeacon(uint8_t channel, const uint8_t* frame, size_t size, BeaconView& beacon) {
    constexpr size_t ssidIdOffset = 36;
    constexpr size_t ssidValueOffset = ssidIdOffset + 2;
    constexpr uint8_t broadcast[6]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (frame == nullptr || size < ssidValueOffset || size > maxBeaconSize || frame[0] != 0x80 ||
        (frame[1] & 0xc7U) != 0 || (frame[22] & 0x0fU) != 0 ||
        memcmp(frame + 4, broadcast, sizeof(broadcast)) != 0 || (frame[16] & 1U) != 0 ||
        memcmp(frame + 10, frame + 16, 6) != 0) {
        return false;
    }

    // A complete 2.4 GHz beacon starts its tagged elements with SSID and Supported Rates.
    const size_t ssidSize = frame[ssidIdOffset + 1];
    const size_t ratesOffset = ssidValueOffset + ssidSize;
    if (frame[ssidIdOffset] != 0 || ssidSize > 32 || size < ratesOffset + 2 ||
        frame[ratesOffset] != 1 || frame[ratesOffset + 1] == 0 || frame[ratesOffset + 1] > 8) {
        return false;
    }
    for (size_t offset = ratesOffset; offset < size;) {
        if (size - offset < 2) {
            return false;
        }
        const uint8_t id = frame[offset++];
        const size_t length = frame[offset++];
        if (length > size - offset || id == 0) {
            return false;
        }
        if ((id == 3 && length != 1) || (id == 61 && length != 22) ||
            ((id == 3 || id == 61) && frame[offset] != channel)) {
            return false;
        }
        offset += length;
    }
    const uint8_t zeroAddress[6]{};
    if (memcmp(frame + 16, zeroAddress, sizeof(zeroAddress)) == 0) {
        return false;
    }
    beacon.bssid = frame + 16;
    beacon.ssid = frame + ssidValueOffset;
    beacon.ssidSize = ssidSize;
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
    enum : uint32_t {
        windowUs = 5000000,
        extensionUs = 1000000,
        maxAdaptiveWindowUs = 7000000,
        maxFrameSize = maxBeaconSize
    };

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

    StartResult startAdaptive(const Target& target, uint64_t budgetUs) {
        const auto result = start(&target, 1);
        if (result == StartResult::started) {
            adaptive_ = true;
            limitUs_ = budgetUs < maxAdaptiveWindowUs ? static_cast<uint32_t>(budgetUs)
                                                      : maxAdaptiveWindowUs;
            deadlineUs_ = limitUs_ < windowUs ? limitUs_ : windowUs;
        }
        return result;
    }

    bool observationDone(uint64_t elapsedUs) const {
        return elapsedUs >= deadlineUs_ || (!adaptive_ && hasSample());
    }

    // Serialize receive and observationDone. Stop delivery before calling finish.
    bool receive(const uint8_t* frame, size_t size, uint8_t receivedChannel, uint64_t elapsedUs) {
        BeaconView beacon;
        if (state_ != VisitState::observing || observationDone(elapsedUs) ||
            receivedChannel != target_.channel || frame == nullptr || size < 22 ||
            memcmp(frame + 16, target_.bssid, sizeof(target_.bssid)) != 0 ||
            !parseBeacon(receivedChannel, frame, size, beacon)) {
            return false;
        }
        if (adaptive_) {
            noteObservation(frame + 32, beacon, static_cast<uint32_t>(elapsedUs));
        }
        if (!hasSample()) {
            memcpy(frame_, frame, size);
            frameSize_ = size;
            timestampUs_ = static_cast<uint32_t>(elapsedUs);
            hasSample_.store(true, std::memory_order_release);
        }
        return true;
    }

    // Reset only after the radio detaches and any save has finished.
    void reset() {
        target_ = {};
        frameSize_ = 0;
        timestampUs_ = 0;
        adaptive_ = false;
        deadlineUs_ = windowUs;
        observationCount_ = 0;
        captureStatistics_ = {};
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

    const CaptureStatistics& captureStatistics() const { return captureStatistics_; }

    size_t savedPcapSize() const {
        return state_ == VisitState::saved ? pcapHeaderSize + recordHeaderSize + frameSize_ : 0;
    }

private:
    static constexpr size_t pcapHeaderSize = 24;
    static constexpr size_t recordHeaderSize = 16;

    struct Observation {
        uint8_t parameters[4]; // Beacon interval and capability flags, in wire order.
        uint8_t ssid[32];
        size_t ssidSize;
    };

    void noteObservation(const uint8_t* parameters, const BeaconView& beacon, uint32_t elapsedUs) {
        if (observationCount_ == observationCapacity || deadlineUs_ == limitUs_) {
            return;
        }
        for (size_t i = 0; i < observationCount_; ++i) {
            const auto& seen = observations_[i];
            if (memcmp(seen.parameters, parameters, sizeof(seen.parameters)) == 0 &&
                seen.ssidSize == beacon.ssidSize &&
                memcmp(seen.ssid, beacon.ssid, beacon.ssidSize) == 0) {
                return;
            }
        }
        auto& observation = observations_[observationCount_++];
        memcpy(observation.parameters, parameters, sizeof(observation.parameters));
        memcpy(observation.ssid, beacon.ssid, beacon.ssidSize);
        observation.ssidSize = beacon.ssidSize;

        const uint32_t proposed = elapsedUs + extensionUs;
        if (proposed > deadlineUs_) {
            deadlineUs_ = proposed < limitUs_ ? proposed : limitUs_;
        }
    }

    static void writeLe32(uint8_t* field, uint32_t value) {
        for (size_t index = 0; index < sizeof(value); ++index) {
            field[index] = static_cast<uint8_t>(value);
            value >>= 8U;
        }
    }

    static constexpr size_t blockSize = 256;
    using Capture = storage::PcapCapture<1, blockSize>;

    VisitState save(const char* path) {
        Capture capture(path);
        const bool saved = capture.open() && writePcap(capture) &&
                           capture.finish() == storage::CaptureState::saved;
        captureStatistics_ = {capture.bytesAccepted(), capture.overflowCount()};
        return saved ? VisitState::saved : VisitState::storage_error;
    }

    bool writePcap(Capture& capture) const {
        using storage::EnqueueStatus;

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
            return false;
        }
        for (size_t offset = 0; offset < frameSize_;) {
            const size_t remaining = frameSize_ - offset;
            const size_t count = remaining < blockSize ? remaining : blockSize;
            if (capture.enqueue(frame_ + offset, count) != EnqueueStatus::queued ||
                !drainBlock(capture)) {
                return false;
            }
            offset += count;
        }
        return true;
    }

    static bool drainBlock(Capture& capture) {
        auto result = capture.drainOne();
        while (result == storage::WriteStatus::partial) {
            result = capture.drainOne();
        }
        return result == storage::WriteStatus::complete;
    }

    static constexpr size_t observationCapacity = 4;
    Observation observations_[observationCapacity]{};
    size_t observationCount_ = 0;
    uint32_t deadlineUs_ = windowUs;
    uint32_t limitUs_ = 0;
    bool adaptive_ = false;
    Target target_{};
    VisitState state_ = VisitState::idle;
    CaptureStatistics captureStatistics_{};
    std::atomic<bool> hasSample_{false};
    uint8_t frame_[maxFrameSize]{};
    size_t frameSize_ = 0;
    uint32_t timestampUs_ = 0;
};

} // namespace session
} // namespace airrounds
