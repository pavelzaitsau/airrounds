#pragma once

#include <stdint.h>
#include <string.h>

namespace airrounds {
namespace session {

struct Target {
    uint8_t bssid[6];
    uint8_t channel;
};

inline bool validTarget(const Target& target) {
    constexpr uint8_t zeroAddress[6]{};
    return target.channel >= 1 && target.channel <= 13 && (target.bssid[0] & 1U) == 0 &&
           memcmp(target.bssid, zeroAddress, sizeof(zeroAddress)) != 0;
}

} // namespace session
} // namespace airrounds
