#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "storage/storage.h"

#define RECORD_GUI "gui"
#define RECORD_STORAGE "storage"
#define RECORD_EXPANSION "expansion"

enum InputKey { InputKeyUp, InputKeyDown, InputKeyRight, InputKeyLeft, InputKeyOk, InputKeyBack };
enum InputType { InputTypePress, InputTypeRelease, InputTypeShort, InputTypeRepeat };
enum { FontPrimary, FontSecondary, ColorWhite, ColorBlack, ViewModelTypeLocking };
enum { ViewDispatcherTypeFullscreen, FuriHalSerialIdUsart };
enum : unsigned {
    FuriHalSerialRxEventData = 1,
    FuriHalSerialRxEventFrameError = 2,
    FuriHalSerialRxEventNoiseError = 4,
    FuriHalSerialRxEventOverrunError = 8,
    FuriHalSerialRxEventParityError = 16,
};
using FuriHalSerialRxEvent = unsigned;
struct InputEvent {
    InputKey key;
    InputType type;
};
struct Canvas {
    std::vector<std::string> text;
};
struct Gui {};
struct Expansion {
    bool enabled = true;
};
struct View {
    void* model = nullptr;
    void* context = nullptr;
    bool locked = false;
    void (*draw)(Canvas*, void*) = nullptr;
    bool (*input)(InputEvent*, void*) = nullptr;
};
struct ViewDispatcher {
    View* views[2]{};
    uint32_t active = 0;
    void* context = nullptr;
    bool (*back)(void*) = nullptr;
    bool (*custom)(void*, uint32_t) = nullptr;
    void (*tick)(void*) = nullptr;
    std::deque<uint32_t> events;
};
struct ByteInput {
    View view;
    void (*saved)(void*) = nullptr;
    void (*changed)(void*) = nullptr;
    void* context = nullptr;
    uint8_t* buffer = nullptr;
    size_t size = 0;
    std::string heading;
};
struct FuriHalSerialHandle {
    void (*receive)(FuriHalSerialHandle*, FuriHalSerialRxEvent, void*) = nullptr;
    void* context = nullptr;
    uint8_t byte = 0;
};
using FuriStreamBuffer = std::deque<uint8_t>;

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
} // namespace emu

inline uint32_t furi_get_tick() {
    return emu::clockMs;
}
inline uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}
inline void furi_hal_random_fill_buf(uint8_t* buffer, size_t size) {
    memset(buffer, 0x12, size);
}
inline void* furi_record_open(const char* name) {
    if (strcmp(name, RECORD_STORAGE) == 0) {
        return &emu::disk;
    }
    if (strcmp(name, RECORD_EXPANSION) == 0) {
        return &emu::expansion;
    }
    return &emu::gui;
}
inline void furi_record_close(const char*) {}
inline void expansion_disable(Expansion* expansion) {
    expansion->enabled = false;
}
inline void expansion_enable(Expansion* expansion) {
    expansion->enabled = true;
}
inline FuriHalSerialHandle* furi_hal_serial_control_acquire(int) {
    return emu::expansion.enabled ? nullptr : &emu::serial;
}
inline void furi_hal_serial_control_release(FuriHalSerialHandle*) {}
inline void furi_hal_serial_init(FuriHalSerialHandle*, int) {}
inline void furi_hal_serial_deinit(FuriHalSerialHandle*) {}
inline void furi_hal_serial_tx(FuriHalSerialHandle*, const uint8_t* data, size_t count) {
    emu::transmit(data, count);
}
inline void furi_hal_serial_async_rx_start(FuriHalSerialHandle* serial,
                                           void (*receive)(FuriHalSerialHandle*,
                                                           FuriHalSerialRxEvent, void*),
                                           void* context, bool) {
    serial->receive = receive;
    serial->context = context;
}
inline void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* serial) {
    serial->receive = nullptr;
}
inline uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* serial) {
    return serial->byte;
}

inline FuriStreamBuffer* furi_stream_buffer_alloc(size_t, size_t) {
    return new FuriStreamBuffer;
}
inline void furi_stream_buffer_free(FuriStreamBuffer* buffer) {
    delete buffer;
}
inline size_t furi_stream_buffer_send(FuriStreamBuffer* buffer, const void* bytes, size_t count,
                                      uint32_t) {
    const auto* data = static_cast<const uint8_t*>(bytes);
    buffer->insert(buffer->end(), data, data + count);
    return count;
}
inline size_t furi_stream_buffer_receive(FuriStreamBuffer* buffer, void* bytes, size_t count,
                                         uint32_t) {
    auto* data = static_cast<uint8_t*>(bytes);
    size_t index = 0;
    while (index < count && !buffer->empty()) {
        data[index++] = buffer->front();
        buffer->pop_front();
    }
    return index;
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
    assert(!view->locked);
    view->locked = true;
    return view->model;
}
inline void view_commit_model(View* view, bool) {
    assert(view->locked);
    view->locked = false;
}
inline void view_set_draw_callback(View* view, void (*draw)(Canvas*, void*)) {
    view->draw = draw;
}
inline void view_set_input_callback(View* view, bool (*input)(InputEvent*, void*)) {
    view->input = input;
}

inline ByteInput* byte_input_alloc() {
    return emu::keyboard = new ByteInput;
}
inline void byte_input_free(ByteInput* keyboard) {
    delete keyboard;
}
inline View* byte_input_get_view(ByteInput* keyboard) {
    return &keyboard->view;
}
inline void byte_input_set_header_text(ByteInput* keyboard, const char* heading) {
    assert(!keyboard->view.locked);
    keyboard->heading = heading;
}
inline void byte_input_set_result_callback(ByteInput* keyboard, void (*saved)(void*),
                                           void (*changed)(void*), void* context, uint8_t* bytes,
                                           size_t count) {
    assert(!keyboard->view.locked);
    keyboard->saved = saved;
    keyboard->context = context;
    keyboard->changed = changed;
    keyboard->buffer = bytes;
    keyboard->size = count;
}

inline ViewDispatcher* view_dispatcher_alloc() {
    return emu::dispatcher = new ViewDispatcher;
}
inline void view_dispatcher_free(ViewDispatcher* dispatcher) {
    delete dispatcher;
}
inline void view_dispatcher_attach_to_gui(ViewDispatcher*, Gui*, int) {}
inline void view_dispatcher_add_view(ViewDispatcher* dispatcher, uint32_t id, View* view) {
    dispatcher->views[id] = view;
}
inline void view_dispatcher_remove_view(ViewDispatcher* dispatcher, uint32_t id) {
    dispatcher->views[id] = nullptr;
}
inline void view_dispatcher_switch_to_view(ViewDispatcher* dispatcher, uint32_t id) {
    assert(!dispatcher->views[dispatcher->active]->locked);
    dispatcher->active = id;
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
                                                    void (*callback)(void*), uint32_t) {
    dispatcher->tick = callback;
}
inline void view_dispatcher_send_custom_event(ViewDispatcher* dispatcher, uint32_t event) {
    dispatcher->events.push_back(event);
}
inline void view_dispatcher_run(ViewDispatcher*) {
    emu::scenario();
}

inline void canvas_clear(Canvas* canvas) {
    canvas->text.clear();
}
inline void canvas_set_font(Canvas*, int) {}
inline void canvas_set_color(Canvas*, int) {}
inline void canvas_draw_str(Canvas* canvas, int, int, const char* text) {
    if (*text != '\0') {
        canvas->text.emplace_back(text);
    }
}
inline void elements_slightly_rounded_box(Canvas*, int, int, int, int) {}
inline void elements_button_center(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
}
inline uint16_t canvas_string_width(Canvas*, const char* text) {
    return static_cast<uint16_t>(strlen(text) * 5);
}
inline void elements_button_left(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
}
inline void elements_button_right(Canvas* canvas, const char* label) {
    canvas->text.emplace_back(label);
}
