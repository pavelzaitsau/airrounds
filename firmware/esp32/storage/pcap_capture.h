#pragma once

#include <string>

#include "pcap_format.h"
#include "sd_file.h"
#include "write_queue.h"

namespace airrounds {
namespace storage {

enum class CaptureState { missing, incomplete, saved, error };

namespace detail {

constexpr uint8_t sealMagic[] = {'A', 'R', 'P', 'C', 'A', 'P', 1, 0};
struct Seal {
    uint64_t size;
    uint32_t crc;
};

constexpr size_t sizeOffset = sizeof(sealMagic);
constexpr size_t crcOffset = sizeOffset + sizeof(uint64_t);
constexpr size_t sealSize = crcOffset + sizeof(uint32_t);

template <typename Number> void writeLittle(uint8_t* output, Number value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<uint8_t>(value);
        value >>= 8U;
    }
}

template <typename File> class PcapOutput : public File {
public:
    // Static template dispatch intentionally wraps File::write to update the CRC.
    // cppcheck-suppress duplInheritedMember
    size_t write(const uint8_t* data, size_t size) {
        const auto written = File::write(data, size);
        if (written <= size) {
            crc_.update(data, written);
        }
        return written;
    }

    uint32_t crc() const { return crc_.value(); }

private:
    pcap::Crc32 crc_;
};

template <typename File> bool checkClosedPcap(const char* path, const Seal& seal) {
    File input;
    if (!input.openRead(path)) {
        return false;
    }
    const bool valid = pcap::validate(input, seal.size, seal.crc);
    return input.close() && valid;
}

template <typename File> bool readSeal(const std::string& path, Seal& seal) {
    File input;
    uint8_t record[sealSize];
    if (!input.openRead(path.c_str()) || !pcap::readExact(input, record, sizeof(record)) ||
        memcmp(record, sealMagic, sizeof(sealMagic)) != 0) {
        return false;
    }
    uint8_t extra;
    const bool atEnd = input.read(&extra, 1) == 0;
    if (!input.close() || !atEnd) {
        return false;
    }
    seal.size = 0;
    for (size_t index = crcOffset; index > sizeOffset; --index) {
        seal.size = (seal.size << 8U) | record[index - 1];
    }
    seal.crc = pcap::readNumber(record + crcOffset, sizeof(seal.crc), true);
    return true;
}

template <typename File> bool publishSeal(const std::string& path, const Seal& proof) {
    const auto finalPath = path + ".ok";
    const auto stagePath = finalPath + ".part";
    File seal;
    if (!seal.openNew(stagePath.c_str())) {
        return false;
    }

    uint8_t record[sealSize];
    memcpy(record, sealMagic, sizeof(sealMagic));
    writeLittle(record + sizeOffset, proof.size);
    writeLittle(record + crcOffset, proof.crc);
    size_t accepted = 0;
    const bool written =
        writePending(seal, record, sizeof(record), accepted) == WriteStatus::complete &&
        seal.lastError() == 0;
    const bool synced = written && seal.sync();
    const bool closed = seal.close();
    Seal confirmed{};
    return synced && closed && readSeal<File>(stagePath, confirmed) &&
           confirmed.size == proof.size && confirmed.crc == proof.crc &&
           File::publish(stagePath.c_str(), finalPath.c_str());
}

} // namespace detail

constexpr size_t pcapReceiptSize = detail::sealSize;

template <typename File = SdFile> CaptureState recoverPcap(const char* path) {
    if (path == nullptr || *path == '\0') {
        return CaptureState::error;
    }
    const std::string sealPath = std::string(path) + ".ok";
    const auto data = File::probe(path);
    const auto seal = File::probe(sealPath.c_str());
    const auto stage = File::probe((sealPath + ".part").c_str());
    if (data == FilePresence::error || seal == FilePresence::error ||
        stage == FilePresence::error) {
        return CaptureState::error;
    }
    if (stage == FilePresence::present) {
        return CaptureState::incomplete;
    }
    if (seal == FilePresence::present) {
        detail::Seal proof{};
        return data == FilePresence::present && detail::readSeal<File>(sealPath, proof) &&
                       detail::checkClosedPcap<File>(path, proof)
                   ? CaptureState::saved
                   : CaptureState::error;
    }
    return data == FilePresence::present ? CaptureState::incomplete : CaptureState::missing;
}

// Stop and join the producer before finishing or destroying the capture.
template <size_t Capacity, size_t MaxBlockSize, typename File = SdFile> class PcapCapture {
public:
    explicit PcapCapture(const char* path) : path_(path ? path : ""), queue_(file_) {}

    bool open() {
        if (attempted_) {
            return false;
        }
        attempted_ = true;
        const auto sealPath = path_ + ".ok";
        active_ = !path_.empty() && File::probe(sealPath.c_str()) == FilePresence::missing &&
                  File::probe((sealPath + ".part").c_str()) == FilePresence::missing &&
                  file_.openNew(path_.c_str());
        result_ = active_ ? CaptureState::incomplete : CaptureState::error;
        return active_;
    }

    EnqueueStatus enqueue(const uint8_t* data, size_t size) {
        return active_ ? queue_.enqueue(data, size) : EnqueueStatus::invalid;
    }

    WriteStatus drainOne() { return active_ ? queue_.drainOne() : WriteStatus::unavailable; }

    uint32_t overflowCount() const { return queue_.overflowCount(); }

    CaptureState finish() {
        if (!active_) {
            return result_;
        }
        active_ = false;
        result_ = finishActive();
        return result_;
    }

private:
    CaptureState finishActive() {
        WriteStatus status;
        do {
            status = queue_.drainOne();
        } while (status == WriteStatus::complete || status == WriteStatus::partial);

        const bool synced = status == WriteStatus::idle && file_.sync();
        const bool closed = file_.close();
        if (!synced || !closed) {
            return CaptureState::error;
        }
        if (queue_.overflowCount() != 0) {
            return CaptureState::incomplete;
        }

        const detail::Seal proof{queue_.bytesWritten(), file_.crc()};
        return detail::checkClosedPcap<File>(path_.c_str(), proof) &&
                       detail::publishSeal<File>(path_, proof)
                   ? CaptureState::saved
                   : CaptureState::error;
    }

    const std::string path_;
    detail::PcapOutput<File> file_;
    WriteQueue<Capacity, MaxBlockSize, detail::PcapOutput<File>> queue_;
    bool attempted_ = false;
    bool active_ = false;
    CaptureState result_ = CaptureState::incomplete;
};

} // namespace storage
} // namespace airrounds
