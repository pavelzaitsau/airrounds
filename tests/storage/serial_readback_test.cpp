#include "../toolchain/esp32/airrounds_passive_smoke/serial_readback.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class SerialOutput {
public:
    size_t printf(const char* format, const char* label, const char* encoded) {
        char line[64];
        const int count = snprintf(line, sizeof(line), format, label, encoded);
        require(count > 0 && static_cast<size_t>(count) < sizeof(line), "format one USB line");
        const auto length = static_cast<size_t>(count);
        const size_t accepted = byteLimit < length ? byteLimit : length;
        text.append(line, accepted);
        byteLimit -= accepted;
        return accepted;
    }

    size_t byteLimit = 4096;
    std::string text;
};

void testReadback(const std::string& directory) {
    const auto path = directory + "/usb-readback.pcap";
    const std::string bytes = "0123456789abcdefghijklmnopqr";
    std::ofstream fixture(path, std::ios::binary);
    fixture << bytes;
    require(static_cast<bool>(fixture), "write the transfer fixture");
    fixture.close();
    require(static_cast<bool>(fixture), "close the transfer fixture");

    SerialOutput output;
    require(airrounds::bench::printHexFile(output, path.c_str(), bytes.size(), "PCAP"),
            "send the expected bytes");
    require(output.text == "PCAP 303132333435363738396162636465666768696a6b6c6d6e\n"
                           "PCAP 6f707172\n",
            "encode the saved bytes in USB lines");

    SerialOutput premature;
    require(!airrounds::bench::printHexFile(premature, path.c_str(), bytes.size() + 1, "PCAP"),
            "early EOF must fail the transfer");
    SerialOutput oversized;
    require(!airrounds::bench::printHexFile(oversized, path.c_str(), bytes.size() - 1, "PCAP"),
            "extra file bytes must fail the transfer");

    SerialOutput partial;
    partial.byteLimit = 57;
    require(!airrounds::bench::printHexFile(partial, path.c_str(), bytes.size(), "PCAP"),
            "short USB write must fail the transfer");
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "pass a test directory");
    testReadback(argv[1]);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
