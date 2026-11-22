// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <esp_wifi.h>

#include "capture_mode.h"

// Stock ESP-IDF rejects a raw 802.11 frame whose source address is not the interface MAC, through
// ieee80211_raw_frame_sanity_check. A deauthentication spoofs the source as the access point BSSID,
// so the stock check fails it and the transmit call returns an error. Overriding the check to pass
// lets the active build send the targeted deauthentication frame. This symbol exists only in the
// opt-in active build, in firmware/active, so the default build is unaffected. The ESP-IDF defines
// the same symbol non-weak in libnet80211.a, so the link needs -Wl,-zmuldefs for this override to
// win; tools/quality/embedded.py sets that flag for the active build only. See docs/t12-architecture.md.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t /*arg*/, int32_t /*arg2*/, int32_t /*arg3*/) {
    return 0;
}

namespace airrounds {
namespace active {

// The sole raw-transmit path in AirRounds. It exists only in the opt-in active build, in this
// directory, outside the default receive-only gate. send transmits one prebuilt deauthentication
// frame through the station interface. The active build opens the radio in WIFI_MODE_STA, so the
// interface exists for the transmit. See docs/t12-architecture.md and docs/acceptable-use.md,
// "Active operation".
class DeauthTransmitter final : public session::Transmitter {
public:
    bool send(const uint8_t* frame, size_t len) override {
        // en_sys_seq true: the radio assigns an incrementing sequence number to each frame. The built
        // frame leaves the sequence field zero, so without this the burst frames would share
        // sequence 0 and a client could discard the repeats as duplicates (802.11 duplicate
        // detection keys on address and sequence).
        return esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true) == ESP_OK;
    }
};

} // namespace active
} // namespace airrounds
