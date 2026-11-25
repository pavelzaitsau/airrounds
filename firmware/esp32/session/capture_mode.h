#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pcap_record.h"
#include "target.h"

namespace airrounds {
namespace session {

// Two hex digits on the wire once the capture-mode command is built; Beacon is 00, EAPOL is 01,
// the active mode is 02. The active mode exists only in the opt-in active build.
enum class CaptureModeId : uint8_t { beacon = 0, eapol = 1, active = 2 };

// found maps to point result 2 after the shared writer verifies the file, not_observed to 1,
// error to 3. absence_confirmed keeps result 1 in the current replies: digit 5 is reserved for
// a later read command. See docs/capture-modes-design.md.
enum class ModeResult { found, absence_confirmed, not_observed, error };

// The frame types the radio delivers to a mode and the shared writer may save for it, as
// storage::managementFrames, controlFrames, and dataFrames bits. Widening a mode beyond management
// frames needs an admission decision in docs/capture-modes-design.md.
struct RadioNeeds {
    uint8_t frameTypes;
};

// A transmit sink for an active mode. The default build injects none, so transmit stays absent
// there. The active build injects one whose implementation holds the sole raw transmit call, in
// firmware/active/, outside the default receive-only gate. See docs/t12-architecture.md.
class Transmitter {
public:
    virtual bool send(const uint8_t* frame, size_t len) = 0;

protected:
    ~Transmitter() = default;
};

// The round-scoped transmit cap, enforced in the shared layer so a mode defect cannot exceed it.
// PassiveRound re-seeds it at every round start; a visit never resets it. tryConsume counts an
// attempt, so a failed send still spends budget, which bounds total transmit attempts per round.
class TransmitBudget {
public:
    void seed(uint32_t frames) { remaining_ = frames; }

    bool tryConsume() {
        if (remaining_ == 0) {
            return false;
        }
        --remaining_;
        return true;
    }

    uint32_t remaining() const { return remaining_; }

private:
    uint32_t remaining_ = 0;
};

// What PassiveVisit hands a mode in poll(), for work outside the receive callback. transmit sends
// one frame under the round cap; it returns false with no transmitter or an empty budget, so the
// default build, which injects no transmitter, never transmits.
struct ModeContext {
    // An aggregate, so a C++11 brace-init sets both. Every caller passes a transmitter (null in the
    // default build) and a budget.
    Transmitter* transmitter;
    TransmitBudget* budget;

    bool transmit(const uint8_t* frame, size_t len) {
        if (transmitter == nullptr || budget == nullptr || !budget->tryConsume()) {
            return false;
        }
        return transmitter->send(frame, len);
    }
};

// What a capture mode decides during one visit. The contract has no transmit operation: a mode
// only judges frames that the receive-only radio delivers, and the shared writer saves them.
// After found, the inherited record list holds one frame or a chain of frames, in capture
// order, which the writer saves as one PCAP. The mode owns the frame buffers and bounds them.
class CaptureMode : public storage::RecordSource {
public:
    virtual CaptureModeId id() const = 0;
    virtual RadioNeeds needs() const = 0;

    // Clears the previous visit. The caller detaches the radio and finishes any save first.
    virtual void reset() = 0;

    // Arms the visit under the receive lock. budgetUs is the point's share of the round.
    virtual bool begin(const Target& target, uint64_t budgetUs) = 0;

    // Runs in the Wi-Fi receive callback under the receive lock, so it must not allocate,
    // write to the card, log, or block. Returns true for a frame the mode accepted.
    virtual bool onFrame(const uint8_t* frame, size_t size, uint8_t channel,
                         uint64_t elapsedUs) = 0;

    // Serialized with onFrame by the same lock.
    virtual bool done(uint64_t elapsedUs) const = 0;

    // Called once reception has stopped. found means the record list holds the frames to save.
    virtual ModeResult finish() = 0;

    // Work outside the receive callback (step R6): an active mode drives its timed transmit from
    // here, never from onFrame, which runs under the receive lock and must not block or transmit.
    // The default does nothing, so a passive mode needs no override. A mode that shares state with
    // onFrame coordinates the two itself, because poll runs on the UART task, not under that lock.
    virtual void poll(uint64_t /*nowUs*/, ModeContext& /*ctx*/) {}

    // The journal transmit audit. The defaults report no transmit, so a passive mode needs no
    // override and the default build's journal records nothing transmitted. transmitTarget returns
    // six bytes of the target client, or null when the mode transmitted nothing.
    virtual uint32_t transmittedFrames() const { return 0; }
    virtual uint16_t transmitReason() const { return 0; }
    virtual const uint8_t* transmitTarget() const { return nullptr; }

protected:
    ~CaptureMode() = default;
};

} // namespace session
} // namespace airrounds
