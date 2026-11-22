// The checks below must run in every build, including one with NDEBUG.
#undef NDEBUG
#include <cassert>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../../firmware/esp32/uart/capture_path.h"
#include "../../firmware/esp32/uart/uart_protocol.h"
#include "../../flipper/profile_storage.h"
#include "emulator_sdk.h"

extern "C" int32_t airrounds_app(void*);

namespace emu {
uint32_t clockMs = 0;
Storage disk;
Gui gui;
Expansion expansion;
Canvas canvas;
ViewDispatcher* dispatcher = nullptr;
ByteInput* keyboard = nullptr;
FuriHalSerialHandle serial;
std::function<void()> scenario;
std::function<void(const uint8_t*, size_t)> transmit;
bool serialBusy = false;
int openRecords = 0;
uint8_t random = 0x12;
// The tick count when the scenario started. The board's clock counts from here.
uint32_t clockStart = 0;
} // namespace emu

namespace {
using namespace airrounds::uart;

class Receiver final : public VisitBackend {
public:
    // The board keeps its own clock. Flipper's 32-bit tick count may wrap during a scenario.
    uint64_t nowUs() const override {
        return static_cast<uint64_t>(emu::clockMs - emu::clockStart) * 1000U;
    }
    VisitStart start(const airrounds::session::Target&, const VisitBudget&) override {
        ++visits;
        if (visits == deadlineOnVisit) {
            return VisitStart::deadline;
        }
        observed = false;
        outcome = Outcome::none;
        return VisitStart::listening;
    }
    bool observationDone() const override { return observed; }
    bool stopReception() override { return true; }
    void beginSave(const CaptureId& id) override { saves.push_back(id); }
    Outcome saveResult() const override { return outcome; }
    bool supportsJournal() const override { return journalSupported; }
    JournalState journalState() const override { return journal; }

    std::vector<CaptureId> saves;
    bool journalSupported = false;
    JournalState journal = JournalState::none;
    unsigned visits = 0;
    unsigned deadlineOnVisit = 0;
    bool observed = false;
    Outcome outcome = Outcome::none;
};

class Scanner final : public ScanBackend {
public:
    Scanner() { names({"TELEPORT", "Studio", "Guest", "Loft", "Atlas", "Workshop"}); }

    // Replace the networks that the next scan finds. Row N has the address 02:00:00:00:00:0N
    // unless the caller gives the last address bytes.
    void names(const std::vector<std::string>& found, const std::vector<uint8_t>& suffixes = {}) {
        list_.clear();
        for (size_t index = 0; index < found.size(); ++index) {
            const auto suffix =
                suffixes.empty() ? static_cast<uint8_t>(index + 1) : suffixes[index];
            const airrounds::session::Target target{{2, 0, 0, 0, 0, suffix}, 11};
            const bool added = list_.add(
                target, reinterpret_cast<const uint8_t*>(found[index].data()), found[index].size());
            assert(added);
        }
    }

    void startScan() override {
        ++scans;
        state_ = ScanState::running;
    }
    void pollScan() override {
        if (state_ == ScanState::running && fails) {
            state_ = ScanState::error;
        } else if (state_ == ScanState::running && completes) {
            state_ = ScanState::complete;
        }
    }
    void stopScan() override { state_ = cleanupFails ? ScanState::error : ScanState::idle; }
    ScanState scanState() const override { return state_; }
    const ScanList& scanList() const override { return list_; }

    bool completes = true;
    bool fails = false;
    bool cleanupFails = false;
    unsigned scans = 0;

private:
    ScanList list_;
    ScanState state_ = ScanState::idle;
};

void pressTimers();

class Board {
public:
    Board(bool legacy = false, bool scan = false)
        : protocol_(new Protocol(receiver, 0x123456789abcdef0ULL, scan ? &scanner : nullptr)),
          scan_(scan), legacy_(legacy) {
        emu::clockMs = emu::clockStart = 1000;
        emu::transmit = [this](const uint8_t* data, size_t size) {
            sent.append(reinterpret_cast<const char*>(data), size);
            if (online) {
                send(data, size);
            }
        };
    }

    void advance(size_t turns = 1) {
        for (size_t i = 0; i < turns; ++i) {
            emu::clockMs += emu::dispatcher->tickPeriod;
            protocol_->poll();
            // With a chunk size, a reply can arrive across several ticks.
            const size_t count = chunk != 0 && chunk < inbound_.size() ? chunk : inbound_.size();
            for (size_t byte = 0; byte < count; ++byte) {
                emu::serialEvent(FuriHalSerialRxEventData, static_cast<uint8_t>(inbound_[byte]));
            }
            inbound_.erase(0, count);
            emu::tick();
            redraw();
            pressTimers();
        }
    }

    // Timings of SessionClient: a status read every 350 ms, a reply deadline of 900 ms, and a
    // new hello 300 ms after a loss. These waits cover each step with room for one more poll.
    static constexpr size_t ticksToLoseLink = (350 + 900 + 50) / 50 + 4;
    static constexpr size_t ticksToReconnect = (900 + 300 + 350 + 100) / 50 + 7;

    // The cable drops until Flipper reports the link as lost.
    void goOffline() {
        disconnect();
        advance(ticksToLoseLink);
    }

    // The cable returns until Flipper has read the board state again.
    void comeBack() {
        online = true;
        advance(ticksToReconnect);
    }

    // The board reboots: a new boot ID, an idle session and no reply in flight. It can come back
    // with other firmware.
    void restart(uint64_t boot) { restart(boot, scan_); }
    void restart(uint64_t boot, bool scan) {
        scan_ = scan;
        boot_ = boot;
        protocol_.reset(new Protocol(receiver, boot, scan ? &scanner : nullptr));
        inbound_.clear();
    }

    void disconnect() {
        online = false;
        inbound_.clear();
    }

    // A reply is on its way to Flipper.
    bool replying() const { return !inbound_.empty(); }

    // The reply reaches the UART now, while the app is busy and runs no tick.
    void deliver() {
        for (const char byte : inbound_) {
            emu::serialEvent(FuriHalSerialRxEventData, static_cast<uint8_t>(byte));
        }
        inbound_.clear();
    }

    // Another client sends a command to the board. Flipper does not see the reply.
    std::string command(const std::string& body) {
        char boot[17];
        snprintf(boot, sizeof(boot), "%016llx", static_cast<unsigned long long>(boot_));
        const std::string line = std::string("@01 ") + boot + " " + body + "\n";
        // Keep any reply that is still on its way to Flipper.
        const auto waiting = inbound_;
        inbound_.clear();
        send(reinterpret_cast<const uint8_t*>(line.data()), line.size());
        const auto reply = inbound_;
        inbound_ = waiting;
        return reply;
    }

    // Another client starts a round.
    void otherClient(const std::string& body) {
        assert(command(body).find(" ok ") != std::string::npos);
    }

    void oldResult() {
        const std::string command =
            "@01 123456789abcdef0 0000000000000001 00000001 start 020000000009 0b\n";
        send(reinterpret_cast<const uint8_t*>(command.data()), command.size());
        receiver.observed = true;
        receiver.outcome = Outcome::not_observed;
        protocol_->poll();
        inbound_.clear();
    }

    // The GUI draws the current view only after the SDK requested an update.
    static void redraw() {
        auto* dispatcher = emu::dispatcher;
        if (dispatcher->stopped || dispatcher->gui == nullptr || !dispatcher->redrawRequested) {
            return;
        }
        dispatcher->redrawRequested = false;
        auto* view = dispatcher->current;
        if (view->draw == nullptr) {
            emu::canvas.text = {emu::keyboard->heading};
            return;
        }
        canvas_reset(&emu::canvas);
        view->draw(&emu::canvas, view_get_model(view));
        view_commit_model(view, false);
    }

    Receiver receiver;
    Scanner scanner;
    bool online = true;
    size_t chunk = 0;
    // Every byte that Flipper sent, including bytes lost while offline.
    std::string sent;
    // Change each reply line on its way to Flipper.
    std::function<void(std::string&)> rewrite;

private:
    void send(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            char reply[Protocol::replyCapacity];
            if (protocol_->receive({data[i], emu::clockMs}, reply)) {
                std::string wire = reply;
                if (legacy_ && wire.size() >= 4) {
                    wire.replace(wire.size() - 3, 2, "01");
                }
                if (rewrite) {
                    rewrite(wire);
                }
                inbound_ += wire;
            }
        }
    }

    std::unique_ptr<Protocol> protocol_;
    uint64_t boot_ = 0x123456789abcdef0ULL;
    std::string inbound_;
    bool scan_;
    bool legacy_;
};

bool shows(const char* label) {
    return std::find(emu::canvas.text.begin(), emu::canvas.text.end(), label) !=
           emu::canvas.text.end();
}

void expect(const char* label) {
    if (!shows(label)) {
        std::cerr << "Expected " << label << ", saw";
        for (const auto& text : emu::canvas.text) {
            std::cerr << " [" << text << ']';
        }
        std::cerr << '\n';
        std::abort();
    }
}

// The screen shows exactly these strings, in drawing order.
void screenAt(int line, std::initializer_list<const char*> labels) {
    const std::vector<std::string> expected(labels.begin(), labels.end());
    if (emu::canvas.text != expected) {
        std::cerr << "Line " << line << ": expected screen";
        for (const auto& text : expected) {
            std::cerr << " [" << text << ']';
        }
        std::cerr << "\nSaw screen";
        for (const auto& text : emu::canvas.text) {
            std::cerr << " [" << text << ']';
        }
        std::cerr << '\n';
        std::abort();
    }
}
#define screen(...) screenAt(__LINE__, {__VA_ARGS__})

// A tick with nothing new for the app must not ask the GUI to draw again.
void idleTick() {
    Board::redraw();
    assert(!emu::dispatcher->redrawRequested);
    emu::clockMs += 50;
    emu::tick();
    assert(!emu::dispatcher->redrawRequested);
}

// The screen has exactly this layout: one drawing call per line. Each line gives the font
// (P primary, S secondary), the color (b black, w white on a highlight), the position, and the
// text.
void layoutAt(int line, const std::string& expected) {
    std::string drawn;
    for (const auto& op : emu::canvas.ops) {
        drawn += op + "\n";
    }
    if (drawn != expected) {
        std::cerr << "Line " << line << ": expected layout\n"
                  << expected << "Saw layout\n"
                  << drawn;
        std::abort();
    }
    // A stable screen stays drawn as it is.
    idleTick();
}
#define layout(expected) layoutAt(__LINE__, expected)

void key(InputKey button, InputType type) {
    emu::input(button, type);
    emu::customEvents();
    Board::redraw();
}

// The input service in 1.5.1 (input.c): a key released within 300 ms gives Short, then Release.
// A longer hold gives Long at 300 ms and Repeat every 150 ms after it, and no Short.
struct HeldKey {
    bool down = false;
    uint32_t since = 0;
    unsigned counts = 0;
};
HeldKey heldKeys[InputKeyBack + 1];

void pressTimers() {
    for (size_t button = 0; button <= InputKeyBack; ++button) {
        auto& held = heldKeys[button];
        while (held.down && !emu::dispatcher->stopped &&
               emu::clockMs - held.since >= 150U * (held.counts + 1)) {
            ++held.counts;
            if (held.counts >= 2) {
                key(static_cast<InputKey>(button),
                    held.counts == 2 ? InputTypeLong : InputTypeRepeat);
            }
        }
    }
}

void press(InputKey button) {
    key(button, InputTypePress);
    heldKeys[button] = {true, emu::clockMs, 0};
}

void release(InputKey button) {
    pressTimers();
    const bool brief = heldKeys[button].counts < 2;
    heldKeys[button] = {};
    if (brief) {
        key(button, InputTypeShort);
    }
    // The dispatcher stops after Back on the main menu. The release then reaches no view.
    if (!emu::dispatcher->stopped) {
        key(button, InputTypeRelease);
    }
}

void tap(InputKey button) {
    press(button);
    release(button);
}

bool editingAddress() {
    return emu::dispatcher->current == &emu::keyboard->view;
}

// Back on the main menu closes the app.
void exitApp() {
    expect("AirRounds");
    tap(InputKeyBack);
    assert(emu::dispatcher->stopped);
}

airrounds::session::Profile profileOf(std::initializer_list<uint8_t> suffixes) {
    airrounds::session::Profile profile;
    for (const uint8_t suffix : suffixes) {
        const bool added = profile.add({{2, 0, 0, 0, 0, suffix}, 6});
        assert(added);
    }
    return profile;
}

// Give each scenario its own card. A test must not depend on what an earlier one saved.
void cardWith(std::initializer_list<uint8_t> suffixes) {
    emu::disk = Storage{};
    if (suffixes.size() != 0) {
        airrounds::flipper::ProfileStorage store(&emu::disk);
        assert(store.save({}, profileOf(suffixes)) == airrounds::flipper::ProfileSave::saved);
    }
}

// Run the app until the scenario ends and Back leaves it, then check what it leaves behind.
// The Board constructor starts the clock. A scenario can start it elsewhere instead.
void runApp(uint8_t seed = 0x12, uint32_t clockStart = 0) {
    // Each scenario starts from the same state, whatever ran before it.
    emu::random = seed;
    if (clockStart != 0) {
        emu::clockMs = emu::clockStart = clockStart;
    }
    emu::canvas = Canvas{};
    for (auto& held : heldKeys) {
        held = {};
    }
    bool finished = false;
    const auto body = emu::scenario;
    emu::scenario = [&] {
        // The UART runs at 115200 baud with error reports. Board::advance models a 50 ms tick.
        assert(emu::dispatcher->tickPeriod == 50);
        assert(!emu::serial.acquired || (emu::serial.baud == 115200 && emu::serial.reportErrors));
        body();
        finished = true;
    };
    const int records = emu::openRecords;
    assert(airrounds_app(nullptr) == 0);
    assert(finished);
    assert(emu::openRecords == records);
    assert(!emu::serial.acquired && emu::serial.receive == nullptr);
    assert(emu::expansion.enabled);
}

// Enter an address on the keyboard and confirm it.
void typeAddress(const uint8_t (&address)[6]) {
    auto* keyboard = emu::keyboard;
    assert(emu::dispatcher->views[emu::dispatcher->active] == &keyboard->view);
    assert(keyboard->size == sizeof(address));
    memcpy(keyboard->buffer, address, sizeof(address));
    keyboard->view.locked = true;
    keyboard->saved(keyboard->context);
    keyboard->view.locked = false;
    emu::customEvents();
    Board::redraw();
}

void bssid(uint8_t suffix) {
    const uint8_t address[]{static_cast<uint8_t>(suffix == 0 ? 0 : 2), 0, 0, 0, 0, suffix};
    typeAddress(address);
}

void addPoint(uint8_t suffix) {
    tap(InputKeyOk);
    expect("Enter BSSID");
    bssid(suffix);
    expect("Select channel");
    tap(InputKeyRight);
    tap(InputKeyOk);
    expect("Access points");
}

void nativeFlow(Board& board) {
    Board::redraw();
    screen("AirRounds", "Set up points", "Access points", "Last result");
    tap(InputKeyOk);
    tap(InputKeyOk);
    bssid(0);
    screen("Invalid BSSID");
    bssid(1);
    screen("Select channel", "02:00:00:00:00:01", "< Channel 1 >", "Save");
    tap(InputKeyOk);
    tap(InputKeyDown);
    addPoint(2);
    tap(InputKeyDown);
    addPoint(3);
    tap(InputKeyBack);
    board.advance(8);
    screen("AirRounds", "Start session", "Access points", "Last result");
    tap(InputKeyOk);
    screen("Review session", "1 02:00:00:00:00:01", "Ch 1", "2 02:00:00:00:00:02", "Ch 2",
           "3 02:00:00:00:00:03", "Ch 2", "Start");

    // A held Start must not execute if the link drops before release.
    press(InputKeyOk);
    emu::serialEvent(FuriHalSerialRxEventFrameError);
    board.advance(2);
    release(InputKeyOk);
    screen("Review session", "1 02:00:00:00:00:01", "Ch 1", "2 02:00:00:00:00:02", "Ch 2",
           "3 02:00:00:00:00:03", "Ch 2", "Devboard offline");
    assert(board.receiver.visits == 0);
    board.advance(38);
    screen("Review session", "1 02:00:00:00:00:01", "Ch 1", "2 02:00:00:00:00:02", "Ch 2",
           "3 02:00:00:00:00:03", "Ch 2", "Start");
    tap(InputKeyOk);
    screen("Starting...", "Waiting for board");
    board.advance();
    screen("Observing", "Point 1 / Ch 1", "Stop");

    board.receiver.observed = true;
    board.advance(12);
    screen("Saving point", "Point 1 / Ch 1", "Stop");
    tap(InputKeyOk);
    screen("Stopping...", "Waiting for board");
    board.disconnect(); // The board received Stop. Flipper did not receive its reply.
    board.receiver.outcome = Outcome::saved;
    board.receiver.journal = JournalState::failed;
    board.advance(22);
    screen("Link lost", "Session may run", "Check cable");
    assert(board.receiver.visits == 1);
    board.online = true;
    for (size_t turn = 0; turn < 36 && !shows("Checking status"); ++turn) {
        board.advance();
    }
    screen("Checking status", "Command unconfirmed");
    board.advance(10);
    screen("Session stopped", "1 of 3 captured", "Session log failed", "Results");
    tap(InputKeyOk);
    screen("Results", "Point 1", "Captured", "Point 2", "Stopped", "Point 3", "Stopped");
    tap(InputKeyDown);
    tap(InputKeyOk);
    screen("Point 2", "Stopped before result", "02:00:00:00:00:02", "Ch 2");
    tap(InputKeyRight);
    screen("Point 2", "Stopped before result", "02:00:00:00:00:02", "Ch 2");
    tap(InputKeyBack);
    tap(InputKeyUp);
    tap(InputKeyOk);
    tap(InputKeyRight);
    screen("Board SD file", "visit-", "123456789abcdef0-", "00000001-1.pcap");
}

void scannedFlow() {
    Board board(false, true);
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Flash board hardware");
        tap(InputKeyBack);
        layout("Pb 4,11 AirRounds\n"
               "Sb 5,22 Scan area\n"
               "Sb 5,33 Capture mode\n"
               "box 1,35 124x11\n"
               "Sw 5,44 Flash board hardware\n"
               "Sb 5,55 Test board\n"
               "Sb 123,63 v\n");
        tap(InputKeyUp);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        tap(InputKeyBack); // Request cancel before the start reply arrives.
        board.advance();
        screen("Canceling...", "Listening for networks");
        board.advance(20);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyOk);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan");
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "2/5 selected", "[x] TELEPORT", "[x] Studio", "[ ] Guest", "v", "Scan",
               "Start (2)");
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/2", "Beacon - Listening", "TELEPORT", "Stop");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Capture", "Captured 1/2", "Beacon - Listening", "Studio", "Stop");
        board.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        board.advance(15);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "[ ] Guest", "v",
               "Scan", "Start (1)");
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(12);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "+ Guest", "Saved", "v", "Scan");
        assert(!shows("Ch 11") && !shows("02:00:00:00:00:01"));
        const unsigned scans = board.scanner.scans;
        tap(InputKeyLeft);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        board.advance(60);
        assert(board.scanner.scans == scans + 1);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "+ Guest", "Saved", "v", "Scan");
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "Not seen",
               "+ Guest", "Saved", "v", "Scan", "Start (1)");
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "+ Guest",
               "Saved", "v", "Scan");
        assert(!shows("Not seen"));
        assert(board.receiver.visits == 5);
    };
    runApp();
}

void deadlineKeepsCompletedMiss() {
    Board board(false, true);
    cardWith({});
    board.receiver.deadlineOnVisit = 3;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        for (unsigned row = 0; row < 3; ++row) {
            tap(InputKeyOk);
            if (row < 2) {
                tap(InputKeyDown);
            }
        }
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(15);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "[x] Guest", "v", "Scan", "Start (1)");
    };
    runApp();
}

void scanCancelShowsCleanupError() {
    Board board(false, true);
    cardWith({});
    board.scanner.completes = false;
    board.scanner.cleanupFails = true;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(8);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        tap(InputKeyBack);
        board.advance(15);
        screen("Cancel failed", "Check Devboard", "Scan");
        tap(InputKeyBack);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
    };
    runApp();
}

void fivePointSelection() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        for (unsigned row = 0; row < 5; ++row) {
            tap(InputKeyOk);
            tap(InputKeyDown);
        }
        tap(InputKeyOk);
        screen("Beacons", "Maximum 5", "[x] Loft", "[x] Atlas", "[ ] Workshop", "^", "Scan",
               "Start (5)");
        assert(!shows("5/5 selected"));
        // Moving on clears the note.
        tap(InputKeyUp);
        screen("Beacons", "5/5 selected", "[x] Guest", "[x] Loft", "[x] Atlas", "^", "v", "Scan",
               "Start (5)");
        assert(!shows("Maximum 5"));
        tap(InputKeyDown);
        screen("Beacons", "5/5 selected", "[x] Loft", "[x] Atlas", "[ ] Workshop", "^", "Scan",
               "Start (5)");
        // A selected row can still be cleared and chosen again at the limit.
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Beacons", "4/5 selected", "[x] Guest", "[x] Loft", "[ ] Atlas", "^", "v", "Scan",
               "Start (4)");
        tap(InputKeyOk);
        screen("Beacons", "5/5 selected", "[x] Guest", "[x] Loft", "[x] Atlas", "^", "v", "Scan",
               "Start (5)");
        assert(board.receiver.visits == 0);
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/5", "Beacon - Listening", "TELEPORT", "Stop");
        assert(board.receiver.visits == 1);
    };
    runApp();
}

void saveAfterCardReturns() {
    Storage card;
    airrounds::flipper::ProfileStorage store(&card);
    airrounds::session::Profile first;
    airrounds::session::Profile second;
    assert(first.add({{2, 0, 0, 0, 0, 1}, 6}));
    assert(second.add({{2, 0, 0, 0, 0, 2}, 11}));
    using airrounds::flipper::ProfileSave;
    assert(store.save({}, first) == ProfileSave::saved);
    assert(store.save(first, second) == ProfileSave::saved);
    card.failRead = true;
    assert(store.load().size() == 0);
    assert(store.save(second, first) == ProfileSave::unavailable);
    card.failRead = false;
    assert(store.save(second, first) == ProfileSave::saved && store.load().matches(first));
    card.shortWrite = true;
    assert(store.save(first, second) == ProfileSave::unavailable && store.load().matches(first));
}

void staleProfileIsNotOverwritten() {
    Board board;
    cardWith({1, 2, 3});
    airrounds::flipper::ProfileStorage store(&emu::disk);
    const auto prior = store.load();
    assert(prior.size() == 3);
    emu::disk.failRead = true;
    emu::scenario = [&] {
        Board::redraw();
        screen("AirRounds", "Set up points", "Access points", "Last result");
        tap(InputKeyOk);
        tap(InputKeyOk);
        bssid(7);
        screen("Select channel", "02:00:00:00:00:07", "< Channel 1 >", "Save");
        emu::disk.failRead = false;
        tap(InputKeyOk);
        screen("Profile changed", "02:00:00:00:00:07", "< Channel 1 >", "Reopen AirRounds");
        assert(!shows("Save"));
        tap(InputKeyOk);
        screen("Profile changed", "02:00:00:00:00:07", "< Channel 1 >", "Reopen AirRounds");
        assert(store.load().matches(prior));
    };
    runApp();
}

void rejectedLegacyStartDoesNotNameAnotherPoint() {
    Board board(true);
    cardWith({7});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:07", "Ch 6", "Start");
        board.oldResult();
        tap(InputKeyOk);
        board.advance(10);
        screen("Start rejected", "Session status updated", "Refresh");
        tap(InputKeyOk);
        board.advance(10);
        screen("Session ended", "0 of 1 captured", "Results");
        tap(InputKeyOk);
        tap(InputKeyOk);
        screen("Point 1", "Point not seen", "Address unavailable");
    };
    runApp();
}

void rescanKeepsSelection() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan");
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "2/5 selected", "[x] TELEPORT", "[ ] Studio", "[x] Guest", "v", "Scan",
               "Start (2)");
        const unsigned scans = board.scanner.scans;
        tap(InputKeyLeft);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        board.advance(60);
        assert(board.scanner.scans == scans + 1);
        screen("Beacons", "2/5 selected", "[x] TELEPORT", "[ ] Studio", "[x] Guest", "v", "Scan",
               "Start (2)");
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/2", "Beacon - Listening", "TELEPORT", "Stop");
        assert(board.receiver.visits == 1);
    };
    runApp();
}

void storeOnePoint() {
    cardWith({4});
}

void startOnePoint(Board& board) {
    board.advance(8);
    screen("AirRounds", "Start session", "Access points", "Last result");
    tap(InputKeyOk);
    screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
    tap(InputKeyOk);
    screen("Starting...", "Waiting for board");
    board.advance();
    screen("Observing", "Point 1 / Ch 6", "Stop");
}

void restartDuringSessionIsShown() {
    Board board;
    storeOnePoint();
    emu::scenario = [&] {
        startOnePoint(board);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyBack);
        startOnePoint(board);
        assert(board.receiver.visits == 2);

        board.restart(0x0fedcba987654321ULL);
        board.advance(30);
        screen("Devboard restarted", "Check board SD", "Last result");
        board.advance(20); // Later status replies from the new boot keep the notice.
        screen("Devboard restarted", "Check board SD", "Last result");
        tap(InputKeyOk);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyBack);
        expect("Session status"); // Home keeps the notice until the status screen closes.
        tap(InputKeyOk);
        screen("Devboard restarted", "Check board SD", "Last result");
        tap(InputKeyBack);
        screen("AirRounds", "Start session", "Access points", "Last result");
        assert(board.receiver.visits == 2);
    };
    runApp();
}

void receiveErrorDropsLink() {
    Board board;
    storeOnePoint();
    emu::scenario = [&] {
        startOnePoint(board);
        emu::serialEvent(FuriHalSerialRxEventFrameError);
        board.advance();
        screen("Link lost", "Session may run", "Check cable");
        board.advance(20);
        screen("Observing", "Point 1 / Ch 6", "Stop");

        // A byte that arrives with a parity error cannot be trusted either.
        emu::serialEvent(FuriHalSerialRxEventData | FuriHalSerialRxEventParityError, '!');
        board.advance();
        screen("Link lost", "Session may run", "Check cable");
        board.advance(20);
        screen("Observing", "Point 1 / Ch 6", "Stop");
        assert(board.receiver.visits == 1);
    };
    runApp();
}

// A slow card blocks the app for longer than the reply deadline. The board's reply reached the
// UART before the deadline, so the link stays up after the save.
void slowSaveKeepsTheLink() {
    Board board;
    cardWith({});
    emu::disk.clockMs = &emu::clockMs;
    emu::disk.syncMs = 1000;
    emu::scenario = [&] {
        board.advance(8);
        screen("AirRounds", "Set up points", "Access points", "Last result");
        tap(InputKeyOk);
        tap(InputKeyOk);
        bssid(7);
        screen("Select channel", "02:00:00:00:00:07", "< Channel 1 >", "Save");
        for (int tick = 0; tick < 16 && !board.replying(); ++tick) {
            board.advance();
        }
        assert(board.replying());
        board.deliver();
        const auto sent = board.sent.size();
        const auto before = emu::clockMs;
        tap(InputKeyOk);
        assert(emu::clockMs - before >= 1000);
        expect("Access points");
        board.advance(Board::ticksToLoseLink);
        assert(board.sent.find("hello", sent) == std::string::npos);
        assert(board.sent.find("status", sent) != std::string::npos);
        emu::disk.clockMs = nullptr;
        emu::disk.syncMs = 0;
    };
    runApp();
}

using airrounds::flipper::ProfileSave;
using airrounds::flipper::ProfileStorage;
using airrounds::flipper::StoredProfile;
const char* const slotA = "/data/profile-a";
const char* const slotB = "/data/profile-b";

std::vector<unsigned char> record(const airrounds::session::Profile& profile, uint32_t generation) {
    const auto encoded = airrounds::flipper::encodeProfile(profile, generation);
    return {encoded.bytes, encoded.bytes + sizeof(encoded.bytes)};
}

StoredProfile onCard(const Storage& card, const char* path) {
    const auto file = card.files.find(path);
    airrounds::flipper::ProfileRecord stored{};
    if (file == card.files.end() || file->second.size() != sizeof(stored.bytes)) {
        return {};
    }
    memcpy(stored.bytes, file->second.data(), sizeof(stored.bytes));
    return airrounds::flipper::decodeProfile(stored);
}

// Clear every injected fault and keep the files.
void heal(Storage& card) {
    const auto files = card.files;
    card = Storage{};
    card.files = files;
}

void storageAlternatesSlots() {
    Storage card;
    ProfileStorage store(&card);
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    const auto third = profileOf({3});
    assert(store.load().size() == 0);
    assert(store.save({}, first) == ProfileSave::saved);
    assert(card.files.size() == 1);
    assert(onCard(card, slotA).generation == 1 && onCard(card, slotA).points.matches(first));
    assert(store.save(first, second) == ProfileSave::saved);
    assert(onCard(card, slotB).generation == 2 && onCard(card, slotB).points.matches(second));
    assert(onCard(card, slotA).points.matches(first));
    assert(store.load().matches(second));
    assert(store.save(second, third) == ProfileSave::saved);
    assert(onCard(card, slotA).generation == 3 && store.load().matches(third));
    // A save from an old copy must not replace a newer profile.
    assert(store.save(second, first) == ProfileSave::changed);
    assert(store.load().matches(third) && onCard(card, slotB).points.matches(second));

    // The generation wraps. The record after the last value is still newer.
    card.files.clear();
    card.files[slotB] = record(second, UINT32_MAX);
    assert(store.load().matches(second));
    assert(store.save(second, first) == ProfileSave::saved);
    assert(onCard(card, slotA).valid && onCard(card, slotA).generation == 0);
    assert(store.load().matches(first));
    assert(store.save(first, third) == ProfileSave::saved);
    assert(onCard(card, slotB).generation == 1 && store.load().matches(third));
}

void failedWriteKeepsTheLastProfile() {
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    const auto third = profileOf({3});
    for (unsigned failure = 0; failure < 5; ++failure) {
        Storage card;
        ProfileStorage store(&card);
        assert(store.save({}, first) == ProfileSave::saved);
        assert(store.save(first, second) == ProfileSave::saved);
        // The next save goes to the older slot.
        card.shortWrite = failure == 0;
        card.failSync = failure == 1 || failure == 4;
        card.failWriteClose = failure == 2;
        card.failWriteOpen = failure == 3;
        card.failRemoval = failure == 4;
        assert(store.save(second, third) == ProfileSave::unavailable);
        heal(card);
        if (failure == 3) {
            // Nothing was written. The older copy stays.
            assert(onCard(card, slotA).points.matches(first));
        } else if (failure == 4) {
            // The app cannot remove the unconfirmed file. It still reports the save as unconfirmed.
            assert(card.files.count(slotA) == 1);
            continue;
        } else {
            // The app removes a file that it could not confirm.
            assert(card.files.count(slotA) == 0);
        }
        assert(store.load().matches(second));
        assert(store.save(second, third) == ProfileSave::saved && store.load().matches(third));
    }
}

void writeIsReadBack() {
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    const auto third = profileOf({3});
    {
        Storage card;
        ProfileStorage store(&card);
        card.shortRead = true;
        assert(store.save({}, first) == ProfileSave::unavailable);
        assert(card.files.empty());
    }
    for (unsigned damage = 0; damage < 3; ++damage) {
        Storage card;
        ProfileStorage store(&card);
        assert(store.save({}, first) == ProfileSave::saved);
        assert(store.save(first, second) == ProfileSave::saved);
        card.damageWrite = [&](std::vector<unsigned char>& bytes) {
            if (damage == 0) {
                bytes[12] ^= 1U;
            } else if (damage == 1) {
                bytes = record(third, 2); // The card kept an older generation.
            } else {
                bytes = record(first, 3); // Same generation, other points.
            }
        };
        assert(store.save(second, third) == ProfileSave::unavailable);
        card.damageWrite = nullptr;
        assert(card.files.count(slotA) == 0);
        assert(store.load().matches(second));
    }
}

void damagedRecordIsReplaced() {
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    Storage card;
    ProfileStorage store(&card);
    // A file with the wrong size is a damaged record. The card itself still works.
    card.files[slotA] = {'A', 'R', 'P', '1'};
    assert(store.load().size() == 0);
    assert(store.save({}, first) == ProfileSave::saved);
    assert(store.load().matches(first) && onCard(card, slotA).generation == 1);

    card.files[slotA] = record(first, 5);
    card.files[slotA].push_back(0);
    card.files[slotB] = record(second, 4);
    assert(store.load().matches(second));
    assert(store.save(second, first) == ProfileSave::saved);
    assert(onCard(card, slotA).generation == 5 && store.load().matches(first));
}

void unreadableCardBlocksSave() {
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    for (unsigned failure = 0; failure < 5; ++failure) {
        Storage card;
        ProfileStorage store(&card);
        assert(store.save({}, first) == ProfileSave::saved);
        assert(store.save(first, second) == ProfileSave::saved);
        const auto files = card.files;
        card.failClose = failure == 0;
        card.failSize = failure == 1;
        card.unreadablePath = failure == 2 ? slotA : failure == 4 ? slotB : "";
        card.failRead = failure == 3;
        if (failure == 4) {
            assert(store.load().matches(first));
        } else if (failure == 2) {
            // One readable slot still gives the profile. Saving needs both.
            assert(store.load().matches(second));
        } else {
            assert(store.load().size() == 0);
        }
        assert(store.save(second, first) == ProfileSave::unavailable);
        assert(card.files == files);
        heal(card);
        assert(store.load().matches(second));
    }
}

void shortReadIsNotARecord() {
    // Use a record whose last checksum byte is zero. A read that misses it still decodes.
    const auto first = profileOf({1});
    uint32_t generation = 1;
    while (airrounds::flipper::encodeProfile(first, generation).bytes[33] != 0) {
        ++generation;
    }
    Storage card;
    ProfileStorage store(&card);
    card.files[slotA] = record(first, generation);
    assert(store.load().matches(first));
    card.shortRead = true;
    assert(store.load().size() == 0);
}

void oversizedProfileLeavesTheCard() {
    Storage card;
    ProfileStorage store(&card);
    const auto first = profileOf({1});
    const auto second = profileOf({1, 2});
    assert(store.save({}, first) == ProfileSave::saved);
    assert(store.save(first, second) == ProfileSave::saved);
    // The file format holds three points. A larger profile must not touch the older copy.
    const auto files = card.files;
    assert(store.save(second, profileOf({1, 2, 3, 4})) == ProfileSave::unavailable);
    assert(card.files == files);
    assert(store.save(second, profileOf({1, 2, 3})) == ProfileSave::saved);
}

// Edit the saved profile: add, reject, edit, remove, and Back at each step.
void editProfilePoints() {
    Board board;
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        screen("AirRounds", "Set up points", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Access points", "Add point");
        tap(InputKeyOk);
        assert(editingAddress());
        screen("Enter BSSID");
        tap(InputKeyBack);
        screen("Access points", "Add point");
        tap(InputKeyOk);
        bssid(1);
        screen("Select channel", "02:00:00:00:00:01", "< Channel 1 >", "Save");
        tap(InputKeyRight);
        screen("Select channel", "02:00:00:00:00:01", "< Channel 2 >", "Save");
        tap(InputKeyLeft);
        tap(InputKeyLeft);
        screen("Select channel", "02:00:00:00:00:01", "< Channel 13 >", "Save");
        tap(InputKeyRight);
        screen("Select channel", "02:00:00:00:00:01", "< Channel 1 >", "Save");
        tap(InputKeyLeft);
        // Back returns to the address and keeps the draft.
        tap(InputKeyBack);
        assert(editingAddress());
        bssid(1);
        screen("Select channel", "02:00:00:00:00:01", "< Channel 13 >", "Save");
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 13", "Add point");

        tap(InputKeyDown);
        tap(InputKeyOk);
        bssid(1);
        assert(editingAddress());
        screen("Point already added");
        bssid(2);
        screen("Select channel", "02:00:00:00:00:02", "< Channel 1 >", "Save");
        tap(InputKeyOk);
        // The new point keeps the focus.
        tap(InputKeyOk);
        screen("Point 2", "02:00:00:00:00:02", "Ch 1", "Edit", "Remove");
        tap(InputKeyBack);
        screen("Access points", "Point 1", "Ch 13", "Point 2", "Ch 1", "Add point");
        tap(InputKeyOk);
        screen("Point 2", "02:00:00:00:00:02", "Ch 1", "Edit", "Remove");
        tap(InputKeyBack);
        tap(InputKeyDown);
        tap(InputKeyOk);
        assert(editingAddress());
        tap(InputKeyBack);
        screen("Access points", "Point 1", "Ch 13", "Point 2", "Ch 1", "Add point");
        tap(InputKeyOk);
        assert(editingAddress());
        bssid(3);
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 13", "Point 2", "Ch 1", "Point 3", "Ch 1");
        assert(!shows("Add point"));
        // Three rows remain. Down from the third returns to the first.
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Point 1", "02:00:00:00:00:01", "Ch 13", "Edit", "Remove");

        // Edit the first point. Back from address entry returns to the point menu.
        tap(InputKeyOk);
        assert(editingAddress());
        tap(InputKeyBack);
        screen("Point 1", "02:00:00:00:00:01", "Ch 13", "Edit", "Remove");
        tap(InputKeyOk);
        bssid(2);
        screen("Point already added");
        typeAddress({0x02, 0x1a, 0x2b, 0x3c, 0x4d, 0x04});
        screen("Select channel", "02:1A:2B:3C:4D:04", "< Channel 13 >", "Save");
        tap(InputKeyRight);
        screen("Select channel", "02:1A:2B:3C:4D:04", "< Channel 1 >", "Save");
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 1", "Point 2", "Ch 1", "Point 3", "Ch 1");
        tap(InputKeyOk);
        screen("Point 1", "02:1A:2B:3C:4D:04", "Ch 1", "Edit", "Remove");
        tap(InputKeyBack);

        // Remove the second point. Back from the confirmation returns to Remove.
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Remove point?", "02:00:00:00:00:02", "Remove");
        tap(InputKeyBack);
        screen("Point 2", "02:00:00:00:00:02", "Ch 1", "Edit", "Remove");
        tap(InputKeyOk);
        screen("Remove point?", "02:00:00:00:00:02", "Remove");
        emu::disk.failSync = true;
        tap(InputKeyOk);
        screen("Save not confirmed", "02:00:00:00:00:02", "Remove");
        assert(ProfileStorage(&emu::disk).load().size() == 3);
        emu::disk.failSync = false;
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 1", "Point 2", "Ch 1", "Add point");
        assert(!shows("Point 3"));
        assert(ProfileStorage(&emu::disk).load().matches([] {
            airrounds::session::Profile profile;
            const bool added = profile.add({{2, 0x1a, 0x2b, 0x3c, 0x4d, 4}, 1}) &&
                               profile.add({{2, 0, 0, 0, 0, 3}, 1});
            assert(added);
            return profile;
        }()));

        // A failed channel save offers a retry.
        tap(InputKeyUp);
        tap(InputKeyOk);
        tap(InputKeyOk);
        bssid(5);
        emu::disk.shortWrite = true;
        tap(InputKeyOk);
        screen("Save not confirmed", "02:00:00:00:00:05", "< Channel 1 >", "Save");
        emu::disk.shortWrite = false;
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 1", "Point 2", "Ch 1", "Add point");
        tap(InputKeyBack);
        board.advance(2);
        screen("AirRounds", "Start session", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:05", "Ch 1", "2 02:00:00:00:00:03", "Ch 1",
               "Start");
        tap(InputKeyBack);
        exitApp();
    };
    runApp();
}

// A single-point session from saving to an SD error, then a saved file.
void singlePointResults() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        screen("Observing", "Point 1 / Ch 6", "Stop");
        board.receiver.observed = true;
        board.advance(12);
        screen("Reception stopped", "Saving to board SD");
        assert(!shows("Stop"));
        board.receiver.outcome = Outcome::storage_error;
        board.advance(12);
        screen("SD write failed", "0 of 1 captured", "Check board SD", "Results");
        tap(InputKeyOk);
        screen("Results", "Point 1", "SD error");
        tap(InputKeyOk);
        screen("Point 1", "SD write failed", "02:00:00:00:00:04", "Ch 6");
        assert(!shows("File"));
        tap(InputKeyRight);
        screen("Point 1", "SD write failed", "02:00:00:00:00:04", "Ch 6");
        tap(InputKeyBack);
        screen("Results", "Point 1", "SD error");
        tap(InputKeyBack);
        screen("SD write failed", "0 of 1 captured", "Check board SD", "Results");
        tap(InputKeyBack);
        screen("AirRounds", "Start session", "Access points", "Last result");

        tap(InputKeyOk);
        tap(InputKeyOk);
        board.advance();
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyBack);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyOk);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyOk);
        screen("Point 1", "Captured to board SD", "02:00:00:00:00:04", "Ch 6", "File");
        tap(InputKeyRight);
        screen("Board SD file", "visit-", "123456789abcdef0-", "00000002.pcap");
        // One point keeps the single-visit filename, as the board writes it.
        assert(board.receiver.saves.size() == 2 && board.receiver.saves.back().slot == 0);
        char path[64];
        assert(capturePath(path, sizeof(path), "", board.receiver.saves.back()));
        assert(std::string(path) == "/visit-123456789abcdef0-00000002.pcap");
        screen("Board SD file", "visit-", "123456789abcdef0-", "00000002.pcap");
        tap(InputKeyBack);
        screen("Point 1", "Captured to board SD", "02:00:00:00:00:04", "Ch 6", "File");
        tap(InputKeyBack);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyBack);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyBack);
        exitApp();
    };
    runApp();
}

void radioFailureSkipsLaterPoints() {
    Board board;
    cardWith({4, 5});
    board.receiver.deadlineOnVisit = 1;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        tap(InputKeyOk);
        board.advance(10);
        screen("Radio failed", "0 of 2 captured", "Results");
        tap(InputKeyOk);
        screen("Results", "Point 1", "Radio error", "Point 2", "Skipped");
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Point 2", "Skipped after error", "02:00:00:00:00:05", "Ch 6");
        tap(InputKeyBack);
        screen("Results", "Point 1", "Radio error", "Point 2", "Skipped");
        tap(InputKeyOk);
        screen("Point 2", "Skipped after error", "02:00:00:00:00:05", "Ch 6");
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Point 1", "Radio error", "02:00:00:00:00:04", "Ch 6");
    };
    runApp();
}

void noResultYet() {
    Board board;
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("No result available");
        assert(!shows("Results"));
        tap(InputKeyBack);
        exitApp();
    };
    runApp();
}

void scanHomeReopensSession() {
    Board board(false, true);
    cardWith({});
    board.otherClient("0000000000000005 00000001 round 020000000001 0b 020000000002 0b");
    emu::scenario = [&] {
        board.advance(8);
        screen("Capture", "Captured 0/2", "Beacon - Listening", "Point 1", "Stop");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
        assert(!shows("Scan area"));
        tap(InputKeyOk);
        screen("Capture", "Captured 0/2", "Beacon - Listening", "Point 1", "Stop");
        tap(InputKeyBack);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
        tap(InputKeyOk);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "2 of 2 captured", "Results");
        // No scan has run, so Back leaves the result for the main menu.
        tap(InputKeyBack);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyOk);
        screen("Scanning...", "Listening for networks", "Back to cancel");
    };
    runApp();
}

void beaconLabels() {
    Board board(false, true);
    cardWith({});
    board.scanner.names(
        {"Lab", std::string("\0\0", 2), "Lab", " ", "a b~\x7f\x1f", "ABCDEFGHIJKLMNOPQRS", ""});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] 1. Lab", "[ ] 1. Hidden network", "[ ] 2. Lab", "v",
               "Scan");
        assert(!shows("^"));
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] 1. Hidden network", "[ ] 2. Lab",
               "[ ] 2. Hidden network", "^", "v", "Scan");
        assert(!shows("[ ] 1. Lab"));
        screen("Beacons", "0/5 selected", "[ ] 1. Hidden network", "[ ] 2. Lab",
               "[ ] 2. Hidden network", "^", "v", "Scan");
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] 2. Lab", "[ ] 2. Hidden network", "[ ] a b~??", "^",
               "v", "Scan");
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] 2. Hidden network", "[ ] a b~??",
               "[ ] ABCDEFGHIJKLMNOP", "^", "v", "Scan");
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] a b~??", "[ ] ABCDEFGHIJKLMNOP",
               "[ ] 3. Hidden network", "^", "Scan");
        assert(!shows("v"));
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "[ ] a b~??", "[ ] ABCDEFGHIJKLMNOP",
               "[x] 3. Hidden network", "^", "Scan", "Start (1)");
        tap(InputKeyDown);
        screen("Beacons", "1/5 selected", "[ ] 1. Lab", "[ ] 1. Hidden network", "[ ] 2. Lab", "v",
               "Scan", "Start (1)");
        assert(!shows("^"));
        tap(InputKeyUp);
        screen("Beacons", "1/5 selected", "[ ] a b~??", "[ ] ABCDEFGHIJKLMNOP",
               "[x] 3. Hidden network", "^", "Scan", "Start (1)");
        // The progress screen uses the same label.
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/1", "Beacon - Listening", "3. Hidden network", "Stop");
    };
    runApp();
}

void beaconStatusNotes() {
    Board board(false, true);
    cardWith({});
    board.receiver.journalSupported = true;
    board.receiver.journal = JournalState::failed;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "Log failed", "+ TELEPORT", "Saved", "[ ] Studio", "[ ] Guest", "v",
               "Scan");
        assert(!shows("0/5 selected"));
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "Log failed", "+ TELEPORT", "Saved", "[x] Studio", "[ ] Guest", "v",
               "Scan", "Start (1)");
        board.receiver.journal = JournalState::saved;
        tap(InputKeyRight);
        screen("Starting...", "Waiting for board");
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[ ] Guest",
               "v", "Scan");
        assert(!shows("Log failed"));

        board.goOffline();
        screen("Beacons", "Board offline", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[ ] Guest",
               "v", "Scan");
        board.online = true;
        board.advance(30);
        // Only the note in the title changes. The screen must still redraw.
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[ ] Guest",
               "v", "Scan");
        board.goOffline();
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "Board offline", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[x] Guest",
               "v", "Scan");
        board.online = true;
        board.advance(30);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[x] Guest",
               "v", "Scan", "Start (1)");
        // Another client starts a session. Only the Start key goes away.
        board.otherClient("0000000000000009 00000003 round 020000000009 06");
        board.advance(10);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[x] Guest",
               "v", "Scan");
        tap(InputKeyRight);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[x] Guest",
               "v", "Scan");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(15);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "+ Studio", "Saved", "[x] Guest",
               "v", "Scan", "Start (1)");
        assert(board.receiver.visits == 3);
    };
    runApp();
}

void stopKeepsBeaconFocus() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "2/5 selected", "[x] TELEPORT", "[x] Studio", "[ ] Guest", "v", "Scan",
               "Start (2)");
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(12);
        screen("Capture", "Captured 0/2", "Beacon - Listening", "Studio", "Stop");
        board.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        board.advance(15);
        screen("Beacons", "1/5 selected", "[ ] TELEPORT", "Not seen", "[x] Studio", "[ ] Guest",
               "v", "Scan", "Start (1)");
        tap(InputKeyOk);
        screen("Beacons", "0/5 selected", "[ ] TELEPORT", "Not seen", "[ ] Studio", "[ ] Guest",
               "v", "Scan");
    };
    runApp();
}

void failedRescanKeepsTheList() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        board.scanner.fails = true;
        tap(InputKeyLeft);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        board.advance(20);
        screen("Scan failed", "Check Devboard", "Scan");
        tap(InputKeyBack);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        tap(InputKeyLeft);
        board.advance(20);
        screen("Scan failed", "Check Devboard", "Scan");
        // A retry that fails again still returns to the list.
        tap(InputKeyOk);
        board.advance(20);
        screen("Scan failed", "Check Devboard", "Scan");
        tap(InputKeyBack);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        tap(InputKeyLeft);
        board.advance(20);
        board.scanner.fails = false;
        tap(InputKeyOk);
        screen("Scanning...", "Listening for networks", "Back to cancel");
        board.advance(60);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        tap(InputKeyBack);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        exitApp();
    };
    runApp();
}

void reviewHintFollowsTheLink() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        board.goOffline();
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Devboard offline");
        board.online = true;
        for (unsigned turn = 0; turn < 40 && shows("Devboard offline"); ++turn) {
            board.advance();
        }
        // Only the hint changes. The screen must still redraw.
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Checking status");
        for (unsigned turn = 0; turn < 40 && !shows("Start"); ++turn) {
            board.advance();
        }
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        board.otherClient("0000000000000009 00000001 round 020000000009 06");
        board.advance(10);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Session in progress");
    };
    runApp();
}

// OK binds to the action on screen when it goes down. A change of link before the release needs
// a fresh press, even when the choice itself stays on screen. A frame error drops the link at once.
void heldChoiceAfterLinkChange() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyDown);
        press(InputKeyOk);
        emu::serialEvent(FuriHalSerialRxEventFrameError);
        board.advance(2);
        release(InputKeyOk);
        screen("AirRounds", "Board status", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 6", "Add point");
    };
    runApp();
}

// Advance until Flipper has just read the status. Its next read follows 350 ms later.
void justAfterStatusRead(Board& board) {
    const auto reads = [&] { return std::count(board.sent.begin(), board.sent.end(), '\n'); };
    const auto before = reads();
    for (unsigned turn = 0; turn < 10 && reads() == before; ++turn) {
        board.advance();
    }
    assert(reads() > before);
    board.advance();
}

void heldStartAfterAnotherSession() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        justAfterStatusRead(board);
        board.advance(3);
        // OK goes down. Another client runs a whole session before the next read.
        press(InputKeyOk);
        board.otherClient("0000000000000009 00000001 round 020000000009 06");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(5);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        // 250 ms after the press, the release is still a Short. The Start is for an older session.
        release(InputKeyOk);
        board.advance();
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        assert(board.receiver.visits == 1);
        tap(InputKeyOk);
        screen("Starting...", "Waiting for board");
        board.advance();
        assert(board.receiver.visits == 2);
    };
    runApp();
}

void uartHeldByAnotherApp() {
    Board board;
    cardWith({4});
    emu::serialBusy = true;
    emu::scenario = [&] {
        board.advance(8);
        screen("AirRounds", "Board status", "Access points", "Last result");
        tap(InputKeyOk);
        screen("UART unavailable", "Close other UART apps");
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("No result available");
        tap(InputKeyBack);
        exitApp();
    };
    runApp();
    assert(emu::expansion.enabled);
    emu::serialBusy = false;
}

void incompatibleBoard() {
    Board board;
    cardWith({4});
    // The board lacks the passive visit feature.
    board.rewrite = [](std::string& line) {
        if (line.find(" 0000000000000000 00000000 ok ") == std::string::npos) {
            line.replace(line.size() - 3, 2, "02");
        }
    };
    emu::scenario = [&] {
        board.advance(8);
        screen("AirRounds", "Board status", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Update Devboard", "AirRounds firmware");
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("No result available");
    };
    runApp();
}

void lostStartReply() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        board.disconnect();
        tap(InputKeyOk);
        screen("Starting...", "Waiting for board");
        board.advance(20);
        screen("Link lost", "Session may run", "Check cable");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Access points", "Last result");
        tap(InputKeyOk);
        board.online = true;
        for (unsigned turn = 0; turn < 20 && !shows("Checking status"); ++turn) {
            board.advance();
        }
        screen("Checking status", "Command unconfirmed");
        board.advance(10);
        screen("Ready", "Press Back for menu");
        assert(board.receiver.visits == 0);
        board.goOffline();
        screen("Devboard offline", "Check cable");
        board.online = true;
        for (unsigned turn = 0; turn < 40 && !shows("Checking status"); ++turn) {
            board.advance();
        }
        // No command was in flight this time.
        screen("Checking status", "Waiting for board");
        board.advance(20);
        screen("Ready", "Press Back for menu");
        tap(InputKeyBack);
        screen("AirRounds", "Start session", "Access points", "Last result");
    };
    runApp();
}

void boardScreenAfterOpening() {
    for (unsigned points = 1; points <= 3; ++points) {
        for (const bool legacy : {false, true}) {
            Board board(legacy);
            cardWith(points == 1   ? std::initializer_list<uint8_t>{4}
                     : points == 2 ? std::initializer_list<uint8_t>{4, 5}
                                   : std::initializer_list<uint8_t>{4, 5, 6});
            emu::scenario = [&] {
                Board::redraw();
                screen("AirRounds", "Board status", "Access points", "Last result");
                tap(InputKeyOk);
                screen("Checking status", "Waiting for board");
                board.advance(8);
                if (legacy && points > 1) {
                    screen("Update Devboard", "Use newer board firmware");
                } else {
                    screen("Ready", "Press Back for menu");
                }
            };
            runApp();
        }
    }
}

void lastSessionNumber() {
    Board board(true);
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        // The board has used its last session ID.
        board.rewrite = [](std::string& line) {
            const auto at = line.find(" ok 00000000 idle none ");
            if (at != std::string::npos) {
                line.replace(at, 23, " ok ffffffff complete saved ");
            }
        };
        board.advance(10);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Board unavailable");
        tap(InputKeyBack);
        screen("AirRounds", "Board status", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Board unavailable", "Press Back for menu");
    };
    runApp();
}

void legacyBoardKeepsTheAddress() {
    Board board(true);
    cardWith({7});
    emu::scenario = [&] {
        // Flipper started this visit, so it knows the point.
        board.advance(8);
        tap(InputKeyOk);
        tap(InputKeyOk);
        board.advance();
        screen("Observing", "Point 1 / Ch 6", "Stop");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyOk);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyOk);
        screen("Point 1", "Captured to board SD", "02:00:00:00:00:07", "Ch 6", "File");
        tap(InputKeyRight);
        assert(board.receiver.saves.back().slot == 0);
        screen("Board SD file", "visit-", "123456789abcdef0-", "00000001.pcap");
        tap(InputKeyBack);
        tap(InputKeyBack);
        tap(InputKeyBack);
        tap(InputKeyBack);
        screen("AirRounds", "Start session", "Access points", "Last result");

        // Another client starts a single visit. The old reply names no point.
        board.otherClient("0000000000000005 00000002 start 020000000009 0b");
        board.advance(10);
        tap(InputKeyOk);
        screen("Observing", "Waiting for a beacon", "Stop");
        board.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        screen("Stopping...", "Waiting for board");
        board.advance(10);
        screen("Session stopped", "0 of 1 captured", "Results");
        tap(InputKeyOk);
        tap(InputKeyOk);
        screen("Point 1", "Point not seen", "Address unavailable");
        assert(board.receiver.visits == 2);
    };
    runApp();
}

void scanStopWhileSaving() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        // The board saves the first point. Stop can still skip the second.
        board.receiver.observed = true;
        board.advance(12);
        screen("Capture", "Captured 0/2", "Beacon - Saving", "TELEPORT", "Stop");
        tap(InputKeyOk);
        screen("Stopping...", "Waiting for board");
        board.advance();
        screen("Capture", "Captured 0/2", "Beacon - Saving", "TELEPORT");
        tap(InputKeyBack);
        screen("Capture", "Captured 0/2", "Beacon - Saving", "TELEPORT");
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "[ ] Guest", "v",
               "Scan", "Start (1)");
        assert(board.receiver.visits == 1);
        tap(InputKeyBack);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
    };
    runApp();
}

// Pin the drawn layout of each kind of screen: lists, menus, status, and the compact scan list.
void screenLayouts() {
    Board board;
    cardWith({4, 5});
    emu::scenario = [&] {
        board.advance(8);
        layout("Pb 4,11 AirRounds\n"
               "box 1,18 125x14\n"
               "Sw 5,28 Start session\n"
               "Sb 5,44 Access points\n"
               "Sb 5,60 Last result\n");
        tap(InputKeyDown);
        tap(InputKeyOk);
        layout("Pb 4,11 Access points\n"
               "box 1,18 125x14\n"
               "Sw 5,28 Point 1\n"
               "Sw 68,28 Ch 6\n"
               "Sb 5,44 Point 2\n"
               "Sb 68,44 Ch 6\n"
               "Sb 5,60 Add point\n");
        tap(InputKeyDown);
        tap(InputKeyOk);
        layout("Pb 4,11 Point 2\n"
               "Sb 5,24 02:00:00:00:00:05\n"
               "Sb 101,24 Ch 6\n"
               "box 1,28 125x14\n"
               "Sw 5,38 Edit\n"
               "Sb 5,52 Remove\n");
        tap(InputKeyDown);
        layout("Pb 4,11 Point 2\n"
               "Sb 5,24 02:00:00:00:00:05\n"
               "Sb 101,24 Ch 6\n"
               "Sb 5,38 Edit\n"
               "box 1,42 125x14\n"
               "Sw 5,52 Remove\n");
        tap(InputKeyBack);
        tap(InputKeyBack);
        tap(InputKeyOk);
        layout("Pb 4,11 Review session\n"
               "Sb 5,22 1 02:00:00:00:00:04\n"
               "Sb 102,22 Ch 6\n"
               "Sb 5,33 2 02:00:00:00:00:05\n"
               "Sb 102,33 Ch 6\n"
               "OK Start\n");
        board.goOffline();
        layout("Pb 4,11 Review session\n"
               "Sb 5,22 1 02:00:00:00:00:04\n"
               "Sb 102,22 Ch 6\n"
               "Sb 5,33 2 02:00:00:00:00:05\n"
               "Sb 102,33 Ch 6\n"
               "Sb 5,61 Devboard offline\n");
        board.comeBack();
        tap(InputKeyOk);
        board.advance();
        layout("Pb 4,11 Observing\n"
               "Sb 5,22 Point 1 / Ch 6\n"
               "OK Stop\n");
        board.receiver.observed = true;
        board.advance(12);
        screen("Saving point", "Point 1 / Ch 6", "Stop");
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        board.receiver.observed = true;
        board.advance(12);
        screen("Reception stopped", "Saving to board SD");
        board.receiver.outcome = Outcome::not_observed;
        board.advance(12);
        layout("Pb 4,11 Session ended\n"
               "Sb 5,22 1 of 2 captured\n"
               "OK Results\n");
        tap(InputKeyOk);
        layout("Pb 4,11 Results\n"
               "box 1,18 125x14\n"
               "Sw 5,28 Point 1\n"
               "Sw 68,28 Captured\n"
               "Sb 5,44 Point 2\n"
               "Sb 68,44 Not seen\n");
        tap(InputKeyOk);
        layout("Pb 4,11 Point 1\n"
               "Sb 5,22 Captured to board SD\n"
               "Sb 5,33 02:00:00:00:00:04\n"
               "Sb 5,44 Ch 6\n"
               "> File\n");
        tap(InputKeyRight);
        layout("Pb 4,11 Board SD file\n"
               "Sb 5,22 visit-\n"
               "Sb 5,33 123456789abcdef0-\n"
               "Sb 5,44 00000001-1.pcap\n");
    };
    runApp();
}

void captureModeChoice() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyUp); // Focus wraps to the last item, and the menu scrolls.
        layout("Pb 4,11 AirRounds\n"
               "Sb 5,22 Capture mode\n"
               "Sb 5,33 Flash board hardware\n"
               "Sb 5,44 Test board\n"
               "box 1,46 124x11\n"
               "Sw 5,55 About\n"
               "Sb 123,25 ^\n");
        tap(InputKeyDown);
        tap(InputKeyDown);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyOk);
        layout("Pb 4,11 Capture mode\n"
               "box 1,13 124x11\n"
               "Sw 5,22 * Beacon\n"
               "Sb 5,33 EAPOL\n"
               "Sb 5,44 Deauth\n"
               "Sb 101,44 Soon\n");

        // The active mode is unavailable on this board, which does not list 02: choosing it keeps
        // Beacon and says why.
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Not available yet");
        tap(InputKeyUp);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon");

        // EAPOL is available: choosing it selects it and returns with focus on Capture mode.
        tap(InputKeyOk);
        layout("Pb 4,11 AirRounds\n"
               "Sb 5,22 Scan area\n"
               "box 1,24 124x11\n"
               "Sw 5,33 Capture mode\n"
               "Sb 5,44 Flash board hardware\n"
               "Sb 5,55 Test board\n"
               "Sb 123,63 v\n");
        tap(InputKeyOk); // Reopen; focus starts on the chosen mode.
        screen("Capture mode", "Beacon", "* EAPOL", "Deauth", "Soon");

        // Choosing Beacon again returns with focus on Capture mode.
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon");

        // An unknown board state locks the choice; the lock ends when the board answers again.
        board.goOffline();
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Board state unknown");
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Board state unknown");
        board.comeBack();
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon");
    };
    runApp();
}

void captureModeLockedDuringRound() {
    Board board(false, true);
    cardWith({});
    board.otherClient("0000000000000005 00000001 round 020000000001 0b 020000000002 0b");
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Session active");
        tap(InputKeyDown);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Session active");
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Session active");

        // A lost link during the round hides the last known state behind the unknown one.
        board.goOffline();
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon",
               "Board state unknown");
        board.comeBack();
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon", "Session active");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
    };
    runApp();
}

// Choosing EAPOL and starting a round: the list heads with the EAPOL title, the round line carries
// the mode 01 tail, and the saved point shows the EAPOL success label.
void captureModeEapolRound() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        // Open the Capture mode screen and select EAPOL, landing home on Capture mode.
        tap(InputKeyUp);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Capture mode", "* Beacon", "EAPOL", "Deauth", "Soon");
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        // Scan; the network list heads with the EAPOL title.
        tap(InputKeyUp);
        tap(InputKeyOk);
        board.advance(60);
        screen("Handshakes", "0/5 selected", "[ ] TELEPORT", "[ ] Studio", "[ ] Guest", "v",
               "Scan");
        tap(InputKeyOk);
        screen("Handshakes", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        board.sent.clear();
        tap(InputKeyRight);
        assert(board.sent.find(" mode 01\n") != std::string::npos);
        board.advance(10);
        screen("Capture", "Captured 0/1", "EAPOL - Listening", "TELEPORT", "Stop");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Handshakes", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "[ ] Guest", "v",
               "Scan");
    };
    runApp();
}

void scanLayouts() {
    Board board(false, true);
    cardWith({});
    board.scanner.names({"ABCDEFGHIJKLMNOPQRS", "Studio", "Guest", "Loft", "Atlas"});
    emu::scenario = [&] {
        board.advance(8);
        layout("Pb 4,11 AirRounds\n"
               "box 1,13 124x11\n"
               "Sw 5,22 Scan area\n"
               "Sb 5,33 Capture mode\n"
               "Sb 5,44 Flash board hardware\n"
               "Sb 5,55 Test board\n"
               "Sb 123,63 v\n");
        tap(InputKeyOk);
        board.advance(60);
        layout("Pb 4,11 Beacons\n"
               "Sb 64,11 0/5 selected\n"
               "box 1,13 124x11\n"
               "Sw 5,22 [ ] ABCDEFGHIJKLMNOP\n"
               "Sb 5,33 [ ] Studio\n"
               "Sb 5,44 [ ] Guest\n"
               "Sb 123,49 v\n"
               "< Scan\n");
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        layout("Pb 4,11 Beacons\n"
               "Sb 64,11 3/5 selected\n"
               "Sb 5,22 [ ] Guest\n"
               "Sb 5,33 [ ] Loft\n"
               "box 1,35 124x11\n"
               "Sw 5,44 [x] Atlas\n"
               "Sb 123,25 ^\n"
               "< Scan\n"
               "> Start (3)\n");
        tap(InputKeyRight);
        board.advance(10);
        layout("Pb 4,11 Capture\n"
               "Sb 64,11 Captured 0/3\n"
               "Sb 5,22 Beacon - Listening\n"
               "Sb 5,33 ABCDEFGHIJKLMNOP\n"
               "OK Stop\n");
        for (unsigned point = 0; point < 3; ++point) {
            board.receiver.observed = true;
            board.receiver.outcome = point == 1 ? Outcome::saved : Outcome::not_observed;
            board.advance(12);
        }
        layout("Pb 4,11 Beacons\n"
               "Sb 64,11 0/5 selected\n"
               "box 1,13 124x11\n"
               "Sw 5,22 [ ] ABCDEFGHIJ\n"
               "Sw 81,22 Not seen\n"
               "Sb 5,33 + Studio\n"
               "Sb 96,33 Saved\n"
               "Sb 5,44 [ ] Guest\n"
               "Sb 123,49 v\n"
               "< Scan\n");
        tap(InputKeyUp);
        layout("Pb 4,11 Beacons\n"
               "Sb 64,11 0/5 selected\n"
               "Sb 5,22 [ ] Guest\n"
               "Sb 5,33 [ ] Loft\n"
               "box 1,35 124x11\n"
               "Sw 5,44 [ ] Atlas\n"
               "Sw 81,44 Not seen\n"
               "Sb 123,25 ^\n"
               "< Scan\n");
        tap(InputKeyBack);
        tap(InputKeyDown);
        layout("Pb 4,11 AirRounds\n"
               "Sb 5,22 Scan area\n"
               "box 1,24 124x11\n"
               "Sw 5,33 Capture mode\n"
               "Sb 5,44 Flash board hardware\n"
               "Sb 5,55 Test board\n"
               "Sb 123,63 v\n");
        tap(InputKeyUp);

        // Another client runs four of the scanned points. Results use the scanned names.
        board.otherClient("0000000000000099 00000002 round 020000000001 0b 020000000002 0b "
                          "020000000003 0b 020000000004 0b");
        board.advance(10);
        tap(InputKeyOk);
        for (unsigned point = 0; point < 4; ++point) {
            board.receiver.observed = true;
            board.receiver.outcome = point == 2 ? Outcome::saved : Outcome::not_observed;
            board.advance(12);
        }
        layout("Pb 4,11 Session ended\n"
               "Sb 5,22 1 of 4 captured\n"
               "OK Results\n");
        tap(InputKeyOk);
        layout("Pb 4,11 Results\n"
               "box 1,13 124x11\n"
               "Sw 5,22 ABCDEFGHIJKLMN\n"
               "Sw 81,22 Not seen\n"
               "Sb 5,33 Studio\n"
               "Sb 81,33 Not seen\n"
               "Sb 5,44 Guest\n"
               "Sb 81,44 Captured\n"
               "Sb 123,49 v\n");
        tap(InputKeyUp);
        layout("Pb 4,11 Results\n"
               "Sb 5,22 Studio\n"
               "Sb 81,22 Not seen\n"
               "Sb 5,33 Guest\n"
               "Sb 81,33 Captured\n"
               "box 1,35 124x11\n"
               "Sw 5,44 Loft\n"
               "Sw 81,44 Not seen\n"
               "Sb 123,25 ^\n");
        tap(InputKeyUp);
        tap(InputKeyOk);
        layout("Pb 4,11 Point 3\n"
               "Sb 5,22 Captured to board SD\n"
               "Sb 5,33 Guest\n"
               "> File\n");
        tap(InputKeyRight);
        layout("Pb 4,11 Board SD file\n"
               "Sb 5,22 visit-\n"
               "Sb 5,33 123456789abcdef0-\n"
               "Sb 5,44 00000002-3.pcap\n");
    };
    runApp();
}

void freshReadIdsEachRun() {
    std::string first;
    for (unsigned run = 0; run < 2; ++run) {
        Board board;
        cardWith({});
        emu::scenario = [&] {
            board.advance(8);
            screen("AirRounds", "Set up points", "Access points", "Last result");
            exitApp();
        };
        runApp(run == 0 ? 0x12 : 0x34);
        // A new app run must not reuse the read IDs of the last one.
        const auto hello = board.sent.substr(0, board.sent.find('\n'));
        assert(hello.find(" hello") != std::string::npos);
        if (run == 0) {
            first = hello;
        } else {
            assert(hello != first);
        }
    }
}

void openingShowsDeauthSessionOnce() {
    {
        Board board;
        cardWith({4});
        board.otherClient("0000000000000005 00000001 round 020000000009 06");
        emu::scenario = [&] {
            board.advance(8);
            screen("Observing", "Point 1 / Ch 6", "Stop");
            tap(InputKeyBack);
            screen("AirRounds", "Session status", "Access points", "Last result");
            tap(InputKeyOk);
            screen("Observing", "Point 1 / Ch 6", "Stop");
            tap(InputKeyBack);
            exitApp();
        };
        runApp();
    }
    {
        // A key press before the first reply keeps the screen the operator chose.
        Board board;
        cardWith({});
        board.otherClient("0000000000000005 00000001 round 020000000009 06");
        emu::scenario = [&] {
            Board::redraw();
            tap(InputKeyOk);
            screen("Access points", "Add point");
            board.advance(8);
            screen("Access points", "Add point");
            tap(InputKeyBack);
            exitApp();
        };
        runApp();
    }
    {
        Board board;
        cardWith({});
        board.otherClient("0000000000000005 00000001 round 020000000009 06");
        emu::scenario = [&] {
            Board::redraw();
            tap(InputKeyDown);
            board.advance(8);
            screen("AirRounds", "Session status", "Access points", "Last result");
            exitApp();
        };
        runApp();
    }
    {
        // Only a session active at entry opens by itself.
        Board board;
        cardWith({4});
        emu::scenario = [&] {
            board.advance(8);
            screen("AirRounds", "Start session", "Access points", "Last result");
            board.otherClient("0000000000000005 00000001 round 020000000009 06");
            board.advance(10);
            screen("AirRounds", "Session status", "Access points", "Last result");
            exitApp();
        };
        runApp();
    }
}

void backWhileStarting() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        tap(InputKeyOk);
        screen("Starting...", "Waiting for board");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Access points", "Last result");
        board.advance();
        screen("AirRounds", "Session status", "Access points", "Last result");
        tap(InputKeyOk);
        screen("Observing", "Point 1 / Ch 6", "Stop");
    };
    runApp();
}

void rejectedCommands() {
    {
        Board board;
        cardWith({4});
        emu::scenario = [&] {
            board.advance(8);
            tap(InputKeyOk);
            // Another client used a newer request ID. Flipper does not know it yet.
            assert(board.command("0000000000000050 00000009 stop").find(" unknown_session ") !=
                   std::string::npos);
            tap(InputKeyOk);
            board.advance();
            // Flipper reads the board state before it shows the rejection.
            screen("Checking status", "Waiting for board");
            board.advance(3);
            screen("Start rejected", "Session status updated", "Refresh");
            board.advance(10);
            screen("Start rejected", "Session status updated", "Refresh");
            tap(InputKeyBack);
            screen("AirRounds", "Start session", "Access points", "Last result");
            tap(InputKeyOk);
            tap(InputKeyOk);
            board.advance();
            screen("Observing", "Point 1 / Ch 6", "Stop");

            assert(board.command("0000000000000090 00000009 stop").find(" unknown_session ") !=
                   std::string::npos);
            tap(InputKeyOk);
            screen("Stopping...", "Waiting for board");
            board.advance(3);
            screen("Stop rejected", "Session status updated", "Refresh");
            tap(InputKeyOk);
            screen("Checking status", "Waiting for board");
            board.advance(10);
            screen("Observing", "Point 1 / Ch 6", "Stop");
            board.receiver.outcome = Outcome::not_observed;
            tap(InputKeyOk);
            board.advance(10);
            screen("Session stopped", "0 of 1 captured", "Results");
            assert(board.receiver.visits == 1);
        };
        runApp();
    }
    {
        // After an error result, the status screen keeps the error.
        Board board;
        cardWith({4});
        emu::scenario = [&] {
            startOnePoint(board);
            board.receiver.observed = true;
            board.receiver.outcome = Outcome::storage_error;
            board.advance(12);
            screen("SD write failed", "0 of 1 captured", "Check board SD", "Results");
            tap(InputKeyBack);
            tap(InputKeyOk);
            board.command("0000000000000050 00000009 stop");
            tap(InputKeyOk);
            board.advance(3);
            screen("SD write failed", "0 of 1 captured", "Check board SD", "Results");
        };
        runApp();
    }
}

void restartDuringFirstSession() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        board.restart(0x0fedcba987654321ULL);
        board.advance(30);
        // No earlier result exists, so OK offers nothing.
        screen("Devboard restarted", "Check board SD");
        board.otherClient("0000000000000005 00000001 round 020000000009 06");
        board.advance(10);
        screen("Observing", "Point 1 / Ch 6", "Stop");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "1 of 1 captured", "Results");
    };
    runApp();
}

void scanRestart() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/1", "Beacon - Listening", "TELEPORT", "Stop");
        board.restart(0x0fedcba987654321ULL);
        board.advance(30);
        screen("Devboard restarted", "Check board SD");
        // The list stays. The interrupted point keeps its selection.
        tap(InputKeyBack);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        // A new Start clears the notice, even when its reply is lost.
        board.disconnect();
        tap(InputKeyRight);
        board.advance(30);
        board.comeBack();
        // The board can start a round from the list. The empty manual profile does not matter.
        screen("Ready", "Press Back for menu");
        tap(InputKeyBack);
        tap(InputKeyRight);
        board.advance(10);
        screen("Capture", "Captured 0/1", "Beacon - Listening", "TELEPORT", "Stop");
        board.restart(0x1fedcba987654321ULL);
        board.advance(30);
        screen("Devboard restarted", "Check board SD");
        board.otherClient("0000000000000005 00000001 round 020000000002 0b");
        board.advance(10);
        screen("Capture", "Captured 0/1", "Beacon - Listening", "Studio", "Stop");
        // This session is not the one Flipper started, so Back leaves it.
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
    };
    runApp();
}

void lostScanStart() {
    Board board(false, true);
    cardWith({});
    board.receiver.journalSupported = true;
    board.receiver.journal = JournalState::failed;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "Log failed", "+ TELEPORT", "Saved", "[ ] Studio", "[ ] Guest", "v",
               "Scan");
        tap(InputKeyDown);
        tap(InputKeyOk);
        board.disconnect();
        tap(InputKeyRight);
        screen("Starting...", "Waiting for board");
        board.advance(30);
        screen("Link lost", "Session may run", "Check cable");
        tap(InputKeyBack);
        screen("Link lost", "Session may run", "Check cable");
        board.comeBack();
        screen("Session ended", "1 of 1 captured", "Session log failed", "Results");
        // The board never started the new round. Back returns to the list.
        tap(InputKeyBack);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "[ ] Guest", "v",
               "Scan", "Start (1)");
        assert(board.receiver.visits == 1);
        // A later round under the same ID ends while the main menu is open. The menu stays.
        tap(InputKeyBack);
        board.otherClient("0000000000000050 00000002 round 020000000002 0b");
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        // That round was not Flipper's. Its result stays out of the list.
        tap(InputKeyOk);
        board.advance(60);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "[ ] Guest", "v",
               "Scan", "Start (1)");
    };
    runApp();
}

void anotherClientTakesTheSessionId() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        board.disconnect();
        tap(InputKeyRight);
        board.otherClient("0000000000000050 00000001 round 020000000002 0b");
        board.advance(30);
        board.comeBack();
        screen("Capture", "Captured 0/1", "Beacon - Listening", "Studio", "Stop");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
    };
    runApp();

    // The lost Start's session ID ran and ended elsewhere. Back leaves the next session as well.
    Board later(false, true);
    cardWith({});
    emu::scenario = [&] {
        later.advance(8);
        tap(InputKeyOk);
        later.advance(60);
        tap(InputKeyOk);
        later.disconnect();
        tap(InputKeyRight);
        later.otherClient("0000000000000050 00000001 round 020000000002 0b");
        later.advance(5);
        later.receiver.observed = true;
        later.receiver.outcome = Outcome::saved;
        later.advance(15);
        later.otherClient("0000000000000051 00000002 round 020000000003 0b");
        later.advance(30);
        later.comeBack();
        screen("Capture", "Captured 0/1", "Beacon - Listening", "Guest", "Stop");
        tap(InputKeyBack);
        screen("AirRounds", "Session status", "Capture mode", "Flash board hardware", "Test board",
               "v");
    };
    runApp();
}

void scanStopAfterTheVisit() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        // The first visit ended without a beacon just before Stop reached the board.
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        board.advance(15);
        screen("Beacons", "1/5 selected", "[ ] TELEPORT", "Not seen", "[x] Studio", "[ ] Guest",
               "v", "Scan", "Start (1)");
    };
    runApp();

    // The Stop reply ends the round. Its detail read follows 350 ms later. Back in between
    // already returns to the list, and the later detail does not move the operator back.
    Board early(false, true);
    cardWith({});
    emu::scenario = [&] {
        early.advance(8);
        tap(InputKeyOk);
        early.advance(60);
        tap(InputKeyOk);
        tap(InputKeyRight);
        early.advance(10);
        early.receiver.observed = true;
        early.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        early.advance();
        screen("Session stopped", "0 of 1 captured", "Results");
        tap(InputKeyBack);
        screen("Beacons", "1/5 selected", "[x] TELEPORT", "[ ] Studio", "[ ] Guest", "v", "Scan",
               "Start (1)");
        tap(InputKeyBack);
        early.advance(10);
        screen("AirRounds", "Scan area", "Capture mode", "Flash board hardware", "Test board", "v");
        tap(InputKeyOk);
        early.advance(60);
        screen("Beacons", "0/5 selected", "[ ] TELEPORT", "Not seen", "[ ] Studio", "[ ] Guest",
               "v", "Scan");
    };
    runApp();
}

void retryAfterARound() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyUp);
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(15);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "[ ] Guest", "v", "Scan");
        // The focus moved to the missed row. A saved row cannot be selected.
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "Not seen",
               "[ ] Guest", "v", "Scan", "Start (1)");
        board.advance(10);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "Not seen",
               "[ ] Guest", "v", "Scan", "Start (1)");
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "+ TELEPORT", "Saved", "[x] Studio", "Not seen",
               "[ ] Guest", "v", "Scan", "Start (1)");
        // OK held while Right finds nothing to start does not select the row.
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "[ ] Guest", "v", "Scan");
        press(InputKeyOk);
        tap(InputKeyRight);
        release(InputKeyOk);
        screen("Beacons", "0/5 selected", "+ TELEPORT", "Saved", "[ ] Studio", "Not seen",
               "[ ] Guest", "v", "Scan");
    };
    runApp();
}

void beaconListChanges() {
    Board board(false, true);
    cardWith({});
    board.scanner.names({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        screen("Beacons", "0/5 selected", "No networks found", "Scan");
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "0/5 selected", "No networks found", "Scan");

        // Rows follow their address when a rescan changes the order.
        board.scanner.names({"Lab", "", "ABCDEFGHIJKLMNO", "Shed", "Yard", "Pier"},
                            {1, 2, 3, 4, 5, 6});
        tap(InputKeyLeft);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] Lab", "[ ] Hidden network", "[ ] ABCDEFGHIJKLMNO",
               "v", "Scan");
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        screen("Beacons", "2/5 selected", "[ ] ABCDEFGHIJKLMNO", "[ ] Shed", "[x] Yard", "^", "v",
               "Scan", "Start (2)");
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::not_observed;
        board.advance(12);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        screen("Beacons", "0/5 selected", "[ ] Lab", "Not seen", "[ ] Hidden network",
               "[ ] ABCDEFGHIJKLMNO", "v", "Scan");
        tap(InputKeyDown);
        tap(InputKeyOk);
        board.scanner.names({"Pier", "Yard", "Shed", "ABCDEFGHIJKLMNO", "", "Lab"},
                            {6, 5, 4, 3, 2, 1});
        tap(InputKeyLeft);
        board.advance(60);
        tap(InputKeyUp);
        tap(InputKeyUp);
        screen("Beacons", "1/5 selected", "[ ] ABCDEFGHIJKLMNO", "[x] Hidden network", "[ ] Lab",
               "Not seen", "^", "Scan", "Start (1)");
        tap(InputKeyUp);
        tap(InputKeyUp);
        tap(InputKeyUp);
        screen("Beacons", "1/5 selected", "[ ] Pier", "+ Yard", "Saved", "[ ] Shed", "v", "Scan",
               "Start (1)");

        // A shorter list moves the focus to its last row.
        board.scanner.names({"Lab", "Pier"}, {1, 6});
        tap(InputKeyLeft);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] Lab", "Not seen", "[ ] Pier", "Scan");
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "[ ] Lab", "Not seen", "[x] Pier", "Scan", "Start (1)");

        // Rows past the end of a shorter list must not come back.
        board.scanner.names({"A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8"},
                            {11, 12, 13, 14, 15, 16, 17, 18});
        tap(InputKeyLeft);
        board.advance(60);
        tap(InputKeyUp);
        tap(InputKeyUp);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "[ ] A5", "[ ] A6", "[x] A7", "^", "v", "Scan",
               "Start (1)");
        board.scanner.names({"A1", "A2", "A3", "A4", "A5", "A6"}, {11, 12, 13, 14, 15, 16});
        tap(InputKeyLeft);
        board.advance(60);
        screen("Beacons", "0/5 selected", "[ ] A4", "[ ] A5", "[ ] A6", "^", "Scan");
        board.scanner.names({"A1", "A2", "A3", "A4", "A5", "A6", "A9"},
                            {11, 12, 13, 14, 15, 16, 19});
        tap(InputKeyLeft);
        board.advance(60);
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] A5", "[ ] A6", "[ ] A9", "^", "Scan");

        // The board sends up to 16 points.
        std::vector<std::string> many;
        std::vector<uint8_t> addresses;
        for (unsigned index = 0; index < 16; ++index) {
            many.push_back("Net" + std::to_string(index + 1));
            addresses.push_back(static_cast<uint8_t>(0x20 + index));
        }
        many[3] = std::string(16, ' ') + "X";
        board.scanner.names(many, addresses);
        for (unsigned scan = 0; scan < 2; ++scan) {
            tap(InputKeyLeft);
            board.advance(80);
        }
        for (unsigned row = 0; row < 9; ++row) {
            tap(InputKeyDown);
        }
        screen("Beacons", "0/5 selected", "[ ] Net14", "[ ] Net15", "[ ] Net16", "^", "Scan");
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] Net1", "[ ] Net2", "[ ] Net3", "v", "Scan");
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyDown);
        screen("Beacons", "0/5 selected", "[ ] Net2", "[ ] Net3", "[ ] Hidden network", "^", "v",
               "Scan");
        // A full list replaced by 16 new addresses: no new row matches an old one.
        for (auto& address : addresses) {
            address = static_cast<uint8_t>(address + 0x20);
        }
        board.scanner.names(many, addresses);
        tap(InputKeyLeft);
        board.advance(80);
        screen("Beacons", "0/5 selected", "[ ] Net2", "[ ] Net3", "[ ] Hidden network", "^", "v",
               "Scan");
        // An empty list resets the focus for the next one.
        board.scanner.names({});
        tap(InputKeyLeft);
        board.advance(60);
        screen("Beacons", "0/5 selected", "No networks found", "Scan");
        board.scanner.names({"Lab", "Pier", "Yard", "Shed", "Mill"});
        tap(InputKeyLeft);
        board.advance(60);
        tap(InputKeyOk);
        screen("Beacons", "1/5 selected", "[x] Lab", "[ ] Pier", "[ ] Yard", "v", "Scan",
               "Start (1)");
    };
    runApp();
}

void beaconSelectionLimitAndFocus() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        for (unsigned row = 0; row < 5; ++row) {
            tap(InputKeyOk);
            tap(InputKeyDown);
        }
        tap(InputKeyOk);
        screen("Beacons", "Maximum 5", "[x] Loft", "[x] Atlas", "[ ] Workshop", "^", "Scan",
               "Start (5)");
        // A rescan clears the note.
        tap(InputKeyLeft);
        board.advance(60);
        screen("Beacons", "5/5 selected", "[x] Loft", "[x] Atlas", "[ ] Workshop", "^", "Scan",
               "Start (5)");
        // Moving through the main menu keeps the list focus.
        tap(InputKeyUp);
        tap(InputKeyBack);
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyUp);
        tap(InputKeyUp);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        screen("Beacons", "4/5 selected", "[x] Guest", "[x] Loft", "[ ] Atlas", "^", "v", "Scan",
               "Start (4)");

        // A scan that the client cannot send keeps its failure on screen.
        board.goOffline();
        tap(InputKeyLeft);
        screen("Scan failed", "Check Devboard", "Scan");
        board.advance(5);
        screen("Scan failed", "Check Devboard", "Scan");
        tap(InputKeyBack);
        screen("Beacons", "Board offline", "[x] Guest", "[x] Loft", "[ ] Atlas", "^", "v", "Scan");
    };
    runApp();
}

// The ByteInput callbacks reach the app through the dispatcher's event queue.
void addressEditorEvents() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        bssid(0);
        screen("Invalid BSSID");
        // Changing a digit resets the heading and keeps the editor open.
        auto* keyboard = emu::keyboard;
        keyboard->buffer[0] = 2;
        keyboard->view.locked = true;
        keyboard->changed(keyboard->context);
        keyboard->view.locked = false;
        emu::customEvents();
        Board::redraw();
        screen("Enter BSSID");
        assert(editingAddress());

        // A confirmation that arrives after Back no longer counts.
        const uint8_t address[]{2, 0, 0, 0, 0, 5};
        memcpy(keyboard->buffer, address, sizeof(address));
        keyboard->view.locked = true;
        keyboard->saved(keyboard->context);
        keyboard->view.locked = false;
        emu::input(InputKeyBack, InputTypePress);
        key(InputKeyBack, InputTypeShort);
        key(InputKeyBack, InputTypeRelease);
        screen("Access points", "Point 1", "Ch 6", "Add point");
        tap(InputKeyOk);
        // Nor does one from an earlier editor that reaches the next one.
        memcpy(keyboard->buffer, address, sizeof(address));
        keyboard->view.locked = true;
        keyboard->saved(keyboard->context);
        keyboard->view.locked = false;
        emu::input(InputKeyBack, InputTypePress);
        emu::input(InputKeyBack, InputTypeShort);
        emu::input(InputKeyBack, InputTypeRelease);
        Board::redraw();
        emu::input(InputKeyOk, InputTypePress);
        emu::input(InputKeyOk, InputTypeShort);
        emu::input(InputKeyOk, InputTypeRelease);
        emu::customEvents();
        Board::redraw();
        assert(editingAddress());
        screen("Enter BSSID");

        // A save failure, then Back and a new address, starts a clean channel step.
        bssid(5);
        emu::disk.failSync = true;
        tap(InputKeyOk);
        screen("Save not confirmed", "02:00:00:00:00:05", "< Channel 1 >", "Save");
        emu::disk.failSync = false;
        tap(InputKeyBack);
        bssid(6);
        screen("Select channel", "02:00:00:00:00:06", "< Channel 1 >", "Save");
        // OK held while the channel changes does not save.
        press(InputKeyOk);
        tap(InputKeyRight);
        release(InputKeyOk);
        screen("Select channel", "02:00:00:00:00:06", "< Channel 2 >", "Save");
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 6", "Point 2", "Ch 2", "Add point");

        // The point menu has two choices.
        tap(InputKeyOk);
        screen("Point 2", "02:00:00:00:00:06", "Ch 2", "Edit", "Remove");
        tap(InputKeyDown);
        tap(InputKeyDown);
        tap(InputKeyOk);
        assert(editingAddress());
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        emu::disk.failSync = true;
        tap(InputKeyOk);
        screen("Save not confirmed", "02:00:00:00:00:06", "Remove");
        emu::disk.failSync = false;
        tap(InputKeyBack);
        layout("Pb 4,11 Point 2\n"
               "Sb 5,24 02:00:00:00:00:06\n"
               "Sb 101,24 Ch 2\n"
               "Sb 5,38 Edit\n"
               "box 1,42 125x14\n"
               "Sw 5,52 Remove\n");
        tap(InputKeyOk);
        screen("Remove point?", "02:00:00:00:00:06", "Remove");
        tap(InputKeyOk);
        screen("Access points", "Point 1", "Ch 6", "Add point");
    };
    runApp();
}

void okTiming() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        // A long press is not a selection.
        press(InputKeyOk);
        board.advance(10);
        release(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        // A long Back works as Back.
        press(InputKeyBack);
        board.advance(6);
        screen("AirRounds", "Start session", "Access points", "Last result");
        release(InputKeyBack);
        screen("AirRounds", "Start session", "Access points", "Last result");
        tap(InputKeyOk);

        // OK held across status ticks still starts.
        press(InputKeyOk);
        board.advance(5);
        release(InputKeyOk);
        screen("Starting...", "Waiting for board");
        board.advance();
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyOk);
        // Down on a one-row list while OK is held is a new choice.
        press(InputKeyOk);
        tap(InputKeyDown);
        release(InputKeyOk);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyBack);
        tap(InputKeyBack);
        tap(InputKeyOk);

        // A tick that the GUI has not drawn yet keeps the OK binding.
        emu::clockMs += 50;
        emu::tick();
        tap(InputKeyOk);
        screen("Starting...", "Waiting for board");
        board.advance();
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "1 of 1 captured", "Results");
        tap(InputKeyBack);
        tap(InputKeyOk);
        // Down does not move anything on the review screen. OK still starts.
        press(InputKeyOk);
        tap(InputKeyDown);
        release(InputKeyOk);
        screen("Starting...", "Waiting for board");
        board.advance();
        assert(board.receiver.visits == 3);
    };
    runApp();
}

void resultsReturnToTheirScreen() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        tap(InputKeyOk);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyBack);
        // Back returns to the live status screen, which shows the next session.
        board.otherClient("0000000000000050 00000002 round 020000000009 06");
        board.advance(10);
        screen("Observing", "Point 1 / Ch 6", "Stop");
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        screen("Session ended", "1 of 1 captured", "Results");
        // Last result keeps the finished session while another one runs.
        tap(InputKeyOk);
        screen("Results", "Point 1", "Captured");
        tap(InputKeyOk);
        screen("Point 1", "Captured to board SD", "02:00:00:00:00:04", "Ch 6", "File");
        tap(InputKeyBack);
        tap(InputKeyBack);
        screen("Session ended", "1 of 1 captured", "Results");
    };
    runApp();
}

void aboutShowsVersions() {
    Board board(false, true);
    cardWith({});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyUp); // Focus wraps to About, the last item.
        tap(InputKeyOk);
        // The app asks the board for its version only when About opens.
        screen("About", "App", "v" FAP_VERSION, "Board", "Reading...");
        board.advance(4);
        screen("About", "App", "v" FAP_VERSION, "Board", "v" FAP_VERSION);
        assert(board.sent.find(" 00000000 firmware\n") != std::string::npos);
        tap(InputKeyBack);
        screen("AirRounds", "Capture mode", "Flash board hardware", "Test board", "About", "^");

        // A board that stops answering takes its version with it.
        board.disconnect();
        board.advance(40); // Past the 900 ms reply deadline.
        tap(InputKeyOk);
        screen("About", "App", "v" FAP_VERSION, "Board", "Not connected");

        // The board comes back. The app reads its version again after the new hello.
        board.online = true;
        board.advance(40);
        screen("About", "App", "v" FAP_VERSION, "Board", "v" FAP_VERSION);
    };
    runApp();
}

void boardFirmwareChanges() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        tap(InputKeyOk);
        layout("Pb 4,11 Results\n"
               "box 1,18 125x14\n"
               "Sw 5,28 Point 1\n"
               "Sw 68,28 Captured\n");
        // The board comes back with scan firmware. The open results switch to the compact list.
        board.restart(0x0fedcba987654321ULL, true);
        board.advance(30);
        layout("Pb 4,11 Results\n"
               "box 1,13 124x11\n"
               "Sw 5,22 Point 1\n"
               "Sw 81,22 Captured\n");
    };
    runApp();

    Board scan(false, true);
    cardWith({});
    emu::scenario = [&] {
        scan.advance(8);
        tap(InputKeyOk);
        scan.advance(60);
        tap(InputKeyOk);
        tap(InputKeyRight);
        scan.advance(10);
        scan.receiver.observed = true;
        scan.receiver.outcome = Outcome::saved;
        scan.advance(12);
        // Older firmware returns. Results then use point numbers, not the scanned names.
        scan.restart(0x0fedcba987654321ULL, false);
        scan.advance(30);
        tap(InputKeyBack);
        screen("AirRounds", "Set up points", "Access points", "Last result");
        tap(InputKeyUp);
        tap(InputKeyOk);
        tap(InputKeyOk);
        screen("Results", "Point 1", "Captured");
    };
    runApp();
}

void repliesSplitAcrossTicks() {
    Board board;
    cardWith({4});
    board.chunk = 60;
    emu::scenario = [&] {
        board.advance(16);
        screen("AirRounds", "Start session", "Access points", "Last result");
        tap(InputKeyOk);
        tap(InputKeyOk);
        board.advance(4);
        screen("Observing", "Point 1 / Ch 6", "Stop");
    };
    runApp();
}

// Feed bytes that are not a reply straight to the UART interrupt.
void noise(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        emu::serialEvent(FuriHalSerialRxEventData, 'x');
    }
}

// The app buffers 512 received bytes and reads all of them each tick. More is a damaged link.
void receiveBurstLimits() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        noise(512);
        emu::tick();
        noise(512);
        emu::tick();
        Board::redraw();
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
        noise(513);
        emu::tick();
        Board::redraw();
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Devboard offline");
    };
    runApp();
}

// A Start reply lost on older firmware leaves the point unknown, even when the visit runs.
void legacyLostStartReply() {
    Board board(true);
    cardWith({7});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        tap(InputKeyOk);
        board.goOffline();
        board.comeBack();
        screen("Observing", "Waiting for a beacon", "Stop");
        assert(board.receiver.visits == 1);
    };
    runApp();
}

// Rows clip their name so it ends 9 px before the result, or within 118 px without one.
void beaconNamesClip() {
    Board board(false, true);
    cardWith({});
    board.scanner.names({"ABCDEFGHIJ||", "ABCDEFGHIJ|||"});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyRight);
        board.advance(10);
        for (unsigned point = 0; point < 2; ++point) {
            board.receiver.observed = true;
            board.receiver.outcome = Outcome::not_observed;
            board.advance(12);
        }
        // 72 px fits before "Not seen". 73 px loses its last glyph.
        screen("Beacons", "0/5 selected", "[ ] ABCDEFGHIJ||", "Not seen", "[ ] ABCDEFGHIJ||",
               "Not seen", "Scan");

        const char* const names[]{"ABCDEFGHIJKLMNO", "ABCDEFGHIJKLMN``", "ABCDEFGHIJKLMNO`"};
        const char* const tenth[]{"[ ] 10. ABCDEFGHIJKLMNO", "[ ] 10. ABCDEFGHIJKLMN``",
                                  "[ ] 10. ABCDEFGHIJKLMNO"};
        for (unsigned width = 0; width < 3; ++width) {
            board.scanner.names(std::vector<std::string>(10, names[width]));
            tap(InputKeyLeft);
            board.advance(60);
            tap(InputKeyUp);
            // 115 px and 118 px fit. 119 px loses its last glyph.
            assert(shows(tenth[width]));
            tap(InputKeyDown);
        }
    };
    runApp();
}

// The board firmware maps a zero random boot ID to 1, so boot 1 is a real boot.
void restartFromBootOne() {
    Board board;
    board.restart(1);
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        board.restart(0x0fedcba987654321ULL);
        board.advance(30);
        screen("Devboard restarted", "Check board SD");
    };
    runApp();
}

// The 32-bit tick count wraps after 49 days. A session across the wrap works as any other.
void sessionAcrossTickWrap() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        startOnePoint(board);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        screen("Session ended", "1 of 1 captured", "Results");
        assert(emu::clockMs < 1000U);
        board.goOffline();
        screen("Devboard offline", "Check cable");
        board.comeBack();
        screen("Session ended", "1 of 1 captured", "Results");
    };
    runApp(0x12, UINT32_MAX - 500U);
}

// The HAL reports an idle line between bursts. It is neither data nor an error.
void idleLineIsIgnored() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        for (unsigned turn = 0; turn < 20; ++turn) {
            emu::serialEvent(FuriHalSerialRxEventIdle);
            board.advance();
        }
        screen("Review session", "1 02:00:00:00:00:04", "Ch 6", "Start");
    };
    runApp();
}

// ByteInput keeps a long Back: it clears the byte under the cursor instead of leaving.
void longBackInTheEditor() {
    Board board;
    cardWith({4});
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyDown);
        tap(InputKeyOk);
        tap(InputKeyOk);
        tap(InputKeyOk);
        assert(editingAddress());
        bssid(0);
        screen("Invalid BSSID");
        emu::keyboard->buffer[0] = 2;
        press(InputKeyBack);
        board.advance(6);
        release(InputKeyBack);
        assert(editingAddress());
        assert(emu::keyboard->buffer[0] == 0);
        screen("Enter BSSID");
        tap(InputKeyBack);
        screen("Point 1", "02:00:00:00:00:04", "Ch 6", "Edit", "Remove");
    };
    runApp();
}

} // namespace

int main() {
    {
        Board board;
        cardWith({});
        board.receiver.journalSupported = true;
        emu::scenario = [&] { nativeFlow(board); };
        runApp();
        assert(emu::expansion.enabled);
    }
    {
        Board board(true);
        cardWith({1, 2, 3});
        board.oldResult();
        emu::scenario = [&] {
            Board::redraw();
            tap(InputKeyUp);
            tap(InputKeyOk);
            screen("Checking status", "Waiting for board");
            board.advance(8);
            screen("Session ended", "0 of 1 captured", "Results");
            tap(InputKeyBack);
            screen("AirRounds", "Board status", "Access points", "Last result");
            tap(InputKeyOk);
            screen("Update Devboard", "Use newer board firmware");
            tap(InputKeyBack);
            tap(InputKeyUp);
            tap(InputKeyOk);
            screen("Session ended", "0 of 1 captured", "Results");
        };
        runApp();
    }
    scannedFlow();
    deadlineKeepsCompletedMiss();
    scanCancelShowsCleanupError();
    fivePointSelection();
    saveAfterCardReturns();
    staleProfileIsNotOverwritten();
    rejectedLegacyStartDoesNotNameAnotherPoint();
    rescanKeepsSelection();
    restartDuringSessionIsShown();
    receiveErrorDropsLink();
    slowSaveKeepsTheLink();
    storageAlternatesSlots();
    failedWriteKeepsTheLastProfile();
    writeIsReadBack();
    damagedRecordIsReplaced();
    unreadableCardBlocksSave();
    oversizedProfileLeavesTheCard();
    shortReadIsNotARecord();
    editProfilePoints();
    singlePointResults();
    radioFailureSkipsLaterPoints();
    noResultYet();
    scanHomeReopensSession();
    beaconLabels();
    beaconStatusNotes();
    stopKeepsBeaconFocus();
    failedRescanKeepsTheList();
    reviewHintFollowsTheLink();
    heldChoiceAfterLinkChange();
    heldStartAfterAnotherSession();
    uartHeldByAnotherApp();
    incompatibleBoard();
    lostStartReply();
    boardScreenAfterOpening();
    lastSessionNumber();
    legacyBoardKeepsTheAddress();
    scanStopWhileSaving();
    screenLayouts();
    scanLayouts();
    captureModeChoice();
    captureModeLockedDuringRound();
    captureModeEapolRound();
    freshReadIdsEachRun();
    openingShowsDeauthSessionOnce();
    backWhileStarting();
    rejectedCommands();
    restartDuringFirstSession();
    scanRestart();
    lostScanStart();
    anotherClientTakesTheSessionId();
    scanStopAfterTheVisit();
    retryAfterARound();
    beaconListChanges();
    beaconSelectionLimitAndFocus();
    addressEditorEvents();
    okTiming();
    resultsReturnToTheirScreen();
    boardFirmwareChanges();
    aboutShowsVersions();
    repliesSplitAcrossTicks();
    receiveBurstLimits();
    legacyLostStartReply();
    beaconNamesClip();
    restartFromBootOne();
    sessionAcrossTickWrap();
    idleLineIsIgnored();
    longBackInTheEditor();
    std::cout
        << "Native UI emulator: exact screens and layouts, profile editing and storage faults, "
           "scan lists, Stop, retry, rejections, link recovery, board restarts, legacy "
           "firmware, UART limits, and app exit passed\n";
}
