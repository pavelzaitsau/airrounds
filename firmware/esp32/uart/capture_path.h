#pragma once

#include <inttypes.h>
#include <stdio.h>

#include "passive_round.h"

namespace airrounds {
namespace uart {

inline bool capturePath(char* path, size_t size, const char* directory, const CaptureId& id) {
    const int count = id.slot == 0
                          ? snprintf(path, size, "%s/visit-%016" PRIx64 "-%08" PRIx32 ".pcap",
                                     directory, id.visit.boot, id.visit.session)
                          : snprintf(path, size, "%s/visit-%016" PRIx64 "-%08" PRIx32 "-%u.pcap",
                                     directory, id.visit.boot, id.visit.session, id.slot);
    return count > 0 && static_cast<size_t>(count) < size;
}

} // namespace uart
} // namespace airrounds
