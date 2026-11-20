#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <new>

#include <expansion/expansion.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <gui/modules/byte_input.h>
#include <gui/view_dispatcher.h>

#include "../firmware/esp32/uart/capture_path.h"
#include "network_list.h"
#include "profile_storage.h"
#include "ui_state.h"

namespace {

using namespace airrounds; // The app uses the shared session and wire types.
using flipper::HomeEntry;
using flipper::Link;
using flipper::ProfileSave;
using flipper::Request;
using flipper::RunReport;
using uart::Outcome;
using uart::State;
using uart::VisitId;

enum class Page {
    home,
    scan,
    networks,
    placeholder,
    points,
    point,
    address,
    channel,
    remove,
    review,
    board,
    status,
    last,
    results,
    detail,
    file
};

enum class Action : uint8_t {
    none,
    scan,
    selectNetwork,
    placeholder,
    board,
    start,
    stop,
    refresh,
    points,
    point,
    add,
    edit,
    remove,
    confirmRemove,
    save,
    review,
    status,
    last,
    results,
    detail,
};

struct Binding {
    Action action = Action::none;
    size_t index = 0;
    VisitId visit{};

    bool matches(const Binding& other) const {
        return action == other.action && index == other.index && visit.matches(other.visit);
    }
};

struct Row {
    char left[27]{};
    char right[15]{};
};

struct Frame {
    char title[25]{};
    char hint[25]{};
    char titleRight[17]{};
    char leftKey[10]{};
    char rightKey[15]{};
    Row rows[4]{};
    size_t count = 0;
    int selected = -1;
    int column = 68;
    bool pointMenu = false;
    bool compact = false;
    bool scrollUp = false;
    bool scrollDown = false;
    bool fileLink = false;
    Binding binding{};

    void row(const char* left, const char* right = "") {
        if (count < sizeof(rows) / sizeof(rows[0])) {
            snprintf(rows[count].left, sizeof(rows[count].left), "%s", left);
            snprintf(rows[count].right, sizeof(rows[count].right), "%s", right);
            ++count;
        }
    }

    bool matches(const Frame& other) const {
        return strcmp(title, other.title) == 0 && strcmp(hint, other.hint) == 0 &&
               memcmp(rows, other.rows, sizeof(rows)) == 0 && count == other.count &&
               selected == other.selected && column == other.column &&
               pointMenu == other.pointMenu && compact == other.compact &&
               scrollUp == other.scrollUp && scrollDown == other.scrollDown &&
               fileLink == other.fileLink && strcmp(titleRight, other.titleRight) == 0 &&
               strcmp(leftKey, other.leftKey) == 0 && strcmp(rightKey, other.rightKey) == 0 &&
               binding.matches(other.binding);
    }
};

struct Display {
    Frame frame;
    Binding drawn;
};

struct App {
    App(Storage* storage, uint64_t nonce) : client(send, this, nonce), store(storage) {}

    static void send(void* context, const char* bytes, size_t count) {
        auto& app = *static_cast<App*>(context);
        furi_hal_serial_tx(app.serial, reinterpret_cast<const uint8_t*>(bytes), count);
    }

    flipper::SessionClient client;
    flipper::ProfileStorage store;
    ViewDispatcher* dispatcher = nullptr;
    View* view = nullptr;
    ByteInput* keyboard = nullptr;
    FuriHalSerialHandle* serial = nullptr;
    // Received bytes with the tick of their interrupt. A slow SD write can block the event loop
    // past the reply deadline, so the tick handler cannot tell when a byte arrived.
    FuriMessageQueue* rx = nullptr;
    std::atomic<bool> damaged{false};

    session::Profile profile;
    flipper::NetworkList networks;
    VisitId networkRun{};
    size_t networkFocus = 0;
    size_t placeholder = 0;
    Page scanOrigin = Page::home;
    size_t scanOriginFocus = 0;
    bool scanStarted = false;
    bool scanCancel = false;
    bool selectionLimit = false;
    bool networkLogFailed = false;
    session::Target draft{{}, 1};
    RunReport latest;
    RunReport selectedReport;
    session::Profile submitted;
    VisitId submittedId{};
    session::Target legacyTarget{};
    VisitId legacyTargetId{};
    VisitId acknowledgedStop{};
    VisitId confirmedId{};
    bool restarted = false;
    bool commandUnconfirmed = false;
    bool opening = true;
    uint32_t editorGeneration = 0;

    Page page = Page::home;
    Page editOrigin = Page::points;
    Page resultOrigin = Page::last;
    size_t focus = 0;
    size_t point = 0;
    size_t resultPoint = 0;
    ProfileSave saveResult = ProfileSave::saved;
    Binding held{};
};

const char* actionLabel(Action action) {
    switch (action) {
    case Action::scan:
        return "Scan";
    case Action::start:
        return "Start";
    case Action::stop:
        return "Stop";
    case Action::refresh:
        return "Refresh";
    case Action::save:
        return "Save";
    case Action::confirmRemove:
        return "Remove";
    case Action::results:
        return "Results";
    case Action::last:
        return "Last result";
    default:
        return nullptr;
    }
}

void title(Frame& frame, const char* name) {
    snprintf(frame.title, sizeof(frame.title), "%s", name);
}

void addressText(const session::Target& target, char (&text)[18]) {
    const auto* bssid = target.bssid;
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", static_cast<unsigned>(bssid[0]),
             static_cast<unsigned>(bssid[1]), static_cast<unsigned>(bssid[2]),
             static_cast<unsigned>(bssid[3]), static_cast<unsigned>(bssid[4]),
             static_cast<unsigned>(bssid[5]));
}

void pointText(size_t index, char (&text)[12]) {
    snprintf(text, sizeof(text), "Point %u", static_cast<unsigned>(index + 1));
}

void channelText(uint8_t channel, char (&text)[8]) {
    snprintf(text, sizeof(text), "Ch %u", static_cast<unsigned>(channel));
}

constexpr size_t visibleNetworkRows = 3;
constexpr const char* scanMenu[]{"Scan area", "Flash board hardware", "Test board", "About"};

size_t topRow(size_t focus) {
    return focus < visibleNetworkRows ? 0 : focus - visibleNetworkRows + 1;
}

void networkName(const App& app, const session::Target& target, size_t slot, char (&name)[25]) {
    const auto row = app.networks.find(target);
    if (row < app.networks.size()) {
        app.networks.label(row, name);
    } else {
        snprintf(name, sizeof(name), "Point %u", static_cast<unsigned>(slot + 1));
    }
}

bool canStart(const App& app) {
    return app.serial != nullptr && app.client.canStart(app.profile.size());
}

// A scan-capable board starts rounds of selected beacons, so the manual profile does not decide
// whether the status screens call the board ready.
bool readyToStart(const App& app) {
    return app.client.supportsScan() ? app.client.canStart(1) : canStart(app);
}

bool unconfirmedStart(const App& app) {
    return app.submittedId.session != 0 && !app.submittedId.matches(app.confirmedId);
}

RunReport currentRun(const App& app) {
    RunReport report;
    report.id = app.client.visitId();
    report.state = app.client.state();
    report.outcome = app.client.outcome();
    report.journal = app.client.journalState();
    report.round = app.client.round();
    if (report.round.profile.size() == 0 && report.id.session != 0 &&
        report.id.matches(app.legacyTargetId)) {
        report.round.profile.add(app.legacyTarget);
        report.round.results[0] = report.outcome;
    }
    return report;
}

void showOutcome(Frame& frame, const RunReport& report) {
    if (report.id.session == 0) {
        title(frame, "No result available");
        return;
    }
    title(frame, flipper::outcomeTitle(report));
    char count[32];
    snprintf(count, sizeof(count), "%u of %u captured", flipper::savedPoints(report),
             static_cast<unsigned>(flipper::reportSize(report)));
    frame.row(count);
    if (report.outcome == Outcome::storage_error) {
        frame.row("Check board SD");
    }
    if (report.journal == uart::JournalState::failed) {
        frame.row("Session log failed");
    }
    frame.binding = {Action::results, 0, report.id};
}

const char* boardIssue(const App& app) {
    if (app.serial == nullptr) {
        return "UART unavailable";
    }
    if (app.client.link() == Link::incompatible ||
        (app.client.link() == Link::ready && app.profile.size() > 1 &&
         !app.client.supportsRounds())) {
        return "Update Devboard";
    }
    if (app.client.link() == Link::lost) {
        return "Devboard offline";
    }
    if (app.client.link() == Link::ready) {
        return "Board unavailable";
    }
    return "Checking status";
}

void showNetworkProgress(Frame& frame, const App& app) {
    const auto report = currentRun(app);
    const auto slot = report.round.next();
    const auto total = report.round.profile.size();
    title(frame, "Capture");
    snprintf(frame.titleRight, sizeof(frame.titleRight), "Captured %u/%u",
             flipper::savedPoints(report), static_cast<unsigned>(total));
    frame.row(report.state == State::running ? "Listening" : "Saving");
    if (slot < total) {
        char name[25];
        networkName(app, report.round.profile[slot], slot, name);
        frame.row(name);
    }
    if (app.client.canStop() && !app.acknowledgedStop.matches(report.id)) {
        frame.binding = {Action::stop, 0, report.id};
    }
}

void showStatus(Frame& frame, const App& app) {
    const auto& client = app.client;
    if (app.serial == nullptr) {
        title(frame, "UART unavailable");
        frame.row("Close other UART apps");
    } else if (client.pending() == Request::start || client.pending() == Request::stop) {
        title(frame, client.pending() == Request::start ? "Starting..." : "Stopping...");
        frame.row("Waiting for board");
    } else if (client.link() == Link::lost) {
        const bool busy = flipper::sessionBusy(client.state()) || unconfirmedStart(app);
        title(frame, busy ? "Link lost" : "Devboard offline");
        if (busy) {
            frame.row("Session may run");
        }
        frame.row("Check cable");
    } else if (client.link() == Link::connecting) {
        title(frame, "Checking status");
        frame.row(app.commandUnconfirmed ? "Command unconfirmed" : "Waiting for board");
    } else if (client.link() == Link::incompatible) {
        title(frame, "Update Devboard");
        frame.row("AirRounds firmware");
    } else if (app.page == Page::board && !flipper::sessionBusy(client.state())) {
        title(frame, canStart(app) ? "Ready" : boardIssue(app));
        frame.row(client.supportsRounds() || app.profile.size() < 2 ? "Press Back for menu"
                                                                    : "Use newer board firmware");
    } else if (app.restarted && !flipper::sessionBusy(client.state())) {
        title(frame, "Devboard restarted");
        frame.row("Check board SD");
        if (app.latest.id.session != 0) {
            frame.binding = {Action::last, 0, app.latest.id};
        }
    } else if (client.rejected() != Request::none && client.state() != State::error) {
        title(frame, client.rejected() == Request::start ? "Start rejected" : "Stop rejected");
        frame.row("Session status updated");
        frame.binding = {Action::refresh, 0, client.visitId()};
    } else if (flipper::sessionBusy(client.state()) && client.supportsScan()) {
        showNetworkProgress(frame, app);
    } else if (flipper::sessionBusy(client.state())) {
        const auto report = currentRun(app);
        const auto slot = report.round.next();
        const bool laterPoints = slot + 1 < report.round.profile.size();
        const bool canStop = client.canStop() && !app.acknowledgedStop.matches(report.id);
        const bool pointSaving = client.state() == State::finalizing && laterPoints &&
                                 !app.acknowledgedStop.matches(report.id);
        title(frame, client.state() == State::running ? "Observing"
                     : pointSaving                    ? "Saving point"
                                                      : "Reception stopped");
        if (client.state() == State::finalizing && !pointSaving) {
            frame.row("Saving to board SD");
        } else if (slot < report.round.profile.size()) {
            char text[27];
            snprintf(text, sizeof(text), "Point %u / Ch %u", static_cast<unsigned>(slot + 1),
                     static_cast<unsigned>(report.round.profile[slot].channel));
            frame.row(text);
        } else {
            frame.row("Waiting for a beacon");
        }
        if (canStop) {
            frame.binding = {Action::stop, 0, report.id};
        }
    } else if (client.state() == State::idle) {
        title(frame, readyToStart(app) ? "Ready" : boardIssue(app));
        frame.row("Press Back for menu");
    } else {
        showOutcome(frame, currentRun(app));
    }
}

void showHome(Frame& frame, const App& app) {
    title(frame, "AirRounds");
    if (app.client.supportsScan()) {
        frame.compact = true;
        const bool busy = flipper::sessionBusy(app.client.state()) || unconfirmedStart(app);
        for (size_t row = 0; row < sizeof(scanMenu) / sizeof(scanMenu[0]); ++row) {
            frame.row(row == 0 && busy ? "Session status" : scanMenu[row]);
        }
        frame.selected = static_cast<int>(app.focus);
        const auto action =
            app.focus == 0 ? (busy ? Action::status : Action::scan) : Action::placeholder;
        frame.binding = {action, app.focus, {}};
        return;
    }
    const auto pending = app.client.pending();
    auto first = flipper::homeEntry(pending, app.client.state(), app.profile.size(), canStart(app));
    if (app.restarted || (unconfirmedStart(app) && app.client.link() != Link::ready)) {
        first = HomeEntry::status;
    }
    switch (first) {
    case HomeEntry::status:
        frame.row("Session status");
        break;
    case HomeEntry::setup:
        frame.row("Set up points");
        break;
    case HomeEntry::start:
        frame.row("Start session");
        break;
    case HomeEntry::board:
        frame.row("Board status");
        break;
    }
    frame.row("Access points");
    frame.row("Last result");
    frame.selected = static_cast<int>(app.focus);
    Action action = Action::last;
    if (app.focus == 0) {
        switch (first) {
        case HomeEntry::status:
            action = Action::status;
            break;
        case HomeEntry::setup:
            action = Action::points;
            break;
        case HomeEntry::start:
            action = Action::review;
            break;
        case HomeEntry::board:
            action = Action::board;
            break;
        }
    } else if (app.focus == 1) {
        action = Action::points;
    }
    frame.binding = {action, app.focus, {}};
}

void profileAction(Frame& frame, const App& app, const char* pageTitle, Action action) {
    if (app.saveResult == ProfileSave::changed) {
        title(frame, "Profile changed");
        snprintf(frame.hint, sizeof(frame.hint), "%s", "Reopen AirRounds");
    } else {
        title(frame, app.saveResult == ProfileSave::saved ? pageTitle : "Save not confirmed");
        frame.binding = {action, app.point, {}};
    }
}

void showScan(Frame& frame, const App& app) {
    if (app.scanStarted && app.client.scanState() == uart::ScanState::running) {
        title(frame, app.scanCancel ? "Canceling..." : "Scanning...");
        frame.row("Listening for networks");
        if (!app.scanCancel) {
            frame.row("Back to cancel");
        }
        return;
    }
    title(frame, app.scanCancel ? "Cancel failed" : "Scan failed");
    frame.row("Check Devboard");
    frame.binding = {Action::scan, 0, {}};
}

void showNetworks(Frame& frame, const App& app) {
    title(frame, "Beacons");
    frame.compact = true;
    const auto count = app.networks.size();
    const auto selected = static_cast<uint8_t>(app.networks.selection().size());
    snprintf(frame.titleRight, sizeof(frame.titleRight), "%u/5 selected",
             static_cast<unsigned>(selected));
    snprintf(frame.leftKey, sizeof(frame.leftKey), "Scan");
    if (selected != 0 && app.client.canStart(selected)) {
        snprintf(frame.rightKey, sizeof(frame.rightKey), "Start (%u)",
                 static_cast<unsigned>(selected));
    }
    if (count == 0) {
        frame.row("No networks found");
        return;
    }
    const size_t first = topRow(app.focus);
    frame.scrollUp = first > 0;
    frame.scrollDown = first + visibleNetworkRows < count;
    frame.selected = static_cast<int>(app.focus - first);
    for (size_t row = first; row < count && row < first + visibleNetworkRows; ++row) {
        const auto outcome = app.networks.outcome(row);
        char name[25];
        app.networks.label(row, name);
        char label[27];
        const auto* marker = outcome == Outcome::saved    ? "+ "
                             : app.networks.selected(row) ? "[x] "
                                                          : "[ ] ";
        snprintf(label, sizeof(label), "%s%.20s", marker, name);
        const char* status = outcome == Outcome::none ? ""
                             : outcome == Outcome::saved
                                 ? "Saved"
                                 : flipper::pointResult(outcome, State::complete);
        frame.row(label, status);
    }
    if (app.selectionLimit) {
        snprintf(frame.titleRight, sizeof(frame.titleRight), "Maximum 5");
    } else if (app.networkLogFailed) {
        snprintf(frame.titleRight, sizeof(frame.titleRight), "Log failed");
    } else if (app.client.link() != Link::ready) {
        snprintf(frame.titleRight, sizeof(frame.titleRight), "Board offline");
    }
    frame.binding = {Action::selectNetwork, app.focus, {}};
}

Frame makeFrame(const App& app) {
    Frame frame;
    switch (app.page) {
    case Page::home:
        showHome(frame, app);
        break;
    case Page::scan:
        showScan(frame, app);
        break;
    case Page::networks:
        showNetworks(frame, app);
        break;
    case Page::placeholder:
        title(frame, scanMenu[app.placeholder]);
        break;
    case Page::points:
        title(frame, "Access points");
        for (size_t i = 0; i < app.profile.size(); ++i) {
            char name[12];
            char channel[8];
            pointText(i, name);
            channelText(app.profile[i].channel, channel);
            frame.row(name, channel);
        }
        if (app.profile.size() < session::Profile::legacyCapacity) {
            frame.row("Add point");
        }
        frame.selected = static_cast<int>(app.focus);
        frame.binding = {
            app.focus == app.profile.size() ? Action::add : Action::point, app.focus, {}};
        break;
    case Page::point: {
        char name[12];
        char address[18];
        char channel[8];
        pointText(app.point, name);
        addressText(app.profile[app.point], address);
        channelText(app.profile[app.point].channel, channel);
        title(frame, name);
        frame.row(address, channel);
        frame.column = 101;
        frame.row("Edit");
        frame.row("Remove");
        frame.pointMenu = true;
        frame.selected = static_cast<int>(app.focus + 1);
        frame.binding = {app.focus == 0 ? Action::edit : Action::remove, app.point, {}};
        break;
    }
    case Page::channel: {
        char address[18];
        char channel[19];
        addressText(app.draft, address);
        profileAction(frame, app, "Select channel", Action::save);
        frame.row(address);
        snprintf(channel, sizeof(channel), "< Channel %u >",
                 static_cast<unsigned>(app.draft.channel));
        frame.row(channel);
        break;
    }
    case Page::remove: {
        char address[18];
        addressText(app.profile[app.point], address);
        profileAction(frame, app, "Remove point?", Action::confirmRemove);
        frame.row(address);
        break;
    }
    case Page::review:
        title(frame, "Review session");
        frame.column = 102;
        for (size_t i = 0; i < app.profile.size(); ++i) {
            char address[18];
            char line[22];
            char channel[8];
            addressText(app.profile[i], address);
            snprintf(line, sizeof(line), "%u %s", static_cast<unsigned>(i + 1), address);
            channelText(app.profile[i].channel, channel);
            frame.row(line, channel);
        }
        if (canStart(app)) {
            frame.binding = {Action::start, 0, app.client.visitId()};
        } else {
            snprintf(frame.hint, sizeof(frame.hint), "%s",
                     flipper::sessionBusy(app.client.state()) ? "Session in progress"
                                                              : boardIssue(app));
        }
        break;
    case Page::board:
    case Page::status:
        showStatus(frame, app);
        break;
    case Page::last:
        if (app.latest.id.session == 0 && app.client.link() == Link::connecting &&
            app.serial != nullptr) {
            title(frame, "Checking status");
            frame.row("Waiting for board");
        } else {
            showOutcome(frame, app.latest);
        }
        break;
    case Page::results: {
        title(frame, "Results");
        const auto count = flipper::reportSize(app.selectedReport);
        const size_t first = topRow(app.focus);
        for (size_t i = first; i < count && i < first + visibleNetworkRows; ++i) {
            char name[25];
            if (app.client.supportsScan() && i < app.selectedReport.round.profile.size()) {
                networkName(app, app.selectedReport.round.profile[i], i, name);
            } else {
                snprintf(name, sizeof(name), "Point %u", static_cast<unsigned>(i + 1));
            }
            frame.row(name, flipper::pointResult(flipper::pointOutcome(app.selectedReport, i),
                                                 app.selectedReport.state));
        }
        frame.compact = app.client.supportsScan();
        frame.scrollUp = first > 0;
        frame.scrollDown = first + visibleNetworkRows < count;
        frame.selected = static_cast<int>(app.focus - first);
        frame.binding = {Action::detail, app.focus, app.selectedReport.id};
        break;
    }
    case Page::detail: {
        char name[12];
        pointText(app.resultPoint, name);
        title(frame, name);
        const auto outcome = flipper::pointOutcome(app.selectedReport, app.resultPoint);
        frame.row(flipper::pointDetail(outcome, app.selectedReport.state));
        if (app.resultPoint < app.selectedReport.round.profile.size()) {
            const auto& target = app.selectedReport.round.profile[app.resultPoint];
            if (app.client.supportsScan()) {
                char label[25];
                networkName(app, target, app.resultPoint, label);
                frame.row(label);
            } else {
                char address[18];
                char channel[8];
                addressText(target, address);
                channelText(target.channel, channel);
                frame.row(address);
                frame.row(channel);
            }
        } else {
            frame.row("Address unavailable");
        }
        frame.fileLink = outcome == Outcome::saved;
        break;
    }
    case Page::file: {
        title(frame, "Board SD file");
        const auto& report = app.selectedReport;
        const auto slot =
            flipper::reportSize(report) == 1 ? 0U : static_cast<unsigned>(app.resultPoint + 1);
        char path[56];
        if (uart::capturePath(path, sizeof(path), "", {report.id, slot})) {
            // The file is at card root. Read the three lines as one filename.
            frame.row("visit-");
            char boot[18];
            snprintf(boot, sizeof(boot), "%.17s", path + 7);
            frame.row(boot);
            frame.row(path + 24);
        }
        break;
    }
    case Page::address:
        break;
    }
    return frame;
}

void draw(Canvas* canvas, void* model) {
    auto& display = *static_cast<Display*>(model);
    const auto& frame = display.frame;
    display.drawn = frame.binding;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, frame.title);
    canvas_set_font(canvas, FontSecondary);
    if (frame.titleRight[0] != '\0') {
        canvas_draw_str(canvas, 124 - canvas_string_width(canvas, frame.titleRight), 11,
                        frame.titleRight);
    }
    for (size_t i = 0; i < frame.count; ++i) {
        const bool selected = static_cast<int>(i) == frame.selected;
        const auto y = frame.compact         ? 22 + static_cast<int>(i) * 11
                       : frame.pointMenu     ? 24 + static_cast<int>(i) * 14
                       : frame.selected >= 0 ? 28 + static_cast<int>(i) * 16
                                             : 22 + static_cast<int>(i) * 11;
        if (selected) {
            elements_slightly_rounded_box(canvas, 1, y - (frame.compact ? 9 : 10),
                                          frame.compact ? 124 : 125, frame.compact ? 11 : 14);
            canvas_set_color(canvas, ColorWhite);
        }
        if (frame.compact) {
            char clipped[sizeof(Row::left)];
            snprintf(clipped, sizeof(clipped), "%s", frame.rows[i].left);
            const auto rightWidth = canvas_string_width(canvas, frame.rows[i].right);
            const auto rightX = 121 - rightWidth;
            const auto limit = rightWidth == 0 ? 118 : rightX - 9;
            for (size_t length = strlen(clipped);
                 length > 0 && canvas_string_width(canvas, clipped) > limit; --length) {
                clipped[length - 1] = '\0';
            }
            canvas_draw_str(canvas, 5, y, clipped);
            if (rightWidth != 0) {
                canvas_draw_str(canvas, rightX, y, frame.rows[i].right);
            }
        } else {
            canvas_draw_str(canvas, 5, y, frame.rows[i].left);
            canvas_draw_str(canvas, frame.column, y, frame.rows[i].right);
        }
        canvas_set_color(canvas, ColorBlack);
    }
    if (frame.hint[0] != '\0') {
        canvas_draw_str(canvas, 5, 61, frame.hint);
    }
    if (frame.selected < 0) {
        if (const char* label = actionLabel(frame.binding.action)) {
            elements_button_center(canvas, label);
        }
    }
    if (frame.scrollUp) {
        canvas_draw_str(canvas, 123, 25, "^");
    }
    if (frame.scrollDown) {
        canvas_draw_str(canvas, 123, 49, "v");
    }
    if (frame.leftKey[0] != '\0') {
        elements_button_left(canvas, frame.leftKey);
    }
    if (frame.rightKey[0] != '\0') {
        elements_button_right(canvas, frame.rightKey);
    } else if (frame.fileLink) {
        elements_button_right(canvas, "File");
    }
}

void paint(App& app, bool reset = false) {
    if (app.page == Page::address) {
        return;
    }
    const auto frame = makeFrame(app);
    auto& display = *static_cast<Display*>(view_get_model(app.view));
    const bool changed = reset || !display.frame.matches(frame);
    if (changed) {
        display.frame = frame;
    }
    if (reset) {
        display.drawn = {};
    }
    view_commit_model(app.view, changed);
}

constexpr uint32_t mainView = 0;
constexpr uint32_t addressView = 1;

void go(App& app, Page page, size_t focus = 0) {
    const bool switching = (app.page == Page::address) != (page == Page::address);
    app.page = page;
    app.focus = focus;
    app.held = {};
    if (page == Page::address) {
        app.editorGeneration = (app.editorGeneration + 1U) & 0x7fffffffU;
    }
    paint(app, true);
    if (switching) {
        view_dispatcher_switch_to_view(app.dispatcher,
                                       page == Page::address ? addressView : mainView);
    }
}

enum class EditorEvent : uint32_t { changed, confirmed };

void sendEditorEvent(App& app, EditorEvent event) {
    view_dispatcher_send_custom_event(app.dispatcher,
                                      (app.editorGeneration << 1U) | static_cast<uint32_t>(event));
}

void confirmAddress(void* context) {
    sendEditorEvent(*static_cast<App*>(context), EditorEvent::confirmed);
}

void addressChanged(void* context) {
    sendEditorEvent(*static_cast<App*>(context), EditorEvent::changed);
}

void edit(App& app, size_t index) {
    app.point = index;
    app.editOrigin = app.page;
    app.draft = index < app.profile.size() ? app.profile[index] : session::Target{{}, 1};
    app.saveResult = ProfileSave::saved;
    byte_input_set_header_text(app.keyboard, "Enter BSSID");
    byte_input_set_result_callback(app.keyboard, confirmAddress, addressChanged, &app,
                                   app.draft.bssid, sizeof(app.draft.bssid));
    go(app, Page::address);
}

bool editorEvent(void* context, uint32_t event) {
    auto& app = *static_cast<App*>(context);
    // ByteInput calls handlers with its model locked. Update it from the event loop.
    if (app.page != Page::address || (event >> 1U) != app.editorGeneration) {
        return true;
    }
    if ((event & 1U) == static_cast<uint32_t>(EditorEvent::changed)) {
        byte_input_set_header_text(app.keyboard, "Enter BSSID");
        return true;
    }
    if (!session::validTarget(app.draft)) {
        byte_input_set_header_text(app.keyboard, "Invalid BSSID");
        return true;
    }
    session::Profile next;
    if (!flipper::editProfile(app.profile, app.point, &app.draft, next)) {
        byte_input_set_header_text(app.keyboard, "Point already added");
        return true;
    }
    app.saveResult = ProfileSave::saved;
    go(app, Page::channel);
    return true;
}

void savePoint(App& app, const session::Target* target) {
    session::Profile updated;
    app.saveResult = flipper::editProfile(app.profile, app.point, target, updated)
                         ? app.store.save(app.profile, updated)
                         : ProfileSave::unavailable;
    if (app.saveResult != ProfileSave::saved) {
        paint(app);
        return;
    }
    app.profile = updated;
    const auto rows =
        updated.size() + static_cast<size_t>(updated.size() < session::Profile::legacyCapacity);
    go(app, Page::points, app.point < rows ? app.point : rows - 1);
}

void enterNetworks(App& app) {
    const auto count = app.networks.size();
    if (count == 0) {
        app.networkFocus = 0;
    } else if (app.networkFocus >= count) {
        app.networkFocus = count - 1;
    }
    go(app, Page::networks, app.networkFocus);
}

void scan(App& app, uint32_t now) {
    if (app.page != Page::scan) {
        app.scanOrigin = app.page;
        app.scanOriginFocus = app.focus;
    }
    app.scanStarted = app.client.scan(now);
    app.scanCancel = false;
    go(app, Page::scan);
}

void leaveScan(App& app) {
    app.scanStarted = false;
    if (app.scanOrigin == Page::networks) {
        enterNetworks(app);
    } else {
        go(app, Page::home, app.scanOriginFocus);
    }
}

void startCapture(App& app, const session::Profile& profile, uint32_t now) {
    if (!app.client.start(profile, now)) {
        return;
    }
    app.submitted = profile;
    app.submittedId = {app.client.visitId().boot, app.client.visitId().session + 1U};
    if (app.page == Page::networks) {
        app.networkRun = app.submittedId;
        app.networkLogFailed = false;
    }
    app.restarted = false;
    app.acknowledgedStop = {};
    go(app, Page::status);
}

void perform(App& app, const Binding& binding) {
    const uint32_t now = furi_get_tick();
    switch (binding.action) {
    case Action::scan:
        scan(app, now);
        break;
    case Action::selectNetwork:
        app.selectionLimit = !app.networks.toggle(binding.index);
        break;
    case Action::placeholder:
        app.placeholder = binding.index;
        go(app, Page::placeholder);
        break;
    case Action::start:
        startCapture(app, app.profile, now);
        break;
    case Action::stop:
        app.client.stop(binding.visit, now);
        break;
    case Action::refresh:
        app.client.refresh(now);
        break;
    case Action::review:
        go(app, Page::review);
        break;
    case Action::board:
        go(app, Page::board);
        break;
    case Action::status:
        go(app, Page::status);
        break;
    case Action::points:
        go(app, Page::points);
        break;
    case Action::point:
        app.point = binding.index;
        go(app, Page::point);
        break;
    case Action::add:
    case Action::edit:
        edit(app, binding.index);
        break;
    case Action::remove:
        app.saveResult = ProfileSave::saved;
        go(app, Page::remove);
        break;
    case Action::save:
        savePoint(app, &app.draft);
        break;
    case Action::confirmRemove:
        savePoint(app, nullptr);
        break;
    case Action::last:
        go(app, Page::last);
        break;
    case Action::results:
        app.resultOrigin = app.page;
        app.selectedReport = app.page == Page::last ? app.latest : currentRun(app);
        go(app, Page::results);
        break;
    case Action::detail:
        app.resultPoint = binding.index;
        go(app, Page::detail);
        break;
    case Action::none:
        break;
    }
}

bool input(InputEvent* event, void* context) {
    auto& app = *static_cast<App*>(context);
    if (event->key == InputKeyOk) {
        if (event->type == InputTypePress) {
            app.opening = false;
            const auto& display = *static_cast<Display*>(view_get_model(app.view));
            app.held = display.drawn;
            view_commit_model(app.view, false);
        } else if (event->type == InputTypeShort) {
            const auto held = app.held;
            app.held = {};
            if (held.action != Action::none && held.matches(makeFrame(app).binding)) {
                perform(app, held);
            }
            paint(app);
        } else if (event->type == InputTypeRelease) {
            app.held = {};
        }
        return true;
    }
    if (event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }
    if (event->key == InputKeyUp || event->key == InputKeyDown) {
        const auto frame = makeFrame(app);
        if (frame.selected >= 0) {
            app.opening = false;
            app.held = {};
            size_t count = frame.pointMenu ? 2U : frame.count;
            if (app.page == Page::networks) {
                count = app.networks.size();
            } else if (app.page == Page::results) {
                count = flipper::reportSize(app.selectedReport);
            }
            if (count != 0) {
                app.focus = (app.focus + (event->key == InputKeyDown ? 1U : count - 1U)) % count;
            }
            if (app.page == Page::networks) {
                app.networkFocus = app.focus;
                app.selectionLimit = false;
            }
            paint(app);
            return true;
        }
    }
    if (event->key == InputKeyLeft || event->key == InputKeyRight) {
        if (app.page == Page::networks && event->type == InputTypeShort) {
            app.held = {};
            const uint32_t now = furi_get_tick();
            if (event->key == InputKeyLeft) {
                scan(app, now);
            } else {
                startCapture(app, app.networks.selection(), now);
            }
            return true;
        }
        if (app.page == Page::channel) {
            app.held = {};
            const unsigned channel = app.draft.channel;
            app.draft.channel = static_cast<uint8_t>(
                (channel - 1U + (event->key == InputKeyRight ? 1U : 12U)) % 13U + 1U);
            paint(app);
            return true;
        }
        if (app.page == Page::detail && event->key == InputKeyRight &&
            flipper::pointOutcome(app.selectedReport, app.resultPoint) == Outcome::saved) {
            go(app, Page::file);
            return true;
        }
    }
    return false;
}

bool back(void* context) {
    auto& app = *static_cast<App*>(context);
    app.opening = false;
    switch (app.page) {
    case Page::home:
        return false;
    case Page::scan:
        if (app.scanStarted && app.client.cancelScan(furi_get_tick())) {
            app.scanCancel = true;
        } else {
            leaveScan(app);
        }
        break;
    case Page::placeholder:
        go(app, Page::home, app.placeholder);
        break;
    case Page::networks:
        go(app, Page::home);
        break;
    case Page::point:
        go(app, Page::points, app.point);
        break;
    case Page::address:
        go(app, app.editOrigin, app.editOrigin == Page::points ? app.point : 0);
        break;
    case Page::channel:
        go(app, Page::address);
        break;
    case Page::remove:
        go(app, Page::point, 1);
        break;
    case Page::results:
        go(app, app.resultOrigin);
        break;
    case Page::detail:
        go(app, Page::results, app.resultPoint);
        break;
    case Page::file:
        go(app, Page::detail);
        break;
    case Page::status:
        if (app.networkRun.session != 0) {
            if (flipper::sessionBusy(app.client.state()) || unconfirmedStart(app)) {
                return true;
            }
            enterNetworks(app);
        } else if (app.client.supportsScan() && app.networks.scanned() &&
                   !flipper::sessionBusy(app.client.state())) {
            enterNetworks(app);
        } else {
            app.restarted = false;
            go(app, Page::home);
        }
        break;
    default:
        go(app, Page::home);
    }
    return true;
}

void onRx(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    auto& app = *static_cast<App*>(context);
    constexpr unsigned errors = FuriHalSerialRxEventFrameError | FuriHalSerialRxEventNoiseError |
                                FuriHalSerialRxEventOverrunError | FuriHalSerialRxEventParityError;
    bool damaged = (event & errors) != 0;
    if ((event & FuriHalSerialRxEventData) != 0) {
        const uart::TimedByte byte{furi_hal_serial_async_rx(handle), furi_get_tick()};
        damaged = damaged || furi_message_queue_put(app.rx, &byte, 0) != FuriStatusOk;
    }
    if (damaged) {
        app.damaged.store(true, std::memory_order_relaxed);
    }
}

void updateRun(App& app, bool wasBusy) {
    const auto report = currentRun(app);
    if (app.confirmedId.boot != 0 && app.confirmedId.boot != report.id.boot) {
        app.restarted = wasBusy || unconfirmedStart(app);
        app.submittedId = {};
        app.legacyTargetId = {};
        app.acknowledgedStop = {};
        app.networkRun = {};
    }
    if (report.id.session != 0) {
        app.restarted = false;
    }
    app.confirmedId = report.id;
    app.submittedId = {};
    if (report.id.session != 0 && !flipper::sessionBusy(report.state)) {
        app.latest = report;
    }
    // A terminal Stop acknowledgment can arrive before the detailed status reply.
    const bool waitingForStopDetail = report.state == State::stopped &&
                                      app.client.supportsRoundDetail() &&
                                      report.round.interruption == uart::VisitInterruption::unknown;
    if (app.networkRun.session != 0 && report.id.matches(app.networkRun) &&
        report.round.profile.matches(app.submitted) && !waitingForStopDetail) {
        const auto missed = app.networks.record(report.round, report.state);
        if (!flipper::sessionBusy(report.state)) {
            app.networkLogFailed = report.journal == uart::JournalState::failed;
            app.networkRun = {};
            if (report.state != State::stopped && missed < app.networks.size()) {
                app.networkFocus = missed;
            }
            if (app.page == Page::status) {
                enterNetworks(app);
            }
        }
    }
    if (app.networkRun.session != 0 && report.id.matches(app.networkRun) &&
        !report.round.profile.matches(app.submitted)) {
        app.networkRun = {};
    }
    // updateRun runs only after a status, Start or Stop reply with no command left in flight. A
    // lost Start is never resent, and UART keeps request order. Once the board applies the Start,
    // every reply of this boot reports that session until the round ends, and the block above
    // records its result. Any other session means the Start never ran or the board moved past it
    // unseen, so no later reply can match the run.
    if (app.networkRun.session != 0 && report.id.boot == app.networkRun.boot &&
        report.id.session != app.networkRun.session) {
        app.networkRun = {};
    }
    if (app.opening) {
        app.opening = false;
        if (flipper::sessionBusy(report.state)) {
            go(app, Page::status);
        }
    }
}

void tick(void* context) {
    auto& app = *static_cast<App*>(context);
    if (app.serial != nullptr) {
        // Count the bytes before reading the clock, so no byte to handle is newer than now.
        const uint32_t count = furi_message_queue_get_count(app.rx);
        const uint32_t now = furi_get_tick();
        const bool wasBusy = flipper::sessionBusy(app.client.state());
        const auto pending = app.client.pending();
        const auto precedingLink = app.client.link();
        const bool damaged = app.damaged.exchange(false, std::memory_order_relaxed);
        if (damaged) {
            app.client.transportLost(now);
        }
        for (uint32_t i = 0; i < count; ++i) {
            uart::TimedByte byte{};
            if (furi_message_queue_get(app.rx, &byte, 0) == FuriStatusOk && !damaged) {
                app.client.receive(byte);
            }
        }
        if ((pending == Request::status || pending == Request::start || pending == Request::stop) &&
            app.client.pending() == Request::none && app.client.link() == Link::ready) {
            app.commandUnconfirmed = false;
            if (app.client.rejected() == Request::none) {
                const auto id = app.client.visitId();
                if (pending == Request::stop) {
                    app.acknowledgedStop = id;
                } else if (pending == Request::start && !app.client.supportsRounds() &&
                           id.matches(app.submittedId) && app.submitted.size() == 1) {
                    app.legacyTargetId = id;
                    app.legacyTarget = app.submitted[0];
                }
            }
            updateRun(app, wasBusy);
        }
        if (app.page == Page::scan && app.scanStarted) {
            if (app.client.scanState() == uart::ScanState::complete) {
                app.networks.replace(app.client.scanResults());
                app.scanStarted = false;
                app.selectionLimit = false;
                enterNetworks(app);
            } else if (app.client.scanState() == uart::ScanState::idle) {
                leaveScan(app);
            }
        }
        app.client.poll(now);
        if ((pending == Request::start || pending == Request::stop) &&
            app.client.link() == Link::lost && app.client.pending() != pending) {
            app.commandUnconfirmed = true;
        }
        if (precedingLink != app.client.link()) {
            app.held = {};
        }
    }
    paint(app);
}

} // namespace

extern "C" int32_t airrounds_app(void*) {
    uint64_t nonce = 0;
    furi_hal_random_fill_buf(reinterpret_cast<uint8_t*>(&nonce), sizeof(nonce));
    auto* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    auto* gui = static_cast<Gui*>(furi_record_open(RECORD_GUI));
    void* memory = malloc(sizeof(App));
    if (memory == nullptr) {
        furi_record_close(RECORD_GUI);
        furi_record_close(RECORD_STORAGE);
        return -1;
    }
    auto* app = new (memory) App(storage, nonce);
    app->profile = app->store.load();

    app->dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();
    app->keyboard = byte_input_alloc();
    view_set_context(app->view, app);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(Display));
    new (view_get_model(app->view)) Display{};
    view_commit_model(app->view, false);
    view_set_draw_callback(app->view, draw);
    view_set_input_callback(app->view, input);
    view_dispatcher_add_view(app->dispatcher, mainView, app->view);
    view_dispatcher_add_view(app->dispatcher, addressView, byte_input_get_view(app->keyboard));

    auto* expansion = static_cast<Expansion*>(furi_record_open(RECORD_EXPANSION));
    expansion_disable(expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if (app->serial != nullptr) {
        app->rx = furi_message_queue_alloc(512, sizeof(uart::TimedByte));
        furi_hal_serial_init(app->serial, 115200);
        furi_hal_serial_async_rx_start(app->serial, onRx, app, true);
    } else {
        expansion_enable(expansion);
    }
    view_dispatcher_attach_to_gui(app->dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, editorEvent);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, back);
    view_dispatcher_set_tick_event_callback(app->dispatcher, tick, furi_ms_to_ticks(50));
    paint(*app, true);
    view_dispatcher_switch_to_view(app->dispatcher, mainView);
    view_dispatcher_run(app->dispatcher);

    if (app->serial != nullptr) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
        expansion_enable(expansion);
        furi_message_queue_free(app->rx);
    }
    furi_record_close(RECORD_EXPANSION);
    view_dispatcher_remove_view(app->dispatcher, addressView);
    view_dispatcher_remove_view(app->dispatcher, mainView);
    byte_input_free(app->keyboard);
    view_free(app->view);
    view_dispatcher_free(app->dispatcher);
    app->~App();
    free(memory);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    return 0;
}
