#include <Arduino.h>
#include <SD.h>

#include "pcap_capture.h"

using airrounds::storage::CaptureState;
using airrounds::storage::EnqueueStatus;
using airrounds::storage::PcapCapture;
using airrounds::storage::recoverPcap;

static const char capturePath[] = "/sd/airrounds-pcap-check.pcap";
static volatile CaptureState result;

static CaptureState finishTestCapture() {
    const uint8_t sample[] = {
        0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0xff, 0xff,
        0,    0,    1,    0,    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 14, 0, 0,    0,
        14,   0,    0,    0,    2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0,  2, 0x88, 0xb5,
    };

    PcapCapture<2, 64> capture(capturePath);
    if (!capture.open() || capture.enqueue(sample, sizeof(sample)) != EnqueueStatus::queued) {
        return CaptureState::error;
    }
    return capture.finish();
}

void setup() {
    if (!SD.begin()) {
        result = CaptureState::error;
        return;
    }
    result = recoverPcap(capturePath);
    if (result == CaptureState::missing) {
        result = finishTestCapture();
    }
    SD.end();
}

void loop() {
    delay(1000);
}
