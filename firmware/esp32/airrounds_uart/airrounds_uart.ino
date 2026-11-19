#include <Arduino.h>

#include "passive_visit.h"

using airrounds::uart::PassiveVisit;
using airrounds::uart::Protocol;
using airrounds::uart::randomId;

void setup() {
    Serial0.begin(115200, SERIAL_8N1, RX, TX);
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
