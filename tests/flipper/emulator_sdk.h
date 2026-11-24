#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "storage/storage.h"

// The stub checks how the app uses the SDK in every build, NDEBUG included.
#define EMU_REQUIRE(condition)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: SDK misuse: %s\n", __FILE__, __LINE__, #condition);       \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

#define RECORD_GUI "gui"
#define RECORD_STORAGE "storage"
#define RECORD_EXPANSION "expansion"

// The UI emulator force-includes this header, so the app guards out the on-device flash execution
// (the boot GPIO, a worker thread, and the ROM loader over the UART) and the test drives the
// stubbed flash through these flags.
#define AIRROUNDS_EMULATOR 1
namespace emu {
inline bool flashFinish = false;  // The test ends the stubbed flash.
inline bool flashSucceeds = true; // ... as a verified flash or a failure.
} // namespace emu

enum InputKey { InputKeyUp, InputKeyDown, InputKeyRight, InputKeyLeft, InputKeyOk, InputKeyBack };
enum InputType { InputTypePress, InputTypeRelease, InputTypeShort, InputTypeLong, InputTypeRepeat };
enum { FontPrimary, FontSecondary, ColorWhite, ColorBlack, ViewModelTypeLocking };
enum { ViewDispatcherTypeFullscreen, FuriHalSerialIdUsart };
enum : unsigned {
    FuriHalSerialRxEventData = 1,
    FuriHalSerialRxEventIdle = 2,
    FuriHalSerialRxEventFrameError = 4,
    FuriHalSerialRxEventNoiseError = 8,
    FuriHalSerialRxEventOverrunError = 16,
    FuriHalSerialRxEventParityError = 32,
};
using FuriHalSerialRxEvent = unsigned;
struct InputEvent {
    InputKey key;
    InputType type;
};
struct Canvas {
    // The visible strings in drawing order.
    std::vector<std::string> text;
    // Every visible drawing call with its position, font and color, for layout checks.
    std::vector<std::string> ops;
    int font = 0;
    int color = 0;
};
struct Gui {};
struct Expansion {
    bool enabled = true;
};
struct ViewDispatcher;
struct View {
    void* model = nullptr;
    void* context = nullptr;
    bool locked = false;
    void (*draw)(Canvas*, void*) = nullptr;
    bool (*input)(InputEvent*, void*) = nullptr;
    // The SDK sets the update callback when a dispatcher adds the view.
    ViewDispatcher* owner = nullptr;
};
struct ViewDispatcher {
    View* views[2]{};
    uint32_t active = 0;
    View* current = nullptr;
    // The GUI thread redraws only after a view port update. The SDK requests one when the
    // dispatcher switches views and when the current view commits its model with an update.
    bool redrawRequested = false;
    // The GUI draws a dispatcher only after the app attaches it.
    Gui* gui = nullptr;
    // view_dispatcher_run returns after view_dispatcher_stop. No later event reaches the app.
    bool stopped = false;
    // Keys down now, and the view that took the first of them. The SDK drops a Short, Long or
    // Repeat for a key that is not down, and sends a Release to that view after a view switch.
    unsigned ongoing = 0;
    View* ongoingView = nullptr;
    void* context = nullptr;
    bool (*back)(void*) = nullptr;
    bool (*custom)(void*, uint32_t) = nullptr;
    void (*tick)(void*) = nullptr;
    uint32_t tickPeriod = 0;
    std::deque<uint32_t> events;
};
struct ByteInput {
    View view;
    void (*saved)(void*) = nullptr;
    void (*changed)(void*) = nullptr;
    void* context = nullptr;
    uint8_t* buffer = nullptr;
    size_t size = 0;
    // Setting the callbacks resets the editor to the first byte on the key rows.
    size_t selected = 0;
    std::string heading;
};
struct FuriHalSerialHandle {
    void (*receive)(FuriHalSerialHandle*, FuriHalSerialRxEvent, void*) = nullptr;
    void* context = nullptr;
    bool acquired = false;
    uint32_t baud = 0;
    // The SDK enables the error interrupts only when the app asks for error reports.
    bool reportErrors = false;
    uint8_t byte = 0;
    bool byteReady = false;
};
enum FuriStatus { FuriStatusOk, FuriStatusErrorResource };
// Fixed-size messages, as the FreeRTOS queue behind the SDK keeps them.
struct FuriMessageQueue {
    std::deque<std::vector<uint8_t>> messages;
    uint32_t capacity = 0;
    uint32_t size = 0;
};

namespace emu {
extern uint32_t clockMs;
extern Storage disk;
extern Gui gui;
extern Expansion expansion;
extern Canvas canvas;
extern ViewDispatcher* dispatcher;
extern ByteInput* keyboard;
extern FuriHalSerialHandle serial;
extern std::function<void()> scenario;
extern std::function<void(const uint8_t*, size_t)> transmit;
// Another app holds the UART, so the app cannot acquire it.
extern bool serialBusy;

// Deliver one UART receive interrupt. The SDK sets FuriHalSerialRxEventData when a byte is
// waiting and the error flags when the UART saw a frame, noise, overrun or parity error.
// Both can arrive in one event.
// The HAL always enables the idle-line event. It enables the error events only on request.
inline void serialEvent(FuriHalSerialRxEvent event, uint8_t byte = 0) {
    EMU_REQUIRE(serial.receive != nullptr);
    if (!serial.reportErrors) {
        event &= FuriHalSerialRxEventData | FuriHalSerialRxEventIdle;
        if (event == 0) {
            return;
        }
    }
    serial.byte = byte;
    serial.byteReady = (event & FuriHalSerialRxEventData) != 0;
    serial.receive(&serial, event, serial.context);
    serial.byteReady = false;
}
} // namespace emu

inline uint32_t furi_get_tick() {
    return emu::clockMs;
}
inline uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}
namespace emu {
// The next byte of the random generator. Each scenario seeds it.
extern uint8_t random;
} // namespace emu
// Each call returns other bytes, as the hardware generator does.
inline void furi_hal_random_fill_buf(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = emu::random;
        emu::random = static_cast<uint8_t>(emu::random * 5U + 3U);
    }
}
namespace emu {
// Open record handles. The app must close every record it opens.
extern int openRecords;
} // namespace emu
inline void* furi_record_open(const char* name) {
    ++emu::openRecords;
    if (strcmp(name, RECORD_STORAGE) == 0) {
        return &emu::disk;
    }
    if (strcmp(name, RECORD_EXPANSION) == 0) {
        return &emu::expansion;
    }
    return &emu::gui;
}
inline void furi_record_close(const char*) {
    EMU_REQUIRE(emu::openRecords > 0);
    --emu::openRecords;
}
inline void expansion_disable(Expansion* expansion) {
    expansion->enabled = false;
}
inline void expansion_enable(Expansion* expansion) {
    expansion->enabled = true;
}
inline FuriHalSerialHandle* furi_hal_serial_control_acquire(int) {
    if (emu::expansion.enabled || emu::serialBusy || emu::serial.acquired) {
        return nullptr;
    }
    emu::serial.acquired = true;
    return &emu::serial;
}
inline void furi_hal_serial_control_release(FuriHalSerialHandle* serial) {
    EMU_REQUIRE(serial->acquired && serial->baud == 0 && serial->receive == nullptr);
    serial->acquired = false;
}
inline void furi_hal_serial_init(FuriHalSerialHandle* serial, uint32_t baud) {
    EMU_REQUIRE(serial->acquired);
    serial->baud = baud;
}
inline void furi_hal_serial_deinit(FuriHalSerialHandle* serial) {
    EMU_REQUIRE(serial->receive == nullptr);
    serial->baud = 0;
}
inline void furi_hal_serial_tx(FuriHalSerialHandle*, const uint8_t* data, size_t count) {
    emu::transmit(data, count);
}
inline void furi_hal_serial_async_rx_start(FuriHalSerialHandle* serial,
                                           void (*receive)(FuriHalSerialHandle*,
                                                           FuriHalSerialRxEvent, void*),
                                           void* context, bool reportErrors) {
    EMU_REQUIRE(serial->baud != 0);
    serial->reportErrors = reportErrors;
    serial->receive = receive;
    serial->context = context;
}
inline void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* serial) {
    serial->receive = nullptr;
}
inline uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* serial) {
    // The SDK reads the data register. Only a data event leaves a byte there.
    EMU_REQUIRE(serial->byteReady);
    serial->byteReady = false;
    return serial->byte;
}

inline FuriMessageQueue* furi_message_queue_alloc(uint32_t count, uint32_t size) {
    return new FuriMessageQueue{{}, count, size};
}
inline void furi_message_queue_free(FuriMessageQueue* queue) {
    delete queue;
}
// An interrupt must not wait, so a full queue refuses the message at once.
inline FuriStatus furi_message_queue_put(FuriMessageQueue* queue, const void* message,
                                         uint32_t timeout) {
    EMU_REQUIRE(timeout == 0);
    if (queue->messages.size() >= queue->capacity) {
        return FuriStatusErrorResource;
    }
    const auto* bytes = static_cast<const uint8_t*>(message);
    queue->messages.emplace_back(bytes, bytes + queue->size);
    return FuriStatusOk;
}
inline FuriStatus furi_message_queue_get(FuriMessageQueue* queue, void* message, uint32_t) {
    if (queue->messages.empty()) {
        return FuriStatusErrorResource;
    }
    memcpy(message, queue->messages.front().data(), queue->size);
    queue->messages.pop_front();
    return FuriStatusOk;
}
inline uint32_t furi_message_queue_get_count(FuriMessageQueue* queue) {
    return static_cast<uint32_t>(queue->messages.size());
}

inline View* view_alloc() {
    return new View;
}
inline void view_free(View* view) {
    free(view->model);
    delete view;
}
inline void view_set_context(View* view, void* context) {
    view->context = context;
}
inline void view_allocate_model(View* view, int, size_t size) {
    view->model = calloc(1, size);
}
inline void* view_get_model(View* view) {
    EMU_REQUIRE(!view->locked);
    view->locked = true;
    return view->model;
}
inline void view_commit_model(View* view, bool update) {
    EMU_REQUIRE(view->locked);
    view->locked = false;
    // The dispatcher ignores an update from a view that is not on screen.
    if (update && view->owner != nullptr && view->owner->current == view) {
        view->owner->redrawRequested = true;
    }
}
inline void view_set_draw_callback(View* view, void (*draw)(Canvas*, void*)) {
    view->draw = draw;
}
inline void view_set_input_callback(View* view, bool (*input)(InputEvent*, void*)) {
    view->input = input;
}

// ByteInput 1.5.1 consumes a Long or Repeat Back: it clears the selected byte, steps back one byte
// and reports the change. A Short Back on the key rows is not consumed, so it leaves the editor.
// The stub keeps the editor on the key rows.
inline bool byte_input_input(InputEvent* event, void* context) {
    auto* keyboard = static_cast<ByteInput*>(context);
    if (event->key != InputKeyBack ||
        (event->type != InputTypeLong && event->type != InputTypeRepeat)) {
        return false;
    }
    keyboard->view.locked = true;
    keyboard->buffer[keyboard->selected] = 0;
    if (keyboard->selected > 0) {
        --keyboard->selected;
    }
    keyboard->changed(keyboard->context);
    keyboard->view.locked = false;
    return true;
}
inline ByteInput* byte_input_alloc() {
    emu::keyboard = new ByteInput;
    emu::keyboard->view.input = byte_input_input;
    emu::keyboard->view.context = emu::keyboard;
    return emu::keyboard;
}
inline void byte_input_free(ByteInput* keyboard) {
    delete keyboard;
}
inline View* byte_input_get_view(ByteInput* keyboard) {
    return &keyboard->view;
}
inline void byte_input_update(ByteInput* keyboard) {
    // ByteInput changes its model with an update request, as the app's own view does.
    keyboard->view.locked = true;
    view_commit_model(&keyboard->view, true);
}
inline void byte_input_set_header_text(ByteInput* keyboard, const char* heading) {
    EMU_REQUIRE(!keyboard->view.locked);
    keyboard->heading = heading;
    byte_input_update(keyboard);
}
inline void byte_input_set_result_callback(ByteInput* keyboard, void (*saved)(void*),
                                           void (*changed)(void*), void* context, uint8_t* bytes,
                                           size_t count) {
    EMU_REQUIRE(!keyboard->view.locked);
    keyboard->saved = saved;
    keyboard->context = context;
    keyboard->changed = changed;
    keyboard->buffer = bytes;
    keyboard->size = count;
    keyboard->selected = 0;
    byte_input_update(keyboard);
}

inline ViewDispatcher* view_dispatcher_alloc() {
    return emu::dispatcher = new ViewDispatcher;
}
// The SDK crashes when a dispatcher still holds views.
inline void view_dispatcher_free(ViewDispatcher* dispatcher) {
    EMU_REQUIRE(dispatcher->views[0] == nullptr && dispatcher->views[1] == nullptr);
    delete dispatcher;
}
inline void view_dispatcher_attach_to_gui(ViewDispatcher* dispatcher, Gui* gui, int) {
    dispatcher->gui = gui;
}
inline void view_dispatcher_add_view(ViewDispatcher* dispatcher, uint32_t id, View* view) {
    dispatcher->views[id] = view;
    view->owner = dispatcher;
}
inline void view_dispatcher_remove_view(ViewDispatcher* dispatcher, uint32_t id) {
    if (dispatcher->current == dispatcher->views[id]) {
        dispatcher->current = nullptr;
    }
    dispatcher->views[id]->owner = nullptr;
    dispatcher->views[id] = nullptr;
}
inline void view_dispatcher_switch_to_view(ViewDispatcher* dispatcher, uint32_t id) {
    EMU_REQUIRE(!dispatcher->views[dispatcher->active]->locked);
    dispatcher->active = id;
    dispatcher->current = dispatcher->views[id];
    dispatcher->redrawRequested = true;
}
inline void view_dispatcher_set_event_callback_context(ViewDispatcher* dispatcher, void* context) {
    dispatcher->context = context;
}
inline void view_dispatcher_set_navigation_event_callback(ViewDispatcher* dispatcher,
                                                          bool (*callback)(void*)) {
    dispatcher->back = callback;
}
inline void view_dispatcher_set_custom_event_callback(ViewDispatcher* dispatcher,
                                                      bool (*callback)(void*, uint32_t)) {
    dispatcher->custom = callback;
}
inline void view_dispatcher_set_tick_event_callback(ViewDispatcher* dispatcher,
                                                    void (*callback)(void*), uint32_t period) {
    dispatcher->tick = callback;
    dispatcher->tickPeriod = period;
}
inline void view_dispatcher_send_custom_event(ViewDispatcher* dispatcher, uint32_t event) {
    dispatcher->events.push_back(event);
}
inline void view_dispatcher_stop(ViewDispatcher* dispatcher) {
    dispatcher->stopped = true;
}
inline void view_dispatcher_run(ViewDispatcher* dispatcher) {
    dispatcher->stopped = false;
    emu::scenario();
}

namespace emu {
// Deliver one input event as ViewDispatcher does. The current view gets it first. A short or long
// Back that the view does not consume goes to the navigation callback. When that callback returns
// false, the dispatcher stops and view_dispatcher_run returns.
inline void input(InputKey key, InputType type) {
    EMU_REQUIRE(!dispatcher->stopped);
    InputEvent event{key, type};
    const unsigned bit = 1U << key;
    if (type == InputTypePress) {
        dispatcher->ongoing |= bit;
    } else if (type == InputTypeRelease) {
        dispatcher->ongoing &= ~bit;
    } else if ((dispatcher->ongoing & bit) == 0) {
        return;
    }
    if ((dispatcher->ongoing & ~bit) == 0 && type == InputTypePress) {
        dispatcher->ongoingView = dispatcher->current;
    }
    View* view = dispatcher->current;
    if (view != nullptr && dispatcher->ongoingView == view) {
        const bool consumed = view->input != nullptr && view->input(&event, view->context);
        if (!consumed && key == InputKeyBack && (type == InputTypeShort || type == InputTypeLong) &&
            dispatcher->back != nullptr && !dispatcher->back(dispatcher->context)) {
            view_dispatcher_stop(dispatcher);
        }
    } else if (dispatcher->ongoingView != nullptr && type == InputTypeRelease &&
               dispatcher->ongoingView->input != nullptr) {
        dispatcher->ongoingView->input(&event, dispatcher->ongoingView->context);
    }
}

// Run queued custom events and one tick as the dispatcher's event loop does.
inline void customEvents() {
    while (!dispatcher->stopped && !dispatcher->events.empty()) {
        const auto event = dispatcher->events.front();
        dispatcher->events.pop_front();
        dispatcher->custom(dispatcher->context, event);
    }
}
inline void tick() {
    EMU_REQUIRE(!dispatcher->stopped);
    dispatcher->tick(dispatcher->context);
}
} // namespace emu

// The GUI resets the canvas before it calls a view's draw callback.
inline void canvas_reset(Canvas* canvas) {
    canvas->font = FontSecondary;
    canvas->color = ColorBlack;
}
inline void canvas_clear(Canvas* canvas) {
    canvas->text.clear();
    canvas->ops.clear();
}
inline void canvas_set_font(Canvas* canvas, int font) {
    canvas->font = font;
}
inline void canvas_set_color(Canvas* canvas, int color) {
    canvas->color = color;
}
// An empty string draws nothing, so the stub records only visible text.
inline void canvas_draw_str(Canvas* canvas, int x, int y, const char* text) {
    if (*text != '\0') {
        canvas->text.emplace_back(text);
        canvas->ops.push_back(std::string(canvas->font == FontPrimary ? "P" : "S") +
                              (canvas->color == ColorWhite ? "w " : "b ") + std::to_string(x) +
                              "," + std::to_string(y) + " " + text);
    }
}
inline void elements_slightly_rounded_box(Canvas* canvas, int x, int y, int width, int height) {
    canvas->ops.push_back("box " + std::to_string(x) + "," + std::to_string(y) + " " +
                          std::to_string(width) + "x" + std::to_string(height));
}
inline void elements_button_center(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
    canvas->ops.push_back(std::string("OK ") + label);
}
// A proportional test font: most glyphs are 5 px wide, '|' is 1 px and '`' is 4 px. The narrow
// glyphs let a test reach any clipping limit exactly.
inline uint16_t canvas_string_width(Canvas*, const char* text) {
    unsigned width = 0;
    for (; *text != '\0'; ++text) {
        width += *text == '|' ? 1U : *text == '`' ? 4U : 5U;
    }
    return static_cast<uint16_t>(width);
}
inline void elements_button_left(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
    canvas->ops.push_back(std::string("< ") + label);
}
inline void elements_button_right(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
    canvas->ops.push_back(std::string("> ") + label);
}
