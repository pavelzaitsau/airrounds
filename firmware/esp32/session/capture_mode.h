#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pcap_record.h"
#include "target.h"

namespace airrounds {
namespace session {

// Two hex digits on the wire once the capture-mode command is built; Beacon is 00.
enum class CaptureModeId : uint8_t { beacon = 0 };

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

protected:
    ~CaptureMode() = default;
};

} // namespace session
} // namespace airrounds
