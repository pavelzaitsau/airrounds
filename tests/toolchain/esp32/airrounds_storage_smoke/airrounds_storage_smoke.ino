#include <Arduino.h>
#include <FS.h>

#include "pending_write.h"

static volatile airrounds::storage::WriteStatus result;

void setup() {
    fs::File file;
    const uint8_t byte = 0;
    size_t accepted = 0;
    result = airrounds::storage::writePending(file, &byte, sizeof(byte), accepted);
}

void loop() {
    delay(1000);
}
