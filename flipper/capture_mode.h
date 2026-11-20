#pragma once

#include <stddef.h>
#include <stdint.h>

namespace airrounds {
namespace flipper {

// How a mode counts a point as a success. A frame-found mode succeeds on a saved capture; an
// absence mode succeeds when the board confirms that the frames were absent. The neutral entry
// for an unknown mode counts both as success.
enum class SuccessMeaning : uint8_t { packet_found, absence_confirmed, any_result };

struct CaptureModeInfo {
    const char* name;
    const char* listTitle;
    const char* rowSuccess;
    const char* rowMiss;
    const char* listening;
    const char* saving;
    const char* counter; // printf format with the success count and the point count.
    const char* resultSuccess;
    const char* resultMiss;
    const char* detailSuccess;
    const char* detailMiss;
    SuccessMeaning meaning;
    uint8_t wireId; // Two hex digits once the board reports modes; Beacon is 00.
    bool implied;   // Available on every board, also one without the capture-mode capability.
    bool implemented;
};

// The mode IDs a board lists in its reply to the reserved `modes` read command. A board
// without the capture-mode capability lists none, and only Beacon is available on it.
struct BoardModes {
    uint8_t count = 0;
    uint8_t ids[8]{};

    bool lists(uint8_t id) const {
        for (size_t index = 0; index < count && index < sizeof(ids); ++index) {
            if (ids[index] == id) {
                return true;
            }
        }
        return false;
    }
};

// The order of the Capture mode screen. Todo A and Todo B hold the place of future modes; their
// labels repeat the neutral ones until a real mode defines its own.
constexpr CaptureModeInfo captureModes[]{
    {"Beacon", "Beacons", "Saved", "Not seen", "Listening", "Saving", "Captured %u/%u", "Captured",
     "Not seen", "Captured to board SD", "Point not seen", SuccessMeaning::packet_found, 0x00, true,
     true},
    {"Todo A", "Points", "Done", "No result", "Listening", "Saving", "Done %u/%u", "Done",
     "No result", "Done", "No result", SuccessMeaning::any_result, 0x01, false, false},
    {"Todo B", "Points", "Done", "No result", "Listening", "Saving", "Done %u/%u", "Done",
     "No result", "Done", "No result", SuccessMeaning::any_result, 0x02, false, false},
};
constexpr size_t captureModeCount = sizeof(captureModes) / sizeof(captureModes[0]);
constexpr size_t beaconModeIndex = 0;

constexpr CaptureModeInfo neutralMode{
    "Unknown",    "Points", "Done",      "No result", "Listening", "Saving",
    "Done %u/%u", "Done",   "No result", "Done",      "No result", SuccessMeaning::any_result,
    0xff,         false,    false};

inline const CaptureModeInfo& beaconMode() {
    return captureModes[beaconModeIndex];
}

inline bool modeAvailable(const CaptureModeInfo& mode, const BoardModes& board) {
    return mode.implemented && (mode.implied || board.lists(mode.wireId));
}

// Labels for a round come from the mode the board reports, not from the local choice. An ID the
// app does not know gets the neutral labels.
inline const CaptureModeInfo& modeOf(uint8_t reportedWireId) {
    // The Flipper app uses no C++ standard library algorithms.
    for (const auto& mode : captureModes) {
        // cppcheck-suppress useStlAlgorithm
        if (mode.wireId == reportedWireId) {
            return mode;
        }
    }
    return neutralMode;
}

} // namespace flipper
} // namespace airrounds
