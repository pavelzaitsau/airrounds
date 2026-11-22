#pragma once

#include <string.h>

#include "pcap_capture.h"

namespace airrounds {
namespace storage {

struct PcapRecord {
    const uint8_t* frame;
    size_t size;
    uint32_t timestampUs;
};

// The frames one visit saves, in capture order: one beacon, or a chain of related frames.
class RecordSource {
public:
    virtual size_t recordCount() const = 0;
    virtual PcapRecord record(size_t index) const = 0;

protected:
    ~RecordSource() = default;
};

struct RecordReceipt {
    bool saved;
    uint64_t bytesWritten;
    uint32_t queueOverflows;
};

constexpr size_t pcapFileHeaderSize = 24;
constexpr size_t pcapRecordHeaderSize = 16;
constexpr size_t managementHeaderSize = 24;
constexpr size_t shortestFrameSize = 10; // An ACK or CTS frame without its FCS.
// The largest IEEE 802.11 frame body the radio delivers; the snap length of every file.
constexpr uint32_t pcapSnapLength = 2304;

// IEEE 802.11 frame types as a mask. A capture mode declares the types it needs.
constexpr uint8_t managementFrames = 1U << 0U;
constexpr uint8_t controlFrames = 1U << 1U;
constexpr uint8_t dataFrames = 1U << 2U;
constexpr uint8_t allFrameTypes = 0x07U;
static_assert(allFrameTypes == (1U << 0U) + (1U << 1U) + (1U << 2U), "every frame type bit");

// Frame control bits 2 and 3 carry the type: 0 management, 1 control, 2 data, 3 extension.
inline uint8_t frameTypeOf(const uint8_t* frame, size_t size) {
    if (frame == nullptr || size < shortestFrameSize) {
        return 0;
    }
    const auto type = static_cast<unsigned>(frame[0] >> 2U) & 3U;
    if (type == 0 && size < managementHeaderSize) {
        return 0;
    }
    return type == 3 ? uint8_t{0} : static_cast<uint8_t>(1U << type);
}

inline bool frameAllowed(const uint8_t* frame, size_t size, uint8_t allowedTypes) {
    return (frameTypeOf(frame, size) & allowedTypes) != 0;
}

// A second, independent EAPOL gate for the writer, keyed off the declared data type. It re-derives
// the 802.11 header length, the LLC/SNAP header, and the ethertype locally, and must not include
// session/eapol.h: storage must not depend on session, and the two gates stay independent only
// while the writer keeps its own code path. It does not classify the key-info, so a defect in the
// mode's parser cannot bypass it. See docs/t11.md, "Writer defence in depth".
inline bool containsEapolEthertype(const uint8_t* frame, size_t size) {
    if (frame == nullptr || size < managementHeaderSize) {
        return false;
    }
    // Data frames only; a Protected body is ciphertext; a 4-address WDS frame has no LLC here.
    if ((frame[0] & 0x0cU) != 0x08U || (frame[1] & 0x40U) != 0 || (frame[1] & 0x03U) == 0x03U) {
        return false;
    }
    size_t header = managementHeaderSize;
    if ((frame[0] & 0x80U) != 0) {
        header += 2; // QoS Control.
        if ((frame[1] & 0x80U) != 0) {
            header += 4; // HT Control.
        }
    }
    if (size < header + 8) { // LLC/SNAP (6) plus the 2-byte ethertype.
        return false;
    }
    static const uint8_t llcSnap[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
    return memcmp(frame + header, llcSnap, sizeof(llcSnap)) == 0 && frame[header + 6] == 0x88 &&
           frame[header + 7] == 0x8e;
}

inline size_t pcapRecordFileSize(size_t frameSize) {
    return pcapFileHeaderSize + pcapRecordHeaderSize + frameSize;
}

inline size_t pcapFileSize(const RecordSource& records) {
    size_t size = pcapFileHeaderSize;
    for (size_t index = 0; index < records.recordCount(); ++index) {
        size += pcapRecordHeaderSize + records.record(index).size;
    }
    return size;
}

namespace detail {

constexpr size_t recordBlockSize = 256;
using RecordCapture = PcapCapture<1, recordBlockSize>;

inline bool drainRecordBlock(RecordCapture& capture) {
    auto result = capture.drainOne();
    while (result == WriteStatus::partial) {
        result = capture.drainOne();
    }
    return result == WriteStatus::complete;
}

inline bool enqueueBlock(RecordCapture& capture, const uint8_t* data, size_t size) {
    return capture.enqueue(data, size) == EnqueueStatus::queued && drainRecordBlock(capture);
}

inline bool enqueueRecord(RecordCapture& capture, const PcapRecord& record) {
    uint8_t header[pcapRecordHeaderSize]{};
    writeLittle(header, record.timestampUs / 1000000U);
    writeLittle(header + 4, record.timestampUs % 1000000U);
    const auto length = static_cast<uint32_t>(record.size);
    writeLittle(header + 8, length);
    writeLittle(header + 12, length);
    if (!enqueueBlock(capture, header, sizeof(header))) {
        return false;
    }
    for (size_t offset = 0; offset < record.size;) {
        const size_t remaining = record.size - offset;
        const size_t count = remaining < recordBlockSize ? remaining : recordBlockSize;
        if (!enqueueBlock(capture, record.frame + offset, count)) {
            return false;
        }
        offset += count;
    }
    return true;
}

inline bool enqueueRecords(RecordCapture& capture, const RecordSource& records) {
    // Classic PCAP 2.4 with raw IEEE 802.11 records and microsecond timestamps.
    uint8_t header[pcapFileHeaderSize]{0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4};
    writeLittle(header + 16, pcapSnapLength);
    writeLittle(header + 20, uint32_t{105});
    if (!enqueueBlock(capture, header, sizeof(header))) {
        return false;
    }
    for (size_t index = 0; index < records.recordCount(); ++index) {
        if (!enqueueRecord(capture, records.record(index))) {
            return false;
        }
    }
    return true;
}

inline bool recordsAllowed(const RecordSource& records, uint8_t allowedTypes) {
    if (records.recordCount() == 0) {
        return false;
    }
    const bool eapolGate = (allowedTypes & dataFrames) != 0;
    for (size_t index = 0; index < records.recordCount(); ++index) {
        const auto record = records.record(index);
        if (!frameAllowed(record.frame, record.size, allowedTypes) ||
            record.size > pcapSnapLength) {
            return false;
        }
        // A data mode saves EAPOL frames only. Every data record must carry the EAPOL ethertype,
        // whatever other types the mode declared; management and control records are unaffected.
        if (eapolGate && (frameTypeOf(record.frame, record.size) & dataFrames) != 0 &&
            !containsEapolEthertype(record.frame, record.size)) {
            return false;
        }
    }
    return true;
}

} // namespace detail

// Saves the records as one sealed PCAP and verifies the closed file. Every capture mode saves
// through this writer with the frame types it declared, so a mode cannot save a frame type it
// was not admitted for: the writer refuses the whole set before it creates the file.
inline RecordReceipt writePcapRecords(const char* path, const RecordSource& records,
                                      uint8_t allowedTypes) {
    if (path == nullptr || !detail::recordsAllowed(records, allowedTypes)) {
        return {false, 0, 0};
    }
    detail::RecordCapture capture(path);
    const bool saved = capture.open() && detail::enqueueRecords(capture, records) &&
                       capture.finish() == CaptureState::saved;
    return {saved, capture.bytesAccepted(), capture.overflowCount()};
}

class SingleRecord final : public RecordSource {
public:
    explicit SingleRecord(const PcapRecord& record) : record_(record) {}
    size_t recordCount() const override { return 1; }
    PcapRecord record(size_t /*index*/) const override { return record_; }

private:
    PcapRecord record_;
};

inline RecordReceipt writePcapRecord(const char* path, const uint8_t* frame, size_t size,
                                     uint32_t timestampUs, uint8_t allowedTypes) {
    return writePcapRecords(path, SingleRecord({frame, size, timestampUs}), allowedTypes);
}

} // namespace storage
} // namespace airrounds
