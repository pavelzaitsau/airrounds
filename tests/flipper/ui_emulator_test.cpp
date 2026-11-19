#include <algorithm>
#include <iostream>

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
} // namespace emu

namespace {
using namespace airrounds::uart;

class Receiver final : public VisitBackend {
public:
    uint64_t nowUs() const override { return static_cast<uint64_t>(emu::clockMs) * 1000U; }
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
    void beginSave(const CaptureId&) override {}
    Outcome saveResult() const override { return outcome; }
    bool supportsJournal() const override { return journalSupported; }
    JournalState journalState() const override { return journal; }

    bool journalSupported = false;
    JournalState journal = JournalState::none;
    unsigned visits = 0;
    unsigned deadlineOnVisit = 0;
    bool observed = false;
    Outcome outcome = Outcome::none;
};

class Scanner final : public ScanBackend {
public:
    Scanner() {
        const char* names[]{"TELEPORT", "Studio", "Guest", "Loft", "Atlas", "Workshop"};
        for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
            const airrounds::session::Target target{
                {2, 0, 0, 0, 0, static_cast<uint8_t>(index + 1)}, 11};
            assert(list_.add(target, reinterpret_cast<const uint8_t*>(names[index]),
                             strlen(names[index])));
        }
    }

    void startScan() override { state_ = ScanState::running; }
    void pollScan() override {
        if (state_ == ScanState::running && completes) {
            state_ = ScanState::complete;
        }
    }
    void stopScan() override { state_ = cleanupFails ? ScanState::error : ScanState::idle; }
    ScanState scanState() const override { return state_; }
    const ScanList& scanList() const override { return list_; }

    bool completes = true;
    bool cleanupFails = false;

private:
    ScanList list_;
    ScanState state_ = ScanState::idle;
};

class Board {
public:
    Board(bool legacy = false, bool scan = false)
        : protocol_(receiver, 0x123456789abcdef0ULL, scan ? &scanner : nullptr), legacy_(legacy) {
        emu::transmit = [this](const uint8_t* data, size_t size) {
            if (online) {
                send(data, size);
            }
        };
    }

    void advance(size_t turns = 1) {
        for (size_t i = 0; i < turns; ++i) {
            emu::clockMs += 50;
            protocol_.poll();
            for (unsigned char byte : inbound_) {
                emu::serial.byte = byte;
                emu::serial.receive(&emu::serial, FuriHalSerialRxEventData, emu::serial.context);
            }
            inbound_.clear();
            emu::dispatcher->tick(emu::dispatcher->context);
        }
        redraw();
    }

    void disconnect() {
        online = false;
        inbound_.clear();
    }

    void oldResult() {
        const std::string command =
            "@01 123456789abcdef0 0000000000000001 00000001 start 020000000009 0b\n";
        send(reinterpret_cast<const uint8_t*>(command.data()), command.size());
        receiver.observed = true;
        receiver.outcome = Outcome::not_observed;
        protocol_.poll();
        inbound_.clear();
    }

    static void redraw() {
        auto* view = emu::dispatcher->views[emu::dispatcher->active];
        if (view->draw == nullptr) {
            emu::canvas.text = {emu::keyboard->heading};
            return;
        }
        view->draw(&emu::canvas, view_get_model(view));
        view_commit_model(view, false);
    }

    Receiver receiver;
    Scanner scanner;
    bool online = true;

private:
    void send(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            char reply[Protocol::replyCapacity];
            if (protocol_.receive({data[i], emu::clockMs}, reply)) {
                std::string wire = reply;
                if (legacy_ && wire.size() >= 4) {
                    wire.replace(wire.size() - 3, 2, "01");
                }
                inbound_ += wire;
            }
        }
    }

    Protocol protocol_;
    std::string inbound_;
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

void events() {
    auto* dispatcher = emu::dispatcher;
    while (!dispatcher->events.empty()) {
        const auto event = dispatcher->events.front();
        dispatcher->events.pop_front();
        dispatcher->custom(dispatcher->context, event);
    }
}

void key(InputKey button, InputType type) {
    auto* dispatcher = emu::dispatcher;
    auto* view = dispatcher->views[dispatcher->active];
    InputEvent event{button, type};
    const bool consumed = view->input != nullptr && view->input(&event, view->context);
    if (!consumed && button == InputKeyBack && type == InputTypeShort) {
        dispatcher->back(dispatcher->context);
    }
    events();
    Board::redraw();
}

void tap(InputKey button) {
    key(button, InputTypePress);
    key(button, InputTypeShort);
    key(button, InputTypeRelease);
}

void bssid(uint8_t suffix) {
    auto* keyboard = emu::keyboard;
    assert(emu::dispatcher->views[emu::dispatcher->active] == &keyboard->view);
    const uint8_t address[]{static_cast<uint8_t>(suffix == 0 ? 0 : 2), 0, 0, 0, 0, suffix};
    assert(keyboard->size == sizeof(address));
    memcpy(keyboard->buffer, address, sizeof(address));
    keyboard->view.locked = true;
    keyboard->saved(keyboard->context);
    keyboard->view.locked = false;
    events();
    Board::redraw();
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
    expect("Set up points");
    tap(InputKeyOk);
    tap(InputKeyOk);
    bssid(0);
    expect("Invalid BSSID");
    bssid(1);
    expect("Select channel");
    tap(InputKeyOk);
    tap(InputKeyDown);
    addPoint(2);
    tap(InputKeyDown);
    addPoint(3);
    tap(InputKeyBack);
    board.advance(8);
    expect("Start session");
    tap(InputKeyOk);
    expect("Review session");
    expect("3 02:00:00:00:00:03");

    // A held Start must not execute if the link changes before release.
    key(InputKeyOk, InputTypePress);
    board.disconnect();
    board.advance(32);
    key(InputKeyOk, InputTypeShort);
    key(InputKeyOk, InputTypeRelease);
    expect("Devboard offline");
    assert(board.receiver.visits == 0);
    board.online = true;
    board.advance(38);
    expect("Start");
    tap(InputKeyOk);
    expect("Starting...");
    board.advance();
    expect("Observing");

    board.receiver.observed = true;
    board.advance(12);
    expect("Saving point");
    tap(InputKeyOk);
    expect("Stopping...");
    board.disconnect(); // The board received Stop. Flipper did not receive its reply.
    board.receiver.outcome = Outcome::saved;
    board.receiver.journal = JournalState::failed;
    board.advance(22);
    expect("Link lost");
    expect("Session may run");
    assert(board.receiver.visits == 1);
    board.online = true;
    for (size_t turn = 0; turn < 36 && !shows("Checking status"); ++turn) {
        board.advance();
    }
    expect("Checking status");
    expect("Command unconfirmed");
    board.advance(10);
    expect("Session stopped");
    expect("1 of 3 captured");
    expect("Session log failed");
    tap(InputKeyOk);
    expect("Results");
    expect("Captured");
    expect("Stopped");
    tap(InputKeyOk);
    tap(InputKeyRight);
    expect("Board SD file");
    expect("00000001-1.pcap");
}

void scannedFlow() {
    Board board(false, true);
    emu::scenario = [&] {
        board.advance(8);
        expect("Scan area");
        expect("Flash board hardware");
        tap(InputKeyDown);
        tap(InputKeyOk);
        expect("Flash board hardware");
        assert(!shows("Scan area"));
        tap(InputKeyBack);
        tap(InputKeyUp);
        tap(InputKeyOk);
        expect("Scanning...");
        tap(InputKeyBack); // Request cancel before the start reply arrives.
        board.advance(20);
        expect("Scan area");
        tap(InputKeyOk);
        board.advance(60);
        expect("Beacons");
        tap(InputKeyOk);
        tap(InputKeyDown);
        tap(InputKeyOk);
        expect("Start (2)");
        tap(InputKeyRight);
        board.advance(10);
        expect("Listening");
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(12);
        expect("Captured 1/2");
        board.receiver.outcome = Outcome::not_observed;
        tap(InputKeyOk);
        board.advance(15);
        expect("Beacons");
        expect("Saved");
        expect("Start (1)");
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
        expect("Beacons");
        expect("Not seen");
        expect("Saved");
        assert(!shows("Ch 11") && !shows("02:00:00:00:00:01"));
        tap(InputKeyLeft);
        board.advance(60);
        expect("Not seen");
        expect("Saved");
        tap(InputKeyOk);
        expect("Start (1)");
        tap(InputKeyRight);
        board.advance(10);
        board.receiver.observed = true;
        board.receiver.outcome = Outcome::saved;
        board.advance(15);
        expect("Beacons");
        assert(!shows("Not seen"));
        assert(board.receiver.visits == 5);
    };
    assert(airrounds_app(nullptr) == 0);
}

void deadlineKeepsCompletedMiss() {
    Board board(false, true);
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
        expect("Beacons");
        expect("Not seen");
        expect("Start (1)");
    };
    assert(airrounds_app(nullptr) == 0);
}

void scanCancelShowsCleanupError() {
    Board board(false, true);
    board.scanner.completes = false;
    board.scanner.cleanupFails = true;
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(8);
        expect("Scanning...");
        tap(InputKeyBack);
        board.advance(15);
        expect("Cancel failed");
        expect("Check Devboard");
        tap(InputKeyBack);
        expect("Scan area");
    };
    assert(airrounds_app(nullptr) == 0);
}

void fivePointSelection() {
    Board board(false, true);
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        board.advance(60);
        for (unsigned row = 0; row < 5; ++row) {
            tap(InputKeyOk);
            tap(InputKeyDown);
        }
        tap(InputKeyOk);
        expect("Start (5)");
        assert(board.receiver.visits == 0);
        tap(InputKeyRight);
        board.advance(10);
        expect("Captured 0/5");
        assert(board.receiver.visits == 1);
    };
    assert(airrounds_app(nullptr) == 0);
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
    airrounds::flipper::ProfileStorage store(&emu::disk);
    const auto prior = store.load();
    assert(prior.size() == 3);
    emu::disk.failRead = true;
    emu::scenario = [&] {
        Board::redraw();
        expect("Set up points");
        tap(InputKeyOk);
        tap(InputKeyOk);
        bssid(7);
        expect("Select channel");
        emu::disk.failRead = false;
        tap(InputKeyOk);
        expect("Profile changed");
        assert(store.load().matches(prior));
    };
    assert(airrounds_app(nullptr) == 0);
}

void rejectedLegacyStartDoesNotNameAnotherPoint() {
    Board board(true);
    emu::disk.files.clear();
    airrounds::flipper::ProfileStorage store(&emu::disk);
    airrounds::session::Profile profile;
    assert(profile.add({{2, 0, 0, 0, 0, 7}, 6}));
    assert(store.save({}, profile) == airrounds::flipper::ProfileSave::saved);
    emu::scenario = [&] {
        board.advance(8);
        tap(InputKeyOk);
        expect("Review session");
        board.oldResult();
        tap(InputKeyOk);
        board.advance(10);
        expect("Start rejected");
        tap(InputKeyOk);
        board.advance(10);
        expect("Session ended");
        tap(InputKeyOk);
        tap(InputKeyOk);
        expect("Address unavailable");
    };
    assert(airrounds_app(nullptr) == 0);
}

} // namespace

int main() {
    {
        Board board;
        board.receiver.journalSupported = true;
        emu::scenario = [&] { nativeFlow(board); };
        assert(airrounds_app(nullptr) == 0);
        assert(emu::expansion.enabled);
    }
    {
        Board board(true);
        board.oldResult();
        emu::scenario = [&] {
            Board::redraw();
            tap(InputKeyUp);
            tap(InputKeyOk);
            expect("Checking status");
            board.advance(8);
            expect("Session ended");
            expect("0 of 1 captured");
            tap(InputKeyBack);
            expect("Board status");
            tap(InputKeyOk);
            expect("Update Devboard");
            tap(InputKeyBack);
            tap(InputKeyUp);
            tap(InputKeyOk);
            expect("Session ended");
            expect("0 of 1 captured");
        };
        assert(airrounds_app(nullptr) == 0);
    }
    scannedFlow();
    deadlineKeepsCompletedMiss();
    scanCancelShowsCleanupError();
    fivePointSelection();
    saveAfterCardReturns();
    staleProfileIsNotOverwritten();
    rejectedLegacyStartDoesNotNameAnotherPoint();
    std::cout << "Native UI emulator: scan cancellation and cleanup errors, five-point limit, "
                 "Stop, deadline, missed point retry, "
                 "legacy profiles, link recovery, and journal failure passed\n";
}
