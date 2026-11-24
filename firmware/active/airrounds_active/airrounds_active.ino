#include <Arduino.h>

#include "active_eapol_mode.h"
#include "deauth_tx.h"
#include "passive_visit.h"

using airrounds::active::ActiveEapolMode;
using airrounds::active::DeauthTransmitter;
using airrounds::uart::PassiveVisit;
using airrounds::uart::Protocol;
using airrounds::uart::randomId;

// The opt-in active build. It is the passive firmware plus the one transmit path in
// firmware/active. It injects the transmitter and the active capture mode, and opens the radio in
// WIFI_MODE_STA so the interface exists for the deauthentication transmit. The default build does
// none of this and stays receive-only. See docs/t12-architecture.md.
void setup() {
    Serial0.begin(115200, SERIAL_8N1, RX, TX);
    static DeauthTransmitter transmitter;
    static ActiveEapolMode activeMode;
    auto& visit = PassiveVisit::instance();
    visit.setTransmitter(&transmitter);
    visit.setActiveMode(&activeMode);
    visit.setRadioMode(WIFI_MODE_STA);
}

void loop() {
    static const uint64_t boot = randomId();
    auto& visit = PassiveVisit::instance();
    static Protocol protocol(visit, boot == 0 ? 1 : boot, &visit);

    // Keep the observation deadline and SD completion independent of UART traffic.
    constexpr size_t budget = 128;
    for (size_t count = 0; count < budget && Serial0.available() > 0; ++count) {
        const int value = Serial0.read();
        char response[Protocol::replyCapacity];
        if (value >= 0 && protocol.receive({static_cast<uint8_t>(value), millis()}, response)) {
            Serial0.write(response);
        }
    }

    protocol.poll();
    delay(1);
}
