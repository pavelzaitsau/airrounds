#include <atomic>
#include <cstdio>
#include <initializer_list>

#include <furi.h>
#include <furi_hal_random.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/view_dispatcher.h>

#include "session_client.h"
#include "visit_labels.h"

namespace {

using airrounds::flipper::Request;
using airrounds::flipper::SessionClient;
using airrounds::flipper::VisitLabels;
using airrounds::flipper::visitLabels;
using airrounds::session::Target;
using airrounds::uart::VisitId;

enum class Screen : uint32_t { menu, status, address, channel };

enum class StatusButton { none, refresh, stop };

struct StatusAction {
    StatusButton button;
    VisitId visit;
};

struct StatusView {
    VisitLabels labels;
    StatusAction action;
    StatusAction drawn;
};

struct StartView {
    uint8_t channel;
    bool canStart;
};

struct App {
    explicit App(uint64_t nonce) : client(send, this, nonce) {}

    static void send(void* context, const char* bytes, size_t count) {
        auto& app = *static_cast<App*>(context);
        furi_hal_serial_tx(app.serial, reinterpret_cast<const uint8_t*>(bytes), count);
    }

    SessionClient client;
    ViewDispatcher* dispatcher = nullptr;
    Submenu* menu = nullptr;
    TextInput* addressInput = nullptr;
    View* statusView = nullptr;
    View* channelView = nullptr;
    FuriHalSerialHandle* serial = nullptr;
    FuriStreamBuffer* rx = nullptr;
    std::atomic<bool> damaged{false};
    Screen screen = Screen::menu;
    char address[13]{};
    char addressDraft[13]{};
    StatusAction heldStatusAction{};
    uint8_t channel = 1;
};

void drawStatus(Canvas* canvas, void* model) {
    auto& state = *static_cast<StatusView*>(model);
    state.drawn = state.action;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 12, "AirRounds status");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 29, state.labels.state);
    canvas_draw_str(canvas, 3, 42, state.labels.detail);
    if (state.drawn.button != StatusButton::none) {
        elements_button_center(canvas,
                               state.drawn.button == StatusButton::stop ? "Stop" : "Refresh");
    }
}

void drawChannel(Canvas* canvas, void* model) {
    const auto& start = *static_cast<StartView*>(model);
    const unsigned channel = start.channel;
    char text[20];
    snprintf(text, sizeof(text), "<  Channel %u  >", channel);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 12, "Start visit");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 30, text);
    if (start.canStart) {
        elements_button_center(canvas, "Start");
    } else {
        canvas_draw_str(canvas, 3, 45, "Check ESP32 status");
    }
}

void showStatus(App& app) {
    const VisitLabels labels = app.serial != nullptr
                                   ? visitLabels(app.client)
                                   : VisitLabels{"UART unavailable", "Close other UART apps"};
    StatusAction action{};
    const auto pending = app.client.pending();
    if (app.serial != nullptr && pending != Request::start && pending != Request::stop) {
        if (app.client.canStop() && app.client.rejected() == Request::none) {
            action = {StatusButton::stop, app.client.visitId()};
        } else {
            action.button = StatusButton::refresh;
        }
    }
    auto& model = *static_cast<StatusView*>(view_get_model(app.statusView));
    const bool update =
        model.labels.state != labels.state || model.labels.detail != labels.detail ||
        model.action.button != action.button || !model.action.visit.matches(action.visit);
    model.labels = labels;
    model.action = action;
    view_commit_model(app.statusView, update);
}

void showChannel(App& app) {
    auto& model = *static_cast<StartView*>(view_get_model(app.channelView));
    const bool ready = app.client.canStart();
    const bool update = model.channel != app.channel || model.canStart != ready;
    model = {app.channel, ready};
    view_commit_model(app.channelView, update);
}

void go(App& app, Screen screen) {
    app.screen = screen;
    app.heldStatusAction = {};
    if (screen == Screen::status) {
        static_cast<StatusView*>(view_get_model(app.statusView))->drawn = {};
        view_commit_model(app.statusView, false);
        showStatus(app);
    }
    if (screen == Screen::channel) {
        showChannel(app);
    }
    if (screen == Screen::address) {
        memcpy(app.addressDraft, app.address, sizeof(app.address));
    }
    view_dispatcher_switch_to_view(app.dispatcher, static_cast<uint32_t>(screen));
}

bool targetFromText(const char* text, Target& target) {
    if (strlen(text) != 12) {
        return false;
    }
    for (size_t i = 0; i < sizeof(target.bssid); ++i) {
        char octet[2]{text[2 * i], text[2 * i + 1]};
        for (char& digit : octet) {
            if (digit >= 'A' && digit <= 'F') {
                digit = static_cast<char>(digit - 'A' + 'a');
            }
        }
        uint64_t value = 0;
        if (!airrounds::uart::Field{octet, sizeof(octet)}.hex(sizeof(octet), value)) {
            return false;
        }
        target.bssid[i] = static_cast<uint8_t>(value);
    }
    return airrounds::session::validTarget(target);
}

bool validateAddress(const char* text, FuriString* error, void*) {
    Target target{{}, 1};
    if (targetFromText(text, target)) {
        return true;
    }
    furi_string_set_str(error, "Enter 12 hex digits\nfor a valid BSSID");
    return false;
}

void confirmAddress(void* context) {
    auto& app = *static_cast<App*>(context);
    memcpy(app.address, app.addressDraft, sizeof(app.address));
    go(app, Screen::channel);
}

void selectMenu(void* context, uint32_t index) {
    auto& app = *static_cast<App*>(context);
    if (index == static_cast<uint32_t>(Screen::address)) {
        go(app, Screen::address);
        return;
    }
    app.client.refresh(furi_get_tick());
    go(app, Screen::status);
}

bool statusInput(InputEvent* event, void* context) {
    if (event->key != InputKeyOk) {
        return false;
    }
    auto& app = *static_cast<App*>(context);
    if (event->type == InputTypePress) {
        const auto& model = *static_cast<StatusView*>(view_get_model(app.statusView));
        // Keep the action and visit from the frame the user pressed on.
        app.heldStatusAction = model.drawn;
        view_commit_model(app.statusView, false);
    } else if (event->type == InputTypeRelease) {
        app.heldStatusAction = {};
    } else if (event->type == InputTypeShort) {
        const auto action = app.heldStatusAction;
        app.heldStatusAction = {};
        const uint32_t now = furi_get_tick();
        if (action.button == StatusButton::refresh ||
            (action.button == StatusButton::stop && !app.client.stop(action.visit, now))) {
            app.client.refresh(now);
        }
        showStatus(app);
    }
    return true;
}

bool channelInput(InputEvent* event, void* context) {
    if (event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }
    auto& app = *static_cast<App*>(context);
    switch (event->key) {
    case InputKeyLeft:
        app.channel = static_cast<uint8_t>(app.channel > 1 ? app.channel - 1 : 13);
        break;
    case InputKeyRight:
        app.channel = static_cast<uint8_t>(app.channel < 13 ? app.channel + 1 : 1);
        break;
    case InputKeyOk: {
        if (event->type != InputTypeShort) {
            return true;
        }
        Target target{{}, app.channel};
        if (targetFromText(app.address, target) && app.client.start(target, furi_get_tick())) {
            go(app, Screen::status);
        }
        return true;
    }
    default:
        return false;
    }
    showChannel(app);
    return true;
}

bool navigateBack(void* context) {
    auto& app = *static_cast<App*>(context);
    if (app.screen == Screen::menu) {
        return false;
    }
    go(app, app.screen == Screen::channel ? Screen::address : Screen::menu);
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

void tick(void* context) {
    auto& app = *static_cast<App*>(context);
    if (app.serial != nullptr) {
        const uint32_t now = furi_get_tick();
        uint8_t bytes[256];
        const size_t count = furi_stream_buffer_receive(app.rx, bytes, sizeof(bytes), 0);
        if (app.damaged.exchange(false, std::memory_order_relaxed)) {
            app.client.transportLost(now);
        } else {
            for (size_t i = 0; i < count; ++i) {
                app.client.receive({bytes[i], now});
            }
        }
        app.client.poll(now);
    }
    if (app.screen == Screen::status) {
        showStatus(app);
    } else if (app.screen == Screen::channel) {
        showChannel(app);
    }
}

void addViews(App& app) {
    app.menu = submenu_alloc();
    submenu_set_header(app.menu, "AirRounds");
    submenu_add_item(app.menu, "Status", static_cast<uint32_t>(Screen::status), selectMenu, &app);
    submenu_add_item(app.menu, "Start visit", static_cast<uint32_t>(Screen::address), selectMenu,
                     &app);

    app.addressInput = text_input_alloc();
    text_input_set_header_text(app.addressInput, "BSSID (12 hex digits)");
    text_input_set_result_callback(app.addressInput, confirmAddress, &app, app.addressDraft,
                                   sizeof(app.addressDraft), false);
    text_input_set_minimum_length(app.addressInput, 12);
    text_input_set_validator(app.addressInput, validateAddress, nullptr);

    app.statusView = view_alloc();
    view_set_context(app.statusView, &app);
    view_allocate_model(app.statusView, ViewModelTypeLocking, sizeof(StatusView));
    *static_cast<StatusView*>(view_get_model(app.statusView)) = {};
    view_commit_model(app.statusView, false);
    view_set_draw_callback(app.statusView, drawStatus);
    view_set_input_callback(app.statusView, statusInput);

    app.channelView = view_alloc();
    view_set_context(app.channelView, &app);
    view_allocate_model(app.channelView, ViewModelTypeLocking, sizeof(StartView));
    *static_cast<StartView*>(view_get_model(app.channelView)) = {};
    view_commit_model(app.channelView, false);
    view_set_draw_callback(app.channelView, drawChannel);
    view_set_input_callback(app.channelView, channelInput);

    view_dispatcher_add_view(app.dispatcher, static_cast<uint32_t>(Screen::menu),
                             submenu_get_view(app.menu));
    view_dispatcher_add_view(app.dispatcher, static_cast<uint32_t>(Screen::address),
                             text_input_get_view(app.addressInput));
    view_dispatcher_add_view(app.dispatcher, static_cast<uint32_t>(Screen::status), app.statusView);
    view_dispatcher_add_view(app.dispatcher, static_cast<uint32_t>(Screen::channel),
                             app.channelView);
}

void freeViews(App& app) {
    for (Screen screen : {Screen::menu, Screen::status, Screen::address, Screen::channel}) {
        view_dispatcher_remove_view(app.dispatcher, static_cast<uint32_t>(screen));
    }
    view_free(app.channelView);
    view_free(app.statusView);
    text_input_free(app.addressInput);
    submenu_free(app.menu);
}

} // namespace

extern "C" int32_t airrounds_app(void*) {
    uint64_t nonce = 0;
    furi_hal_random_fill_buf(reinterpret_cast<uint8_t*>(&nonce), sizeof(nonce));
    App app(nonce);
    app.dispatcher = view_dispatcher_alloc();
    app.serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if (app.serial != nullptr) {
        app.rx = furi_stream_buffer_alloc(512, 1);
        furi_hal_serial_init(app.serial, 115200);
        furi_hal_serial_async_rx_start(app.serial, onRx, &app, true);
    }
    auto* gui = static_cast<Gui*>(furi_record_open(RECORD_GUI));
    view_dispatcher_attach_to_gui(app.dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app.dispatcher, &app);
    view_dispatcher_set_navigation_event_callback(app.dispatcher, navigateBack);
    view_dispatcher_set_tick_event_callback(app.dispatcher, tick, furi_ms_to_ticks(50));
    addViews(app);
    go(app, Screen::menu);
    view_dispatcher_run(app.dispatcher);

    if (app.serial != nullptr) {
        furi_hal_serial_async_rx_stop(app.serial);
        furi_hal_serial_deinit(app.serial);
        furi_hal_serial_control_release(app.serial);
        furi_stream_buffer_free(app.rx);
    }
    freeViews(app);
    view_dispatcher_free(app.dispatcher);
    furi_record_close(RECORD_GUI);
    return 0;
}
