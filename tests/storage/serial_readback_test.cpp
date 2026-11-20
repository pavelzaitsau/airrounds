#include "../toolchain/esp32/airrounds_passive_smoke/serial_readback.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <csignal>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// The group that is running, named by the watchdog if the group does not finish in time.
char runningGroup[64] = "argument check";
size_t groupsPassed = 0;

} // namespace

// Plain C and async-signal-safe calls only.
extern "C" void reportStuckGroup(int /*signal*/) {
    static const char prefix[] = "watchdog: this test group did not finish in time: ";
    size_t length = 0;
    while (runningGroup[length] != '\0') {
        ++length;
    }
    write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write(STDERR_FILENO, runningGroup, length);
    write(STDERR_FILENO, "\n", 1);
    _exit(EXIT_FAILURE);
}

namespace {

// Runs one test group; the watchdog fails it, by name, after `seconds`.
template <typename Group> void runGroup(const char* name, Group group, unsigned seconds = 30) {
    snprintf(runningGroup, sizeof(runningGroup), "%s", name);
    alarm(seconds);
    group();
    alarm(0);
    ++groupsPassed;
}

// Each run needs new file names, so the binary refuses a missing, unusable, or used directory.
void requireEmptyDirectory(int argc, char** argv) {
    DIR* directory = argc == 2 ? opendir(argv[1]) : nullptr;
    bool empty = directory != nullptr;
    for (const dirent* entry = empty ? readdir(directory) : nullptr; entry != nullptr && empty;
         entry = readdir(directory)) {
        empty = strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
    }
    if (directory != nullptr) {
        closedir(directory);
    }
    require(empty, "pass an empty temporary directory");
}

bool failNextClose = false;

} // namespace

// printHexFile closes its SdFile with close(). Defining close() here replaces the C library
// function for this binary, so a test can make one successful close report an I/O error.
extern "C" int close(int descriptor) {
    using Close = int (*)(int);
    static const auto next = reinterpret_cast<Close>(dlsym(RTLD_NEXT, "close"));
    if (next == nullptr) {
        std::cerr << "find the C library close()\n";
        std::abort();
    }
    const int result = next(descriptor);
    if (result != 0 || !failNextClose) {
        return result;
    }
    failNextClose = false;
    errno = EIO;
    return -1;
}

namespace {

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
        failNextClose = failClose;
        return accepted + overcount;
    }

    size_t byteLimit = 4096;
    size_t overcount = 0;   // Extra bytes a faulty output claims to have sent.
    bool failClose = false; // Make the file close after this line fail.
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

    SerialOutput inflated;
    inflated.overcount = 1;
    require(!airrounds::bench::printHexFile(inflated, path.c_str(), bytes.size(), "PCAP"),
            "a write count larger than the line must fail the transfer");

    SerialOutput unclosed;
    unclosed.failClose = true;
    require(!airrounds::bench::printHexFile(unclosed, path.c_str(), bytes.size(), "PCAP") &&
                !failNextClose,
            "a failed close must fail the transfer");
}

void testNothingToSend(const std::string& directory) {
    const auto empty = directory + "/empty.pcap";
    std::ofstream(empty).close();
    SerialOutput output;
    require(!airrounds::bench::printHexFile(output, empty.c_str(), 0, "PCAP") &&
                output.text.empty(),
            "an empty transfer is a failure, not a success");
    require(
        !airrounds::bench::printHexFile(output, (directory + "/absent.pcap").c_str(), 4, "PCAP") &&
            output.text.empty(),
        "a file that cannot be opened fails the transfer");
}

} // namespace

int main(int argc, char** argv) try {
    signal(SIGALRM, reportStuckGroup);
    requireEmptyDirectory(argc, argv);
    runGroup("testReadback", [&] { testReadback(argv[1]); });
    runGroup("testNothingToSend", [&] { testNothingToSend(argv[1]); });
    std::cout << "serial readback: " << groupsPassed << " groups passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
