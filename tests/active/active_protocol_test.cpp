// This test compiles the active-build protocol path by defining AIRROUNDS_ACTIVE before the include.
// The default-build path (a mode 02 tail rejected, the modes read listing only 02 00 01) is covered
// by tests/uart/protocol_test.cpp, which the gate compiles without the define.
#define AIRROUNDS_ACTIVE 1

// Keep every check active in builds that define NDEBUG.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "uart_protocol.h"

using namespace airrounds::uart;
using airrounds::session::Target;

namespace {

constexpr uint64_t boot = 0x123456789abcdef0ULL;

class Visit final : public VisitBackend {
public:
    uint64_t nowUs() const override { return 0; }
    VisitStart start(const Target&, const VisitBudget&) override { return VisitStart::listening; }
    bool observationDone() const override { return false; }
    bool stopReception() override { return true; }
    void beginSave(const CaptureId&) override {}
    Outcome saveResult() const override { return Outcome::none; }
};

class TestScan final : public ScanBackend {
public:
    void startScan() override { state = ScanState::running; }
    void pollScan() override {}
    void stopScan() override { state = ScanState::idle; }
    ScanState scanState() const override { return state; }
    const ScanList& scanList() const override { return list; }

    ScanState state = ScanState::idle;
    ScanList list;
};

std::string command(Protocol& protocol, const char* tail) {
    const std::string input = std::string("@01 123456789abcdef0 ") + tail + "\n";
    std::string reply;
    for (char byte : input) {
        char output[Protocol::replyCapacity];
        if (protocol.receive({static_cast<uint8_t>(byte), 0}, output)) {
            reply += output;
        }
    }
    return reply;
}

void contains(const std::string& actual, const std::string& part) {
    if (actual.find(part) == std::string::npos) {
        std::cerr << "expected \"" << part << "\" in \"" << actual << "\"\n";
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    Visit visit;
    TestScan scan;
    Protocol protocol(visit, boot, &scan);

    // The active build lists the active mode (02) in the modes read, alongside Beacon and EAPOL.
    contains(command(protocol, "8000000000000001 00000000 modes"), " modes 03 00 01 02\n");

    // A round may select the active mode, and round_status_mode reports it.
    contains(command(protocol, "0000000000000001 00000001 round 020000000001 06 mode 02"), " ok ");
    contains(command(protocol, "8000000000000002 00000001 round_status_mode"), " mode 02 aps ");

    std::cout << "active protocol tests passed\n";
    return 0;
}
