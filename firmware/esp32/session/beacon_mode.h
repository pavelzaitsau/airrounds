#pragma once

#include "capture_mode.h"
#include "passive_session.h"

namespace airrounds {
namespace session {

// The T8 beacon capture behind the capture-mode contract. PassiveSession keeps the beacon
// parser and the adaptive window unchanged; this class only adapts its interface.
class BeaconMode final : public CaptureMode {
public:
    CaptureModeId id() const override { return CaptureModeId::beacon; }
    RadioNeeds needs() const override { return {storage::managementFrames}; }

    void reset() override { session_.reset(); }

    bool begin(const Target& target, uint64_t budgetUs) override {
        return session_.startAdaptive(target, budgetUs) == StartResult::started;
    }

    bool onFrame(const uint8_t* frame, size_t size, uint8_t channel, uint64_t elapsedUs) override {
        return session_.receive(frame, size, channel, elapsedUs);
    }

    bool done(uint64_t elapsedUs) const override { return session_.observationDone(elapsedUs); }

    ModeResult finish() override {
        return session_.hasSample() ? ModeResult::found : ModeResult::not_observed;
    }

    size_t recordCount() const override { return session_.hasSample() ? 1 : 0; }

    storage::PcapRecord record(size_t /*index*/) const override {
        return {session_.sampleFrame(), session_.sampleSize(), session_.sampleTimestampUs()};
    }

private:
    PassiveSession session_;
};

} // namespace session
} // namespace airrounds
