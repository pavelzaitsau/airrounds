#include "pcap_capture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/wait.h>

using namespace airrounds::storage;

namespace {

// Classic PCAP with a single synthetic Ethernet frame.
const std::vector<uint8_t> sample{
    0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 1, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 14, 0, 0, 0, 14, 0, 0, 0,
    2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 2, 0x88, 0xb5,
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Capture>
void enqueue(Capture& capture, const std::vector<uint8_t>& bytes) {
    require(capture.enqueue(bytes.data(), bytes.size()) == EnqueueStatus::queued,
            "queue synthetic PCAP bytes");
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void testFinishAndNextFile(const std::string& directory) {
    const auto first = directory + "/first.pcap";
    const auto second = directory + "/second.pcap";
    {
        PcapCapture<2, 64> capture(first.c_str());
        require(capture.open(), "open first capture");
        require(capture.enqueue(sample.data(), 8) == EnqueueStatus::queued, "queue prefix");
        require(capture.drainOne() == WriteStatus::complete, "write prefix");
        require(capture.enqueue(sample.data() + 8, sample.size() - 8) == EnqueueStatus::queued,
                "queue remaining bytes");
        require(recoverPcap(first.c_str()) == CaptureState::incomplete,
                "do not report saved before finalization");
        require(capture.finish() == CaptureState::saved, "finish drains and checks the closed file");
        require(capture.finish() == CaptureState::saved, "repeat finish is safe");
        require(capture.enqueue(sample.data(), 1) == EnqueueStatus::invalid,
                "reject writes after finalization");
    }
    {
        PcapCapture<2, 64> capture(first.c_str());
        require(!capture.open(), "refuse to replace a capture");
    }
    {
        PcapCapture<2, 64> capture(second.c_str());
        require(capture.open(), "open an independent capture");
        enqueue(capture, sample);
        require(capture.finish() == CaptureState::saved, "finish the next capture");
    }
    require(readFile(first) == sample && readFile(second) == sample, "keep both files intact");
    require(recoverPcap(first.c_str()) == CaptureState::saved &&
                recoverPcap(second.c_str()) == CaptureState::saved,
            "restart verifies both files independently");
}

void testInvalidPcapAndOverflow(const std::string& directory) {
    auto malformed = sample;
    malformed[32] = 30;  // The record claims a payload longer than its original frame.
    const std::vector<std::vector<uint8_t>> badInputs{
        {sample.begin(), sample.begin() + 12},
        {sample.begin(), sample.begin() + 24},
        {sample.begin(), sample.begin() + 33},
        {sample.begin(), sample.end() - 1},
        malformed,
    };
    for (size_t index = 0; index < badInputs.size(); ++index) {
        const auto path = directory + "/bad" + std::to_string(index) + ".pcap";
        PcapCapture<1, 64> capture(path.c_str());
        require(capture.open(), "create malformed capture");
        enqueue(capture, badInputs[index]);
        require(capture.finish() == CaptureState::error, "reject malformed or empty PCAP");
        require(recoverPcap(path.c_str()) == CaptureState::incomplete, "leave failed output unsealed");
    }

    const auto overflow = directory + "/overflow.pcap";
    PcapCapture<1, 64> capture(overflow.c_str());
    require(capture.open(), "create overflow case");
    enqueue(capture, sample);
    require(capture.enqueue(sample.data(), 1) == EnqueueStatus::full, "drop one block");
    require(capture.finish() == CaptureState::incomplete, "overflow prevents publishing a capture");
    require(recoverPcap(overflow.c_str()) == CaptureState::incomplete, "leave overflow output unsealed");
}

void testInterruptedProcess(const std::string& directory) {
    const auto path = directory + "/interrupted.pcap";
    require(recoverPcap(path.c_str()) == CaptureState::missing, "unknown capture is missing");
    const pid_t child = fork();
    require(child >= 0, "fork for restart test");
    if (child == 0) {
        PcapCapture<1, 64> capture(path.c_str());
        require(capture.open(), "create interrupted capture");
        enqueue(capture, sample);
        require(capture.drainOne() == WriteStatus::complete, "write before restart");
        _exit(0);  // Skip finalization and object cleanup.
    }

    int childStatus = 0;
    require(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
                WEXITSTATUS(childStatus) == 0,
            "capture process exited before finalization");
    require(readFile(path) == sample, "interruption left a structurally valid PCAP");
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "a valid unsealed PCAP stays incomplete after restart");
}

void testDamageAfterSave(const std::string& directory) {
    const auto path = directory + "/damaged.pcap";
    PcapCapture<2, 64> capture(path.c_str());
    require(capture.open(), "create damage case");
    enqueue(capture, sample);
    const std::vector<uint8_t> record(sample.begin() + 24, sample.end());
    enqueue(capture, record);
    require(capture.finish() == CaptureState::saved, "finish before damage");
    auto corrupt = readFile(path);
    corrupt.back() ^= 1;
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(corrupt.data()), corrupt.size());
    }
    require(recoverPcap(path.c_str()) == CaptureState::error,
            "restart detects changed payload bytes even when the length matches");
    require(truncate(path.c_str(), sample.size()) == 0, "drop one complete record");
    require(recoverPcap(path.c_str()) == CaptureState::error,
            "restart detects truncation exactly at a record boundary");
}

struct FaultFile : SdFile {
    enum Fault { none, zero_write, sync_data, close_data, reread, corrupt_read, sync_seal, close_seal,
        seal_reread, publish_seal };
    static Fault fault;
    bool writingSeal = false;
    bool writingData = false;
    bool readingSeal = false;

    bool openNew(const char* path) {
        writingSeal = std::string(path).find(".ok.part") != std::string::npos;
        writingData = !writingSeal;
        return SdFile::openNew(path);
    }

    bool openRead(const char* path) {
        readingSeal = std::string(path).find(".ok.part") != std::string::npos;
        return SdFile::openRead(path);
    }

    size_t write(const uint8_t* bytes, size_t count) {
        return fault == zero_write && writingData ? 0 : SdFile::write(bytes, count);
    }

    ssize_t read(uint8_t* bytes, size_t count) {
        if (fault == reread || (fault == seal_reread && readingSeal)) {
            return -1;
        }
        const auto got = SdFile::read(bytes, count);
        if (fault == corrupt_read && got == 14) {
            bytes[got - 1] ^= 1;
        }
        return got;
    }

    bool sync() {
        if (fault == (writingSeal ? sync_seal : sync_data)) {
            return false;
        }
        return SdFile::sync();
    }

    bool close() {
        const bool closed = SdFile::close();
        return closed && !(writingSeal && fault == close_seal) &&
               !(writingData && fault == close_data);
    }

    static bool publish(const char* from, const char* to) {
        return fault != publish_seal && SdFile::publish(from, to);
    }
};

FaultFile::Fault FaultFile::fault = FaultFile::none;

void testFinalizationErrors(const std::string& directory) {
    for (const auto fault : {FaultFile::zero_write, FaultFile::sync_data, FaultFile::close_data,
                            FaultFile::reread, FaultFile::corrupt_read, FaultFile::sync_seal,
                            FaultFile::close_seal, FaultFile::seal_reread, FaultFile::publish_seal}) {
        const auto path = directory + "/fault" + std::to_string(fault) + ".pcap";
        PcapCapture<1, 64, FaultFile> capture(path.c_str());
        require(capture.open(), "create fault case");
        enqueue(capture, sample);
        FaultFile::fault = fault;
        require(capture.finish() == CaptureState::error, "surface a finalization error");
        FaultFile::fault = FaultFile::none;
        require(recoverPcap(path.c_str()) == CaptureState::incomplete,
                "a failed finalization must not appear saved on restart");
    }
}

void testClassicFormats(const std::string& directory) {
    constexpr std::array<std::array<uint8_t, 4>, 4> magic{{
        {{0xd4, 0xc3, 0xb2, 0xa1}},
        {{0xa1, 0xb2, 0xc3, 0xd4}},
        {{0x4d, 0x3c, 0xb2, 0xa1}},
        {{0xa1, 0xb2, 0x3c, 0x4d}},
    }};
    for (size_t index = 0; index < magic.size(); ++index) {
        auto bytes = sample;
        std::copy(magic[index].begin(), magic[index].end(), bytes.begin());
        if (index % 2 != 0) {
            std::reverse(bytes.begin() + 4, bytes.begin() + 6);
            std::reverse(bytes.begin() + 6, bytes.begin() + 8);
            for (size_t offset = 8; offset < 40; offset += 4) {
                std::reverse(bytes.begin() + offset, bytes.begin() + offset + 4);
            }
        }

        const auto path = directory + "/format" + std::to_string(index) + ".pcap";
        PcapCapture<1, 64> capture(path.c_str());
        require(capture.open(), "create a classic PCAP variant");
        enqueue(capture, bytes);
        require(capture.finish() == CaptureState::saved &&
                    recoverPcap(path.c_str()) == CaptureState::saved,
                "accept both byte orders and timestamp resolutions");
    }
}

void testSealRecovery(const std::string& directory) {
    const auto path = directory + "/seal.pcap";
    PcapCapture<1, 64> capture(path.c_str());
    require(capture.open(), "create seal case");
    enqueue(capture, sample);
    require(capture.finish() == CaptureState::saved, "save the test capture");

    const auto seal = path + ".ok";
    const auto stage = seal + ".part";
    require(rename(seal.c_str(), stage.c_str()) == 0, "stage a valid seal");
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "even a valid staged seal cannot mark a capture saved");
    require(rename(stage.c_str(), seal.c_str()) == 0, "restore the published seal");

    const auto good = readFile(seal);
    {
        std::ofstream output(stage, std::ios::binary);
        output.write(reinterpret_cast<const char*>(good.data()), good.size());
    }
    require(recoverPcap(path.c_str()) == CaptureState::incomplete,
            "two seal names after an interrupted rename remain incomplete");
    require(unlink(stage.c_str()) == 0, "remove the staged seal");
    {
        std::ofstream output(seal, std::ios::binary | std::ios::app);
        output.put('X');
    }
    require(recoverPcap(path.c_str()) == CaptureState::error, "reject a malformed seal");
    require(unlink(path.c_str()) == 0, "remove the data file");
    require(recoverPcap(path.c_str()) == CaptureState::error, "a seal alone is an error");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "pass an empty temporary directory");
    testFinishAndNextFile(argv[1]);
    testInvalidPcapAndOverflow(argv[1]);
    testInterruptedProcess(argv[1]);
    testDamageAfterSave(argv[1]);
    testFinalizationErrors(argv[1]);
    testClassicFormats(argv[1]);
    testSealRecovery(argv[1]);
    std::cout << "PCAP capture: 7 groups passed\n";
}
