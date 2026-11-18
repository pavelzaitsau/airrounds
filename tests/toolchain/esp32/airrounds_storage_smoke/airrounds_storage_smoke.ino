#include <Arduino.h>
#include <SD.h>

#include "sd_file.h"
#include "write_queue.h"

using airrounds::storage::EnqueueStatus;
using airrounds::storage::SdFile;
using airrounds::storage::WriteQueue;
using airrounds::storage::WriteStatus;

static volatile WriteStatus result;

static WriteStatus writeTestBlock() {
    SdFile file;
    if (!file.openNew("/sd/airrounds-storage-check.bin")) {
        return WriteStatus::unavailable;
    }

    WriteQueue<2, 32, SdFile> queue(file);
    const uint8_t block[] = {0x41, 0x52};
    if (queue.enqueue(block, sizeof(block)) != EnqueueStatus::queued) {
        return WriteStatus::invalid;
    }

    return queue.drainOne();
}

void setup() {
    if (!SD.begin()) {
        result = WriteStatus::unavailable;
        return;
    }
    result = writeTestBlock();
    SD.end();
}

void loop() {
    delay(1000);
}
