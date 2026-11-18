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
    Row rows[3]{};
    size_t count = 0;
    int selected = -1;
    int column = 68;
    bool pointMenu = false;
    bool fileLink = false;
    Binding binding{};

    void row(const char* left, const char* right = "") {
        if (count < 3) {
            snprintf(rows[count].left, sizeof(rows[count].left), "%s", left);
            snprintf(rows[count].right, sizeof(rows[count].right), "%s", right);
            ++count;
        }
    }

    bool matches(const Frame& other) const {
        return strcmp(title, other.title) == 0 && strcmp(hint, other.hint) == 0 &&
               memcmp(rows, other.rows, sizeof(rows)) == 0 && count == other.count &&
               selected == other.selected && column == other.column &&
               pointMenu == other.pointMenu && fileLink == other.fileLink &&
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
    FuriStreamBuffer* rx = nullptr;
    std::atomic<bool> damaged{false};

    session::Profile profile;
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

bool canStart(const App& app) {
    return app.serial != nullptr && app.client.canStart(app.profile.size());
}

bool unconfirmedStart(const App& app) {
    return app.submittedId.session != 0 && !app.submittedId.matches(app.confirmedId);
}

RunReport currentRun(const App& app) {
    RunReport report;
    report.id = app.client.visitId();
    report.state = app.client.state();
    report.outcome = app.client.outcome();
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
    snprintf(count, sizeof(count), "%u of %u saved", flipper::savedPoints(report),
             static_cast<unsigned>(flipper::reportSize(report)));
    frame.row(count);
    if (report.outcome == Outcome::storage_error) {
        frame.row("Check board SD");
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
        title(frame, canStart(app) ? "Ready" : boardIssue(app));
        frame.row("Press Back for menu");
    } else {
        showOutcome(frame, currentRun(app));
    }
}

void showHome(Frame& frame, const App& app) {
    title(frame, "AirRounds");
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

Frame makeFrame(const App& app) {
    Frame frame;
    switch (app.page) {
    case Page::home:
        showHome(frame, app);
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
        if (app.profile.size() < session::Profile::capacity) {
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
    case Page::results:
        title(frame, "Results");
        for (size_t i = 0; i < flipper::reportSize(app.selectedReport); ++i) {
            char name[12];
            pointText(i, name);
            frame.row(name, flipper::pointResult(flipper::pointOutcome(app.selectedReport, i),
                                                 app.selectedReport.state));
        }
        frame.selected = static_cast<int>(app.focus);
        frame.binding = {Action::detail, app.focus, app.selectedReport.id};
        break;
    case Page::detail: {
        char name[12];
        pointText(app.resultPoint, name);
        title(frame, name);
        const auto outcome = flipper::pointOutcome(app.selectedReport, app.resultPoint);
        frame.row(flipper::pointDetail(outcome, app.selectedReport.state));
        if (app.resultPoint < app.selectedReport.round.profile.size()) {
            const auto& target = app.selectedReport.round.profile[app.resultPoint];
            char address[18];
            char channel[8];
            addressText(target, address);
            channelText(target.channel, channel);
            frame.row(address);
            frame.row(channel);
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
    for (size_t i = 0; i < frame.count; ++i) {
        const bool selected = static_cast<int>(i) == frame.selected;
        const auto y = frame.pointMenu       ? 24 + static_cast<int>(i) * 14
                       : frame.selected >= 0 ? 28 + static_cast<int>(i) * 16
                                             : 22 + static_cast<int>(i) * 11;
        if (selected) {
            elements_slightly_rounded_box(canvas, 1, y - 10, 125, 14);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 5, y, frame.rows[i].left);
        canvas_draw_str(canvas, frame.column, y, frame.rows[i].right);
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
    if (frame.fileLink) {
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
        updated.size() + static_cast<size_t>(updated.size() < session::Profile::capacity);
    go(app, Page::points, app.point < rows ? app.point : rows - 1);
}

void perform(App& app, const Binding& binding) {
    const uint32_t now = furi_get_tick();
    switch (binding.action) {
    case Action::start:
        if (app.client.start(app.profile, now)) {
            app.submitted = app.profile;
            app.submittedId = {binding.visit.boot, binding.visit.session + 1U};
            app.restarted = false;
            app.acknowledgedStop = {};
            go(app, Page::status);
        }
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
            const size_t count = frame.pointMenu ? 2U : frame.count;
            app.focus = (app.focus + (event->key == InputKeyDown ? 1U : count - 1U)) % count;
            paint(app);
            return true;
        }
    }
    if (event->key == InputKeyLeft || event->key == InputKeyRight) {
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
        app.restarted = false;
        go(app, Page::home);
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
        const uint8_t byte = furi_hal_serial_async_rx(handle);
        damaged = damaged || furi_stream_buffer_send(app.rx, &byte, 1, 0) != 1;
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
    }
    if (report.id.session != 0) {
        app.restarted = false;
    }
    app.confirmedId = report.id;
    app.submittedId = {};
    if (report.id.session != 0 && !flipper::sessionBusy(report.state)) {
        app.latest = report;
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
        const uint32_t now = furi_get_tick();
        uint8_t bytes[256];
        const size_t count = furi_stream_buffer_receive(app.rx, bytes, sizeof(bytes), 0);
        const bool wasBusy = flipper::sessionBusy(app.client.state());
        const auto pending = app.client.pending();
        const auto precedingLink = app.client.link();
        if (app.damaged.exchange(false, std::memory_order_relaxed)) {
            app.client.transportLost(now);
        } else {
            for (size_t i = 0; i < count; ++i) {
                app.client.receive({bytes[i], now});
            }
        }
        if (pending != Request::none && pending != Request::hello &&
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
        app->rx = furi_stream_buffer_alloc(512, 1);
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
        furi_stream_buffer_free(app->rx);
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
